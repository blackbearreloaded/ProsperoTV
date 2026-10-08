/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_http.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace
{

std::string Describe(iptv::http::Status status, int http_status = 0, int native_error = 0,
                     const char *response = nullptr)
{
    char text[192]{};
    iptv::http::DescribeFailure(status, http_status, native_error, response, text, sizeof(text));
    return text;
}

TEST(IptvHttpTest, DetectsGeoIpBlockCaseInsensitively)
{
    constexpr char response[] = "This channel is GEOIP BLOCKED in your region";
    EXPECT_TRUE(iptv::http::ResponseIndicatesGeographicBlock(response, std::strlen(response)));
    EXPECT_EQ(Describe(iptv::http::Status::http_status_error, 403, 0, response),
              "unavailable in your region (GeoIP blocked; HTTP 403)");
}

TEST(IptvHttpTest, RedirectsKeepPortalCredentialsOnlyOnTheOriginalOrigin)
{
    const iptv::http::RequestHeaders headers{"ProsperoTV", "https://portal.invalid/c/",
                                             "mac=00:11:22:33:44:55", "Bearer token"};
    for (const auto target : {"https://portal.invalid/other", "HTTPS://PORTAL.invalid:443/other"})
    {
        const auto kept =
            iptv::http::HeadersForUrl("https://portal.invalid/server/load.php", target, headers);
        EXPECT_EQ(kept.cookie, headers.cookie);
        EXPECT_EQ(kept.authorization, headers.authorization);
    }
    for (const auto target :
         {"http://portal.invalid/other", "https://cdn.invalid/stream",
          "https://portal.invalid:444/stream", "https://portal.invalid.evil/", "file:///other"})
    {
        const auto kept =
            iptv::http::HeadersForUrl("https://portal.invalid/server/load.php", target, headers);
        EXPECT_EQ(kept.cookie, nullptr);
        EXPECT_EQ(kept.authorization, nullptr);
        EXPECT_EQ(kept.user_agent, headers.user_agent);
    }
}

TEST(IptvHttpTest, DoesNotGuessThatEveryForbiddenResponseIsGeographic)
{
    EXPECT_EQ(Describe(iptv::http::Status::http_status_error, 403, 0, "Forbidden"),
              "access denied by the channel provider (HTTP 403)");
}

TEST(IptvHttpTest, ValidatesTheByteRangeBeforeSeekingInAMovie)
{
    std::int64_t size = -1;
    EXPECT_TRUE(iptv::http::ParseStreamRange("Content-Length: 9000000000\r\n", 200, 0, &size));
    EXPECT_EQ(size, 9000000000);
    EXPECT_FALSE(iptv::http::ParseStreamRange("Content-Length: 1000\r\n", 200, 200, &size));
    EXPECT_TRUE(iptv::http::ParseStreamRange(
        "content-range: bytes 200-999/1000\r\nContent-Length: 800\r\n", 206, 200, &size));
    EXPECT_EQ(size, 1000);
    for (const auto headers :
         {"Content-Range: bytes 0-999/1000\r\n", "Content-Range: bytes 200-1000/1000\r\n",
          "Content-Range: bytes 200-999/1000\r\nContent-Length: 799\r\n",
          "Content-Range: bytes 200-999/1000\r\nContent-Range: bytes 200-999/1000\r\n",
          "Content-Length: 18446744073709551615\r\n"})
        EXPECT_FALSE(iptv::http::ParseStreamRange(headers, 206, 200, &size));
    EXPECT_TRUE(
        iptv::http::ParseStreamRange("Content-Range: bytes 200-999/*\r\n", 206, 200, &size));
    EXPECT_EQ(size, -1);
    EXPECT_TRUE(iptv::http::ParseStreamRange("Transfer-Encoding: chunked\r\n", 200, -1, &size));
    EXPECT_EQ(size, -1);
}

TEST(IptvHttpTest, ExplainsStandardHttpFailures)
{
    EXPECT_EQ(Describe(iptv::http::Status::http_status_error, 404),
              "stream is offline or no longer exists (HTTP 404)");
    EXPECT_EQ(Describe(iptv::http::Status::http_status_error, 451),
              "unavailable for legal or regional restrictions (HTTP 451)");
    EXPECT_EQ(Describe(iptv::http::Status::http_status_error, 503),
              "the channel provider is unavailable (HTTP 503)");
}

TEST(IptvHttpTest, PreservesNativeConnectionErrorForLogsAndUi)
{
    EXPECT_EQ(Describe(iptv::http::Status::request_failed, 0, -42),
              "connection failed (native error 0xFFFFFFD6)");
}

} // namespace
