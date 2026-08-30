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

TEST(IptvHttpTest, DoesNotGuessThatEveryForbiddenResponseIsGeographic)
{
    EXPECT_EQ(Describe(iptv::http::Status::http_status_error, 403, 0, "Forbidden"),
              "access denied by the channel provider (HTTP 403)");
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
