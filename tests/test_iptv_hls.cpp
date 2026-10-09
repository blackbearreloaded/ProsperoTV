/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_hls.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

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

TEST(IptvHlsTest, IsolatesSelectedVariantAndItsRenditionsForPlayback)
{
    auto master = ParseMaster(
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"English\",LANGUAGE=\"en-US\","
        "DEFAULT=YES,AUTOSELECT=YES,URI=\"sound/a.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"b\",NAME=\"Other quality\",URI=\"other.m3u8\"\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",NAME=\"Captions\",LANGUAGE=\"zh-Hans\","
        "FORCED=YES,CHARACTERISTICS=\"public.accessibility.describes-music-and-sound\",URI=\"s."
        "m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,AUDIO=\"a\",SUBTITLES=\"s\"\nfirst.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=500000,AUDIO=\"b\"\nsecond.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=100000\nsimple.m3u8\n");
    std::string text;
    ASSERT_TRUE(iptv::HlsVariantManifest(master, 0, text));
    const auto selected = ParseMaster(text.c_str());
    ASSERT_EQ(selected.variant_count, 1u);
    EXPECT_STREQ(selected.variants[0].url, master.variants[0].url);
    ASSERT_EQ(selected.rendition_count, 2u);
    EXPECT_STREQ(selected.renditions[0].url, "http://fixture.test/sound/a.m3u8");
    EXPECT_STREQ(selected.renditions[0].language, "en-US");
    EXPECT_TRUE(selected.renditions[0].is_default);
    EXPECT_TRUE(selected.renditions[1].hearing_impaired);
    EXPECT_TRUE(selected.renditions[1].forced);
    EXPECT_EQ(text.find("second.m3u8"), text.npos);
    EXPECT_EQ(text.find("other.m3u8"), text.npos);
    EXPECT_TRUE(iptv::HlsVariantManifest(master, 2, text));
    EXPECT_TRUE(text.empty());
    EXPECT_FALSE(iptv::HlsVariantManifest(master, 3, text));
    std::strcpy(master.renditions[0].name, "Injected\"\n#EXT-X-MEDIA:");
    EXPECT_FALSE(iptv::HlsVariantManifest(master, 0, text));
    EXPECT_TRUE(text.empty());
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

TEST(IptvHlsTest, SelectsHevcMain10At4k)
{
    const auto playlist = ParseMaster("#EXTM3U\n"
                                      "#EXT-X-STREAM-INF:BANDWIDTH=18000000,RESOLUTION=3840x2160,"
                                      "CODECS=\"hvc1.2.4.L153.B0,mp4a.40.2\"\nmain10.m3u8\n");
    ASSERT_EQ(playlist.variant_count, 1u);
    EXPECT_EQ(playlist.variants[0].compatible, 1u);
    EXPECT_EQ(playlist.variants[0].profile, 2u);
    EXPECT_EQ(playlist.variants[0].bit_depth, 10u);
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

TEST(IptvHlsTest, KeepsRenditionGroupsLanguagesAccessibilityAndResolvedUris)
{
    const auto playlist = ParseMaster(
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"sound\",NAME=\"English\",LANGUAGE=\"en-US\","
        "DEFAULT=YES,AUTOSELECT=YES\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"sound\",NAME=\"Description, Français\","
        "LANGUAGE=\"fr\",URI=\"audio/fr/list.m3u8?token=fixture\","
        "CHARACTERISTICS=\"public.accessibility.describes-video\"\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"captions\",NAME=\"中文字幕\",LANGUAGE=\"zh-Hans\","
        "FORCED=YES,URI=\"//captions.test/zh.m3u8\","
        "CHARACTERISTICS=\"public.accessibility.transcribes-spoken-dialog,"
        "public.accessibility.describes-music-and-sound\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,CODECS=\"avc1.640029,mp4a.40.2\","
        "AUDIO=\"sound\",SUBTITLES=\"captions\"\nvideo/main.m3u8\n");
    ASSERT_EQ(playlist.rendition_count, 3u);
    EXPECT_STREQ(playlist.variants[0].audio_group, "sound");
    EXPECT_STREQ(playlist.variants[0].subtitle_group, "captions");
    const auto &english = playlist.renditions[0], &french = playlist.renditions[1],
               &chinese = playlist.renditions[2];
    EXPECT_EQ(english.kind, IPTV_HLS_RENDITION_AUDIO);
    EXPECT_EQ(english.is_default, 1u);
    EXPECT_EQ(english.autoselect, 1u);
    EXPECT_STREQ(english.url, "");
    EXPECT_STREQ(english.language, "en-US");
    EXPECT_STREQ(french.name, "Description, Français");
    EXPECT_STREQ(french.url, "http://fixture.test/audio/fr/list.m3u8?token=fixture");
    EXPECT_EQ(french.visual_impaired, 1u);
    EXPECT_EQ(chinese.kind, IPTV_HLS_RENDITION_SUBTITLE);
    EXPECT_STREQ(chinese.name, "中文字幕");
    EXPECT_STREQ(chinese.language, "zh-Hans");
    EXPECT_STREQ(chinese.url, "http://captions.test/zh.m3u8");
    EXPECT_EQ(chinese.forced, 1u);
    EXPECT_EQ(chinese.hearing_impaired, 1u);
}

TEST(IptvHlsTest, RejectsIncompleteAmbiguousOrOversizedRenditions)
{
    const char *base = "http://fixture.test/master.m3u8";
    auto playlist = std::make_unique<iptv_hls_playlist_t>();
    const auto parse = [&](const std::string &text)
    {
        return iptv_hls_parse(text.data(), text.size(), base, std::strlen(base), nullptr,
                              playlist.get());
    };
    const std::string variant = "#EXT-X-STREAM-INF:BANDWIDTH=1000000\nvideo.m3u8\n";
    for (const auto *attributes :
         {"TYPE=AUDIO,NAME=\"Missing group\"",
          "TYPE=AUDIO,GROUP-ID=\"g\",NAME=\"No\",DEFAULT=YES,AUTOSELECT=NO",
          "TYPE=AUDIO,GROUP-ID=\"g\",NAME=\"No\",FORCED=NO",
          "TYPE=SUBTITLES,GROUP-ID=\"g\",NAME=\"Missing URI\"",
          "TYPE=SUBTITLES,GROUP-ID=\"g\",NAME=\"No\",URI=\"file:///local.vtt\"",
          "TYPE=AUDIO,GROUP-ID=\"g\",NAME=\"No\",NAME=\"Duplicate\"",
          "TYPE=AUDIO,GROUP-ID=\"g\",NAME=\"No\",DEFAULT=MAYBE"})
    {
        SCOPED_TRACE(attributes);
        EXPECT_NE(parse(std::string("#EXTM3U\n#EXT-X-MEDIA:") + attributes + "\n" + variant),
                  IPTV_HLS_OK);
    }
    EXPECT_EQ(parse("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"g\",NAME=\"" +
                    std::string(IPTV_HLS_LABEL_BYTES, 'x') + "\"\n" + variant),
              IPTV_HLS_MALFORMED);
    const std::string track = "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"g\",NAME=\"English\"\n";
    EXPECT_EQ(parse("#EXTM3U\n" + track + track + variant), IPTV_HLS_MALFORMED);
    EXPECT_EQ(parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000000,AUDIO=\"missing\"\nv.m3u8\n"),
              IPTV_HLS_MALFORMED);
    EXPECT_EQ(parse("#EXTM3U\n" + track + "#EXT-X-TARGETDURATION:2\n#EXTINF:2,\na.ts\n"),
              IPTV_HLS_MALFORMED);
}

TEST(IptvHlsTest, BoundsRenditionsAndAllowsSameNameInDifferentGroups)
{
    const char *base = "http://fixture.test/master.m3u8";
    auto playlist = std::make_unique<iptv_hls_playlist_t>();
    std::string text = "#EXTM3U\n";
    for (unsigned i = 0; i <= IPTV_HLS_MAX_RENDITIONS; ++i)
    {
        text += "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"group" + std::to_string(i) +
                "\",NAME=\"English\",DEFAULT=YES\n";
        const auto master = text + "#EXT-X-STREAM-INF:BANDWIDTH=1000000,AUDIO=\"group0\"\nv.m3u8\n";
        const auto result = iptv_hls_parse(master.data(), master.size(), base, std::strlen(base),
                                           nullptr, playlist.get());
        EXPECT_EQ(result, i == IPTV_HLS_MAX_RENDITIONS ? IPTV_HLS_OUTPUT_LIMIT : IPTV_HLS_OK);
        EXPECT_LE(playlist->rendition_count, IPTV_HLS_MAX_RENDITIONS);
    }
}

} // namespace
