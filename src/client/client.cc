/*
   Mathieu Stefani, 29 janvier 2016

   Implementation of the Http client
*/

#include <pistache/client.h>
#include <pistache/http.h>
#include <pistache/stream.h>
#include <pistache/net.h>

#include <sys/sendfile.h>
#include <netdb.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>


namespace Pistache {

using namespace Polling;

namespace Http {

static constexpr const char* UA = "pistache/0.1";

namespace
{
    std::pair<StringView, StringView> splitUrl(const std::string& url)
    {
        RawStreamBuf<char> buf(const_cast<char *>(&url[0]), url.size());
        StreamCursor cursor(&buf);

        match_string("http://", std::strlen("http://"), cursor);
        match_string("www", std::strlen("www"), cursor);
        match_literal('.', cursor);

        StreamCursor::Token hostToken(cursor);
        match_until({ '?', '/' }, cursor);

        StringView host(hostToken.rawText(), hostToken.size());
        StringView page(cursor.offset(), buf.endptr());

        return std::make_pair(std::move(host), std::move(page));
    }
}

struct ExceptionPrinter {
    void operator()(std::exception_ptr exc) const {
        try {
            std::rethrow_exception(exc);
        } catch (const std::exception& e) {
            std::cout << "Got exception: " << e.what() << std::endl;
        }
    }
};

namespace {
    template<typename H, typename... Args>
    void writeHeader(std::stringstream& streamBuf, Args&& ...args) {
        using Http::crlf;

        H header(std::forward<Args>(args)...);

        streamBuf << H::Name << ": ";
        header.write(streamBuf);

        streamBuf << crlf;
    }

    void writeHeaders(std::stringstream& streamBuf, const Http::Header::Collection& headers) {
        using Http::crlf;

        for (const auto& header: headers.list()) {
            streamBuf << header->name() << ": ";
            header->write(streamBuf);
            streamBuf << crlf;
        }
    }

    void writeCookies(std::stringstream& streamBuf, const Http::CookieJar& cookies) {
        using Http::crlf;

        streamBuf << "Cookie: ";
        bool first = true;
        for (const auto& cookie: cookies) {
            if (!first) {
                streamBuf << "; ";
            }
            else {
                first = false;
            }
            streamBuf << cookie.name << "=" << cookie.value;
        }

        streamBuf << crlf;
    }

    void writeRequest(std::stringstream& streamBuf, const Http::Request& request) {
        using Http::crlf;

        auto res = request.resource();
        auto s = splitUrl(res);
        auto body = request.body();

        auto host = s.first;
        auto path = s.second;

        auto pathStr = path.toString();

        streamBuf << request.method() << " ";
        if (pathStr[0] != '/')
            streamBuf << '/';
        streamBuf << pathStr;
        streamBuf << " HTTP/1.1" << crlf;

        writeCookies(streamBuf, request.cookies());
        writeHeaders(streamBuf, request.headers());

        writeHeader<Http::Header::UserAgent>(streamBuf, UA);
        writeHeader<Http::Header::Host>(streamBuf, host.toString());
        if (!body.empty()) {
            writeHeader<Http::Header::ContentLength>(streamBuf, body.size());
        }
        streamBuf << crlf;

        if (!body.empty()) {
            streamBuf << body;
        }
    }
}

void
Transport::onReady(const Aio::FdSet& fds) {
    for (const auto& entry: fds) {
        if (entry.getTag() == connectionsQueue.tag()) {
            handleConnectionQueue();
        }
        else if (entry.getTag() == requestsQueue.tag()) {
            handleRequestsQueue();
        }

        else if (entry.isReadable()) {
            auto tag = entry.getTag();
            auto fd = tag.value();
            auto connIt = connections.find(fd);
            if (connIt != std::end(connections)) {
                auto connection = connIt->second.connection.lock();
                if (connection) {
                    handleIncoming(connection);
                }
                else {
                    throw std::runtime_error("Connection error");
                }
            }
            else {
                auto timerIt = timeouts.find(fd);
                if (timerIt != std::end(timeouts))
                {
                    auto connection = timerIt->second.lock();
                    if (connection)
                    {
                        handleTimeout(connection);
                    }
                    else
                    {
                        throw std::runtime_error("Connection error: problem with handling timeout");
                    }
                }
                else
                {
                    throw std::runtime_error("Unknown fd");
                }
            }
        }
        else {
            auto tag = entry.getTag();
            auto fd = tag.value();
            auto connIt = connections.find(fd);
            if (connIt != std::end(connections)) {
                auto& conn = connIt->second;
                if (entry.isHangup())
                    conn.reject(Error::system("Could not connect"));
                else {
                    conn.resolve();
                    // We are connected, we can start reading data now
                    auto connection = connIt->second.connection.lock();
                    if (connection) {
                        reactor()->modifyFd(key(), connection->fd(), NotifyOn::Read);
                    } else {
                        throw std::runtime_error("Connection error");
                    }
                }
            } else {
                throw std::runtime_error("Unknown fd");
            }
        }
    }
}

void
Transport::registerPoller(Polling::Epoll& poller) {
    requestsQueue.bind(poller);
    connectionsQueue.bind(poller);
}

Async::Promise<void>
Transport::asyncConnect(std::shared_ptr<Connection> connection, const struct sockaddr* address, socklen_t addr_len)
{
    return Async::Promise<void>([=](Async::Resolver& resolve, Async::Rejection& reject) {
        ConnectionEntry entry(std::move(resolve), std::move(reject), connection, address, addr_len);
        connectionsQueue.push(std::move(entry));
    });
}

Async::Promise<ssize_t>
Transport::asyncSendRequest(
        std::shared_ptr<Connection> connection,
        std::shared_ptr<TimerPool::Entry> timer,
        std::string buffer) {

    return Async::Promise<ssize_t>([&](Async::Resolver& resolve, Async::Rejection& reject) {
        auto ctx = context();
        RequestEntry req(std::move(resolve), std::move(reject), connection, std::move(timer), std::move(buffer));
        if (std::this_thread::get_id() != ctx.thread()) {
            requestsQueue.push(std::move(req));
        } else {
            asyncSendRequestImpl(req);
        }
    });
}


void
Transport::asyncSendRequestImpl(
        const RequestEntry& req, WriteStatus status)
{
    const auto& buffer = req.buffer;
    auto conn = req.connection.lock();
    if (!conn)
        throw std::runtime_error("Send request error");

    auto fd = conn->fd();

    ssize_t totalWritten = 0;
    for (;;) {
        ssize_t bytesWritten = 0;
        ssize_t len = buffer.size() - totalWritten;
        auto ptr = buffer.data() + totalWritten;
        bytesWritten = ::send(fd, ptr, len, 0);
        if (bytesWritten < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (status == FirstTry) {
                    throw std::runtime_error("Unimplemented, fix me!");
                }
                reactor()->modifyFd(key(), fd, NotifyOn::Write, Polling::Mode::Edge);
            }
            else {
                req.reject(Error::system("Could not send request"));
            }
            break;
        }
        else {
            totalWritten += bytesWritten;
            if (totalWritten == len) {
                if (req.timer) {
                    timeouts.insert(
                          std::make_pair(req.timer->fd, conn));
                    req.timer->registerReactor(key(), reactor());
                }
                req.resolve(totalWritten);
                break;
            }
        }
    }
}

void
Transport::handleRequestsQueue() {
    // Let's drain the queue
    for (;;) {
        auto req = requestsQueue.popSafe();
        if (!req) break;

        asyncSendRequestImpl(*req);
    }
}

void
Transport::handleConnectionQueue() {
    for (;;) {
        auto data = connectionsQueue.popSafe();
        if (!data) break;

        auto conn = data->connection.lock();
        if (!conn) {
            throw std::runtime_error("Connection error");
        }
        int res = ::connect(conn->fd(), data->getAddr(), data->addr_len);
        if (res == -1) {
            if (errno == EINPROGRESS) {
                reactor()->registerFdOneShot(key(), conn->fd(), NotifyOn::Write | NotifyOn::Hangup | NotifyOn::Shutdown);
            }
            else {
                data->reject(Error::system("Failed to connect"));
                continue;
            }
        }
        connections.insert(std::make_pair(conn->fd(), std::move(*data)));
    }
}

void
Transport::handleIncoming(std::shared_ptr<Connection> connection) {
    char buffer[Const::MaxBuffer] = {0};

    ssize_t totalBytes = 0;

    for (;;) {
        ssize_t bytes = recv(connection->fd(), buffer + totalBytes, Const::MaxBuffer - totalBytes, 0);
        if (bytes == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (totalBytes > 0) {
                    handleResponsePacket(connection, buffer, totalBytes);
                }
            } else {
                connection->handleError(strerror(errno));
            }
            break;
        }
        else if (bytes == 0) {
            if (totalBytes > 0) {
                handleResponsePacket(connection, buffer, totalBytes);
            } else {
                connection->handleError("Remote closed connection");
            }
            connections.erase(connection->fd());
            connection->close();
            break;
        }

        else {
            totalBytes += bytes;
            if (static_cast<size_t>(totalBytes) > Const::MaxBuffer) {
                std::cerr << "Client: Too long packet" << std::endl;
                break;
            }
        }
    }
}

void
Transport::handleResponsePacket(const std::shared_ptr<Connection>& connection, const char* buffer, size_t totalBytes) {
    connection->handleResponsePacket(buffer, totalBytes);
}

void
Transport::handleTimeout(const std::shared_ptr<Connection>& connection) {
    connection->handleTimeout();
}

Connection::Connection()
    : fd_(-1)
    , requestEntry(nullptr)
{
    state_.store(static_cast<uint32_t>(State::Idle));
    connectionState_.store(NotConnected);
}

void
Connection::connect(const Address& addr)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = addr.family();
    hints.ai_socktype = SOCK_STREAM; /* Stream socket */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    const auto& host = addr.host();
    const auto& port = addr.port().toString();

    AddrInfo addressInfo;
    
    TRY(addressInfo.invoke(host.c_str(), port.c_str(), &hints));
    const addrinfo *addrs = addressInfo.get_info_ptr();
    
    int sfd = -1;

    for (const addrinfo *addr = addrs; addr; addr = addr->ai_next) {
        sfd = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (sfd < 0) continue;

        make_non_blocking(sfd);

        connectionState_.store(Connecting);
        fd_ = sfd;

        transport_->asyncConnect(shared_from_this(), addr->ai_addr, addr->ai_addrlen)
            .then([=]() {
                socklen_t len = sizeof(saddr);
                getsockname(sfd, (struct sockaddr *)&saddr, &len);
                connectionState_.store(Connected);
                processRequestQueue();
            }, ExceptionPrinter());
        break;

    }

    if (sfd < 0)
        throw std::runtime_error("Failed to connect");
}

std::string
Connection::dump() const {
    std::ostringstream oss;
    oss << "Connection(fd = " << fd_ << ", src_port = ";
    oss << ntohs(saddr.sin_port) << ")";
    return oss.str();
}

bool
Connection::isIdle() const {
    return static_cast<Connection::State>(state_.load()) == Connection::State::Idle;
}

bool
Connection::isConnected() const {
    return connectionState_.load() == Connected;
}

void
Connection::close() {
    connectionState_.store(NotConnected);
    ::close(fd_);
}

void
Connection::associateTransport(const std::shared_ptr<Transport>& transport) {
    if (transport_)
        throw std::runtime_error("A transport has already been associated to the connection");

    transport_ = transport;
}

bool
Connection::hasTransport() const {
    return transport_ != nullptr;
}

Fd Connection::fd() const
{
    assert(fd_ != -1);
    return fd_;
}

void
Connection::handleResponsePacket(const char* buffer, size_t totalBytes) {

    parser.feed(buffer, totalBytes);
    if (parser.parse() == Private::State::Done) {
        if (requestEntry) {
            if (requestEntry->timer) {
                requestEntry->timer->disarm();
                timerPool_.releaseTimer(requestEntry->timer);
            }

            requestEntry->resolve(std::move(parser.response));
            parser.reset();

            auto onDone = requestEntry->onDone;

            requestEntry.reset(nullptr);

            if (onDone)
                onDone();
        }
    }
}

void
Connection::handleError(const char* error) {
    if (requestEntry) {
        if (requestEntry->timer) {
            requestEntry->timer->disarm();
            timerPool_.releaseTimer(requestEntry->timer);
        }

        auto onDone = requestEntry->onDone;

        requestEntry->reject(Error(error));

        requestEntry.reset(nullptr);

        if (onDone)
            onDone();
    }
}

void
Connection::handleTimeout() {
    if (requestEntry) {
        timerPool_.releaseTimer(requestEntry->timer);

        auto onDone = requestEntry->onDone;

        /* @API: create a TimeoutException */
        requestEntry->reject(std::runtime_error("Timeout"));

        requestEntry.reset(nullptr);

        if (onDone)
            onDone();
    }
}


Async::Promise<Response>
Connection::perform(
        const Http::Request& request,
        std::chrono::milliseconds timeout,
        Connection::OnDone onDone) {
    return Async::Promise<Response>([=](Async::Resolver& resolve, Async::Rejection& reject) {
        performImpl(request, timeout, std::move(resolve), std::move(reject), std::move(onDone));
    });
}

Async::Promise<Response>
Connection::asyncPerform(
        const Http::Request& request,
        std::chrono::milliseconds timeout,
        Connection::OnDone onDone) {
    return Async::Promise<Response>([=](Async::Resolver& resolve, Async::Rejection& reject) {
        requestsQueue.push(
            RequestData(
                std::move(resolve),
                std::move(reject),
                request,
                timeout,
                std::move(onDone)));
    });
}

void
Connection::performImpl(
        const Http::Request& request,
        std::chrono::milliseconds timeout,
        Async::Resolver resolve,
        Async::Rejection reject,
        Connection::OnDone onDone) {

    std::stringstream streamBuf;
    writeRequest(streamBuf, request);
    if (!streamBuf)
        reject(std::runtime_error("Could not write request"));
    std::string buffer = streamBuf.str();

    std::shared_ptr<TimerPool::Entry> timer(nullptr);
    if (timeout.count() > 0) {
        timer = timerPool_.pickTimer();
        timer->arm(timeout);
    }

    auto rejectClone = reject.clone();

    requestEntry.reset(new RequestEntry(std::move(resolve), std::move(reject), timer, std::move(onDone)));
    transport_->asyncSendRequest(shared_from_this(), timer, std::move(buffer)).then(
        [](size_t /*bytes*/) {},
        [&](std::exception_ptr e) { rejectClone(e); });
}

void
Connection::processRequestQueue() {
    for (;;) {
        auto req = requestsQueue.popSafe();
        if (!req) break;

        performImpl(
                req->request,
                req->timeout, std::move(req->resolve), std::move(req->reject), std::move(req->onDone));
    }

}

void
ConnectionPool::init(size_t maxConnsPerHost) {
    maxConnectionsPerHost = maxConnsPerHost;
}

std::shared_ptr<Connection>
ConnectionPool::pickConnection(const std::string& domain) {
    Connections pool;

    {
        Guard guard(connsLock);
        auto poolIt = conns.find(domain);
        if (poolIt == std::end(conns)) {
            Connections connections;
            for (size_t i = 0; i < maxConnectionsPerHost; ++i) {
                connections.push_back(std::make_shared<Connection>());
            }

            poolIt = conns.insert(std::make_pair(domain, std::move(connections))).first;
        }
        pool = poolIt->second;
    }

    for (auto& conn: pool) {
        auto& state = conn->state_;
        auto curState = static_cast<uint32_t>(Connection::State::Idle);
        auto newState = static_cast<uint32_t>(Connection::State::Used);
        if (state.compare_exchange_strong(curState, newState)) {
            return conn;
        }
    }

    return nullptr;
}

void
ConnectionPool::releaseConnection(const std::shared_ptr<Connection>& connection) {
    connection->state_.store(static_cast<uint32_t>(Connection::State::Idle));
}

size_t
ConnectionPool::usedConnections(const std::string& domain) const {
    Connections pool;
    {
        Guard guard(connsLock);
        auto it = conns.find(domain);
        if (it == std::end(conns)) {
            return 0;
        }
        pool = it->second;
    }

    return std::count_if(pool.begin(), pool.end(), [](const std::shared_ptr<Connection>& conn) {
        return conn->isConnected();
    });
}

size_t
ConnectionPool::idleConnections(const std::string& domain) const {
    Connections pool;
    {
        Guard guard(connsLock);
        auto it = conns.find(domain);
        if (it == std::end(conns)) {
            return 0;
        }
        pool = it->second;
    }

    return std::count_if(pool.begin(), pool.end(), [](const std::shared_ptr<Connection>& conn) {
        return conn->isIdle();
    });
}

size_t
ConnectionPool::availableConnections(const std::string& domain) const {
    UNUSED(domain)
    return 0;
}

void
ConnectionPool::closeIdleConnections(const std::string& domain) {
    UNUSED(domain)
}

RequestBuilder&
RequestBuilder::method(Method method)
{
    request_.method_ = method;
    return *this;
}

RequestBuilder&
RequestBuilder::resource(const std::string& val) {
    request_.resource_ = val;
    return *this;
}

RequestBuilder&
RequestBuilder::params(const Uri::Query& query) {
    request_.query_ = query;
    return *this;
}

RequestBuilder&
RequestBuilder::header(const std::shared_ptr<Header::Header>& header) {
    request_.headers_.add(header);
    return *this;
}

RequestBuilder&
RequestBuilder::cookie(const Cookie& cookie) {
    request_.cookies_.add(cookie);
    return *this;
}

RequestBuilder&
RequestBuilder::body(const std::string& val) {
    request_.body_ = val;
    return *this;
}

RequestBuilder&
RequestBuilder::body(std::string&& val) {
    request_.body_ = std::move(val);
    return *this;
}

RequestBuilder&
RequestBuilder::timeout(std::chrono::milliseconds val) {
    timeout_ = val;
    return *this;
}

Async::Promise<Response>
RequestBuilder::send() {
    return client_->doRequest(request_, timeout_);
}

Client::Options&
Client::Options::threads(int val) {
    threads_ = val;
    return *this;
}

Client::Options&
Client::Options::keepAlive(bool val) {
    keepAlive_ = val;
    return *this;
}

Client::Options&
Client::Options::maxConnectionsPerHost(int val) {
    maxConnectionsPerHost_ = val;
    return *this;
}

Client::Client()
    : reactor_(Aio::Reactor::create())
    , pool()
    , transportKey()
    , ioIndex(0)
    , queuesLock()
    , requestsQueues()
    , stopProcessPequestsQueues(false)
{ }

Client::~Client() {
    assert(stopProcessPequestsQueues == true && "You must explicitly call shutdown method of Client object");
}

Client::Options
Client::options() {
    return Client::Options();
}

void
Client::init(const Client::Options& options) {
    pool.init(options.maxConnectionsPerHost_);
    reactor_->init(Aio::AsyncContext(options.threads_));
    transportKey = reactor_->addHandler(std::make_shared<Transport>());
    reactor_->run();
}

void
Client::shutdown() {
    reactor_->shutdown();
    Guard guard(queuesLock);
    stopProcessPequestsQueues = true;
}

RequestBuilder
Client::get(const std::string& resource)
{
    return prepareRequest(resource, Http::Method::Get);
}

RequestBuilder
Client::post(const std::string& resource)
{
    return prepareRequest(resource, Http::Method::Post);
}

RequestBuilder
Client::put(const std::string& resource)
{
    return prepareRequest(resource, Http::Method::Put);
}

RequestBuilder
Client::patch(const std::string& resource)
{
    return prepareRequest(resource, Http::Method::Patch);
}

RequestBuilder
Client::del(const std::string& resource)
{
    return prepareRequest(resource, Http::Method::Delete);
}

RequestBuilder
Client::prepareRequest(const std::string& resource, Http::Method method)
{
    RequestBuilder builder(this);
    builder
        .resource(resource)
        .method(method);

    return builder;
}

Async::Promise<Response>
Client::doRequest(
        Http::Request request,
        std::chrono::milliseconds timeout)
{
    //request.headers_.add<Header::Connection>(ConnectionControl::KeepAlive);
    request.headers_.remove<Header::UserAgent>();
    auto resource = request.resource();

    auto s = splitUrl(resource);
    auto conn = pool.pickConnection(s.first);

    if (conn == nullptr) {
        // TODO: C++14 - use capture move for s
        return Async::Promise<Response>([this, s, request, timeout](Async::Resolver& resolve, Async::Rejection& reject) {
            Guard guard(queuesLock);

            auto data = std::make_shared<Connection::RequestData>(std::move(resolve), std::move(reject), request, timeout, nullptr);
            auto& queue = requestsQueues[s.first];
            if (!queue.enqueue(data))
                data->reject(std::runtime_error("Queue is full"));
        });
    }
    else {

        if (!conn->hasTransport()) {
            auto transports = reactor_->handlers(transportKey);
            auto index = ioIndex.fetch_add(1) % transports.size();

            auto transport = std::static_pointer_cast<Transport>(transports[index]);
            conn->associateTransport(transport);

        }

        if (!conn->isConnected()) {
            auto res = conn->asyncPerform(request, timeout, [this, conn]() {
                pool.releaseConnection(conn);
                processRequestQueue();
            });
            conn->connect(helpers::httpAddr(s.first));
            return res;
        }

        return conn->perform(request, timeout, [this, conn]() {
            pool.releaseConnection(conn);
            processRequestQueue();
        });
    }
}

void
Client::processRequestQueue() {
    Guard guard(queuesLock);

    if (stopProcessPequestsQueues)
        return;

    for (auto& queues: requestsQueues) {
        for (;;) {
            const auto& domain = queues.first;
            auto conn = pool.pickConnection(domain);
            if (!conn)
                break;

            auto& queue = queues.second;
            std::shared_ptr<Connection::RequestData> data;
            if (!queue.dequeue(data)) {
                pool.releaseConnection(conn);
                break;
            }

            conn->performImpl(
                    data->request,
                    data->timeout,
                    std::move(data->resolve), std::move(data->reject),
                    [this, conn]() {
                        pool.releaseConnection(conn);
                        processRequestQueue();
                    });
        }
    }
}

} // namespace Http
} // namespace Pistache
