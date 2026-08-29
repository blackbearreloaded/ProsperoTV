/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_hls.h"

#include <gtest/gtest.h>

#include <cstring>

namespace
{

iptv_hls_playlist_t ParseMaster(const char *text)
{
    iptv_hls_playlist_t playlist{};
    EXPECT_EQ(iptv_hls_parse(text, std::strlen(text), "http://fixture.test/master.m3u8",
                             std::strlen("http://fixture.test/master.m3u8"), nullptr, &playlist),
              IPTV_HLS_OK);
    return playlist;
}

TEST(IptvHlsTest, AcceptsDeclaredHighResolutionCodecLevels)
{
    const auto playlist = ParseMaster("#EXTM3U\n"
                                      "#EXT-X-STREAM-INF:BANDWIDTH=12000000,RESOLUTION=3840x2160,"
                                      "CODECS=\"avc1.640034,mp4a.40.2\"\n"
                                      "avc-4k.m3u8\n"
                                      "#EXT-X-STREAM-INF:BANDWIDTH=8000000,RESOLUTION=2560x1440,"
                                      "CODECS=\"hvc1.1.6.L150.B0,mp4a.40.2\"\n"
                                      "hevc-1440.m3u8\n"
                                      "#EXT-X-STREAM-INF:BANDWIDTH=16000000,RESOLUTION=3840x2160,"
                                      "CODECS=\"hvc1.1.6.L153.B0,mp4a.40.2\"\n"
                                      "hevc-4k.m3u8\n");

    ASSERT_EQ(playlist.variant_count, 3u);
    EXPECT_EQ(playlist.variants[0].compatible, 1u);
    EXPECT_EQ(playlist.variants[0].level, 52u);
    EXPECT_EQ(playlist.variants[1].compatible, 1u);
    EXPECT_EQ(playlist.variants[1].level, 150u);
    EXPECT_EQ(playlist.variants[2].compatible, 1u);
    EXPECT_EQ(playlist.variants[2].level, 153u);
}

TEST(IptvHlsTest, KeepsMissingAndNonStandardResolutionVariantsEligible)
{
    const auto playlist =
        ParseMaster("#EXTM3U\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=1000000,CODECS=\"avc1.640034,mp4a.40.2\"\n"
                    "unknown-size.m3u8\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=960x540,"
                    "CODECS=\"avc1.640029,mp4a.40.2\"\n"
                    "540p.m3u8\n");

    ASSERT_EQ(playlist.variant_count, 2u);
    EXPECT_EQ(playlist.variants[0].compatible, 1u);
    EXPECT_EQ(playlist.variants[0].width, 0u);
    EXPECT_EQ(playlist.variants[1].compatible, 1u);
    EXPECT_EQ(playlist.variants[1].width, 960u);
    EXPECT_NE(iptv_hls_select_variant(&playlist, nullptr, 0u), IPTV_HLS_NO_VARIANT);
}

TEST(IptvHlsTest, RejectsCodecLevelAboveTheDeclaredResolutionClass)
{
    constexpr char text[] = "#EXTM3U\n"
                            "#EXT-X-STREAM-INF:BANDWIDTH=4000000,RESOLUTION=1920x1080,"
                            "CODECS=\"hvc1.1.6.L153.B0,mp4a.40.2\"\n"
                            "invalid-level.m3u8\n";
    iptv_hls_playlist_t playlist{};
    EXPECT_EQ(iptv_hls_parse(text, std::strlen(text), "http://fixture.test/master.m3u8",
                             std::strlen("http://fixture.test/master.m3u8"), nullptr, &playlist),
              IPTV_HLS_UNSUPPORTED_CODEC);

    ASSERT_EQ(playlist.variant_count, 1u);
    EXPECT_EQ(playlist.variants[0].compatible, 0u);
}

} // namespace
