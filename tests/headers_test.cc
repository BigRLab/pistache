#include <pistache/http_headers.h>
#include <pistache/date.h>

#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace Pistache::Http;

TEST(headers_test, accept) {
    Header::Accept a1;
    a1.parse("audio/*; q=0.2");

    {
        const auto& media = a1.media();
        ASSERT_EQ(media.size(), 1U);

        const auto& mime = media[0];
        ASSERT_EQ(mime, MIME(Audio, Star));
        ASSERT_EQ(mime.q().getOrElse(Mime::Q(0)), Mime::Q(20));
    }

    Header::Accept a2;
    a2.parse("text/*, text/html, text/html;level=1, */*");

    {
        const auto& media = a2.media();
        ASSERT_EQ(media.size(), 4U);

        const auto &m1 = media[0];
        ASSERT_EQ(m1, MIME(Text, Star));
        const auto &m2 = media[1];
        ASSERT_EQ(m2, MIME(Text, Html));
        const auto& m3 = media[2];
        ASSERT_EQ(m3, MIME(Text, Html));
        auto level = m3.getParam("level");
        ASSERT_EQ(level.getOrElse(""), "1");
        const auto& m4 = media[3];
        ASSERT_EQ(m4, MIME(Star, Star));
    }

    Header::Accept a3;
    a3.parse("text/*;q=0.3, text/html;q=0.7, text/html;level=1, "
             "text/html;level=2;q=0.4, */*;q=0.5");

    {
        const auto& media = a3.media();
        ASSERT_EQ(media.size(), 5U);

        ASSERT_EQ(media[0], MIME(Text, Star));
        ASSERT_EQ(media[0].q().getOrElse(Mime::Q(0)), Mime::Q(30));

        ASSERT_EQ(media[1], MIME(Text, Html));
        ASSERT_EQ(media[2], MIME(Text, Html));
        ASSERT_EQ(media[3], MIME(Text, Html));
        ASSERT_EQ(media[4], MIME(Star, Star));
        ASSERT_EQ(media[4].q().getOrElse(Mime::Q(0)), Mime::Q::fromFloat(0.5));
    }

    Header::Accept a4;
    ASSERT_THROW(a4.parse("text/*;q=0.4, text/html;q=0.3,"), std::runtime_error);

    Header::Accept a5;
    ASSERT_THROW(a5.parse("text/*;q=0.4, text/html;q=0.3, "), std::runtime_error);
}

TEST(headers_test, allow) {
    Header::Allow a1(Method::Get);

    std::ostringstream os;
    a1.write(os);
    ASSERT_EQ(os.str(), "GET");
    os.str("");

    Header::Allow a2({ Method::Post, Method::Put });
    a2.write(os);
    ASSERT_EQ(os.str(), "POST, PUT");
    os.str("");

    Header::Allow a3;
    a3.addMethod(Method::Get);
    a3.write(os);
    ASSERT_EQ(os.str(), "GET");
    os.str("");
    a3.addMethod(Method::Options);
    a3.write(os);
    ASSERT_EQ(os.str(), "GET, OPTIONS");
    os.str("");

    Header::Allow a4(Method::Head);
    a4.addMethods({ Method::Get, Method::Options });
    a4.write(os);
    ASSERT_EQ(os.str(), "HEAD, GET, OPTIONS");
}

TEST(headers_test, cache_control) {
    auto testTrivial = [](std::string str, CacheDirective::Directive expected) {
        Header::CacheControl cc;
        cc.parse(str);

        auto directives = cc.directives();
        ASSERT_EQ(directives.size(), 1U);
        ASSERT_EQ(directives[0].directive(), expected);
    };

    auto testTimed = [](
            std::string str, CacheDirective::Directive expected, uint64_t delta) {
        Header::CacheControl cc;
        cc.parse(str);

        auto directives = cc.directives();
        ASSERT_EQ(directives.size(), 1U);

        ASSERT_EQ(directives[0].directive(), expected);
        ASSERT_EQ(directives[0].delta(), std::chrono::seconds(delta));
    };

    testTrivial("no-cache", CacheDirective::NoCache);
    testTrivial("no-store", CacheDirective::NoStore);
    testTrivial("no-transform", CacheDirective::NoTransform);
    testTrivial("only-if-cached", CacheDirective::OnlyIfCached);

    testTimed("max-age=0", CacheDirective::MaxAge, 0);
    testTimed("max-age=12", CacheDirective::MaxAge, 12);

    testTimed("max-stale=12345", CacheDirective::MaxStale, 12345);
    testTimed("min-fresh=48", CacheDirective::MinFresh, 48);

    Header::CacheControl cc1;
    cc1.parse("private, max-age=600");
    auto d1 = cc1.directives();
    ASSERT_EQ(d1.size(), 2U);
    ASSERT_EQ(d1[0].directive(), CacheDirective::Private);
    ASSERT_EQ(d1[1].directive(), CacheDirective::MaxAge);
    ASSERT_EQ(d1[1].delta(), std::chrono::seconds(600));

    Header::CacheControl cc2;
    cc2.parse("public, s-maxage=200, proxy-revalidate");
    auto d2 = cc2.directives();
    ASSERT_EQ(d2.size(), 3U);
    ASSERT_EQ(d2[0].directive(), CacheDirective::Public);
    ASSERT_EQ(d2[1].directive(), CacheDirective::SMaxAge);
    ASSERT_EQ(d2[1].delta(), std::chrono::seconds(200));
    ASSERT_EQ(d2[2].directive(), CacheDirective::ProxyRevalidate);

    Header::CacheControl cc3(CacheDirective::NoCache);
    std::ostringstream oss;
    cc3.write(oss);
    ASSERT_EQ(oss.str(), "no-cache");
    oss.str("");

    cc3.addDirective(CacheDirective::NoStore);
    cc3.write(oss);
    ASSERT_EQ(oss.str(), "no-cache, no-store");
    oss.str("");

    Header::CacheControl cc4;
    cc4.addDirectives({
            CacheDirective::Public,
            CacheDirective(CacheDirective::MaxAge, std::chrono::seconds(600))
    });
    cc4.write(oss);
    ASSERT_EQ(oss.str(), "public, max-age=600");

}

TEST(headers_test, content_length) {
    Header::ContentLength cl;

    cl.parse("3495");
    ASSERT_EQ(cl.value(), 3495U);
}

TEST(headers_test, connection) {
    Header::Connection connection;

    constexpr struct Test {
        const char *data;
        ConnectionControl expected;
    } tests[] = {
      { "close", ConnectionControl::Close },
      { "clOse", ConnectionControl::Close },
      { "Close", ConnectionControl::Close },
      { "CLOSE", ConnectionControl::Close },

      { "keep-alive", ConnectionControl::KeepAlive },
      { "Keep-Alive", ConnectionControl::KeepAlive },
      { "kEEp-alIvE", ConnectionControl::KeepAlive },
      { "KEEP-ALIVE", ConnectionControl::KeepAlive }
    };

    for (auto test: tests) {
        Header::Connection connection;
        connection.parse(test.data);

        ASSERT_EQ(connection.control(), test.expected);
    }
}


TEST(headers_test, date_test_rfc_1123) {

    using namespace std::chrono;
    FullDate::time_point expected_time_point = date::sys_days(date::year{1994}/11/6)
                                                + hours(8) + minutes(49) + seconds(37);

    /* RFC-1123 */
    Header::Date d1;
    d1.parse("Sun, 06 Nov 1994 08:49:37 GMT");
    auto dd1 = d1.fullDate().date();
    ASSERT_EQ(expected_time_point, dd1);
}

TEST(headers_test, date_test_rfc_850) {

    using namespace std::chrono;
    FullDate::time_point expected_time_point = date::sys_days(date::year{1994}/11/6)
                                                + hours(8) + minutes(49) + seconds(37);

    /* RFC-850 */
    Header::Date d2;
    d2.parse("Sunday, 06-Nov-94 08:49:37 GMT");
    auto dd2 = d2.fullDate().date();
    ASSERT_EQ(dd2, expected_time_point);
}

TEST(headers_test, date_test_asctime) {

    using namespace std::chrono;
    FullDate::time_point expected_time_point = date::sys_days(date::year{1994}/11/6)
                                                + hours(8) + minutes(49) + seconds(37);

    /* ANSI C's asctime format */
    Header::Date d3;
    d3.parse("Sun Nov  6 08:49:37 1994");
    auto dd3 = d3.fullDate().date();
    ASSERT_EQ(dd3, expected_time_point);
}

TEST(headers_test, date_test_ostream) {

    std::ostringstream os;

    Header::Date d4;
    d4.parse("Fri, 25 Jan 2019 21:04:45.000000000 UTC");
    d4.write(os);
    ASSERT_EQ("Fri, 25 Jan 2019 21:04:45.000000000 UTC", os.str());
}

TEST(headers_test, host) {
    Header::Host host;

    host.parse("www.w3.org");
    ASSERT_EQ(host.host(), "www.w3.org");
    ASSERT_EQ(host.port(), 80);

    host.parse("www.example.com:8080");
    ASSERT_EQ(host.host(), "www.example.com");
    ASSERT_EQ(host.port(), 8080);

    host.parse("localhost:8080");
    ASSERT_EQ(host.host(), "localhost");
    ASSERT_EQ(host.port(), 8080);

/* Due to an error in GLIBC these tests don't fail as expected, further research needed */
//     ASSERT_THROW( host.parse("256.256.256.256:8080");, std::invalid_argument);
//     ASSERT_THROW( host.parse("1.0.0.256:8080");, std::invalid_argument);

    host.parse("[::1]:8080");
    ASSERT_EQ(host.host(), "[::1]");
    ASSERT_EQ(host.port(), 8080);

    host.parse("[2001:0DB8:AABB:CCDD:EEFF:0011:2233:4455]:8080");
    ASSERT_EQ(host.host(), "[2001:0DB8:AABB:CCDD:EEFF:0011:2233:4455]");
    ASSERT_EQ(host.port(), 8080);

/* Due to an error in GLIBC these tests don't fail as expected, further research needed */
//     ASSERT_THROW( host.parse("[GGGG:GGGG:GGGG:GGGG:GGGG:GGGG:GGGG:GGGG]:8080");, std::invalid_argument);
//     ASSERT_THROW( host.parse("[::GGGG]:8080");, std::invalid_argument);
}

TEST(headers_test, user_agent) {
    Header::UserAgent ua;

    ua.parse("CERN-LineMode/2.15 libwww/2.17b3");
    ASSERT_EQ(ua.agent(), "CERN-LineMode/2.15 libwww/2.17b3");
}

TEST(headers_test, content_encoding) {
    Header::ContentEncoding ce;

    ce.parse("gzip");
    ASSERT_EQ(ce.encoding(), Header::Encoding::Gzip);
}

TEST(headers_test, content_type) {
    Header::ContentType ct;

    ct.parse("text/html; charset=ISO-8859-4");
    const auto& mime = ct.mime();
    ASSERT_EQ(mime, MIME(Text, Html));
    ASSERT_EQ(mime.getParam("charset").getOrElse(""), "ISO-8859-4");
}

TEST(headers_test, access_control_allow_origin_test)
{
    Header::AccessControlAllowOrigin allowOrigin;

    allowOrigin.parse("http://foo.bar");
    ASSERT_EQ(allowOrigin.uri(), "http://foo.bar");
}

TEST(headers_test, access_control_allow_headers_test)
{
    Header::AccessControlAllowHeaders allowHeaders;

    allowHeaders.parse("Content-Type, Access-Control-Allow-Headers, Authorization, X-Requested-With");
    ASSERT_EQ(allowHeaders.val(), "Content-Type, Access-Control-Allow-Headers, Authorization, X-Requested-With");
}

TEST(headers_test, access_control_expose_headers_test)
{
    Header::AccessControlExposeHeaders exposeHeaders;

    exposeHeaders.parse("Accept, Location");
    ASSERT_EQ(exposeHeaders.val(), "Accept, Location");
}

TEST(headers_test, access_control_allow_methods_test)
{
    Header::AccessControlAllowMethods allowMethods;

    allowMethods.parse("GET, POST, DELETE");
    ASSERT_EQ(allowMethods.val(), "GET, POST, DELETE");
}

CUSTOM_HEADER(TestHeader)

TEST(header_test, macro_for_custom_headers)
{
    TestHeader testHeader;
    ASSERT_TRUE( strcmp(TestHeader::Name,"TestHeader") == 0);

    testHeader.parse("Header Content Test");
    ASSERT_EQ(testHeader.val(), "Header Content Test");
}

TEST(headers_test, add_new_header_test)
{
    const std::string headerName = "TestHeader";

    ASSERT_FALSE(Header::Registry::instance().isRegistered(headerName));
    Header::Registry::instance().registerHeader<TestHeader>();
    ASSERT_TRUE(Header::Registry::instance().isRegistered(headerName));

    const auto& headersList = Header::Registry::instance().headersList();
    const bool isFound = std::find(headersList.begin(), headersList.end(), headerName) != headersList.end();
    ASSERT_TRUE(isFound);
}
