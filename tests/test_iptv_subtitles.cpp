// ProsperoTV - Subtitle timing, decoder isolation, and synthetic DVB pictures.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_subtitles.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <string_view>

namespace
{
iptv::SubtitleTrack track(iptv::SubtitleCodec codec, unsigned id = 1)
{
    iptv::SubtitleTrack result;
    result.info.id = id;
    result.info.codec = codec;
    result.info.language = id == 1 ? "eng" : "spa";
    return result;
}
bool text(iptv::Subtitles &subtitles, std::string_view value, std::int64_t at = 1000000,
          std::int64_t duration = 1000000, unsigned id = 1)
{
    return subtitles.push(id, reinterpret_cast<const std::uint8_t *>(value.data()), value.size(),
                          at, duration);
}
std::vector<std::uint8_t> dvb(bool clear = false)
{
    std::vector<std::uint8_t> out;
    const auto segment = [&](std::uint8_t type, std::initializer_list<std::uint8_t> body)
    {
        out.insert(out.end(), {0x0f, type, 0, 1, 0, static_cast<std::uint8_t>(body.size())});
        out.insert(out.end(), body);
    };
    if (clear)
        segment(0x10, {0, 0x1b}); // New page version, no regions.
    else
    {
        segment(0x14, {0, 2, 0xcf, 2, 0x3f});             // 720 x 576 canvas.
        segment(0x10, {30, 0x0b, 0, 0xff, 0, 10, 0, 20}); // Region at 10,20.
        segment(0x12, {0, 0x0f, 0, 0x3f, 0, 128, 128, 255, 1, 0x3f, 235, 128, 128, 0});
        segment(0x11, {0, 7, 0, 4, 0, 2, 0x6f, 0, 0, 3, 0, 0, 0, 0, 0xf0, 0});
        segment(0x13,
                {0, 0, 1, 0, 8, 0, 8, 0x12, 1, 1, 1, 1, 0, 0, 0xf0, 0x12, 1, 1, 1, 1, 0, 0, 0xf0});
    }
    segment(0x80, {});
    return out;
}

TEST(Subtitles, RepeatedHlsCuesAreRenderedOnceAfterChangingLanguage)
{
    iptv::Subtitles subtitles;
    subtitles.set_tracks({track(iptv::SubtitleCodec::webvtt)});
    EXPECT_TRUE(text(subtitles, "Same cue"));
    EXPECT_TRUE(text(subtitles, "Same cue"));
    ASSERT_TRUE(subtitles.select(1));
    ASSERT_EQ(subtitles.at(1250000).size(), 1u);
    EXPECT_TRUE(text(subtitles, "Same cue"));
    EXPECT_EQ(subtitles.at(1250000).size(), 1u);
    EXPECT_TRUE(subtitles.select(0));
    EXPECT_TRUE(subtitles.select(1));
    EXPECT_EQ(subtitles.at(1250000).size(), 1u);
    EXPECT_TRUE(text(subtitles, "Different cue"));
    EXPECT_EQ(subtitles.at(1250000).size(), 2u);
}

TEST(Subtitles, UsesPictureTimestampsRatherThanDownloadTimeAndPreservesUnicode)
{
    iptv::Subtitles subtitles;
    subtitles.set_tracks({track(iptv::SubtitleCodec::subrip)});
    EXPECT_EQ(subtitles.state().selected, 0u);
    ASSERT_TRUE(subtitles.select(1));
    ASSERT_TRUE(text(subtitles, "Hello, <i>world</i>!\nFrançois — 中文"));
    EXPECT_TRUE(subtitles.at(999999).empty());
    const auto active = subtitles.at(1000000);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0]->text, "Hello, world!\nFrançois — 中文");
    EXPECT_EQ(active[0]->end_us, 2000000);
    EXPECT_EQ(subtitles.at(1999999).size(), 1u);
    EXPECT_TRUE(subtitles.at(2000000).empty());
    // A rendered immutable cue remains valid after the session forgets it.
    EXPECT_EQ(active[0]->text, "Hello, world!\nFrançois — 中文");
}
TEST(Subtitles, AssStylesAndVectorDrawingCommandsAreNotVisibleDialogue)
{
    iptv::Subtitles subtitles;
    subtitles.set_tracks({track(iptv::SubtitleCodec::ass)});
    ASSERT_TRUE(subtitles.select(1));
    ASSERT_TRUE(
        text(subtitles,
             "0,0,Default,,0,0,0,,{\\i1}First{\\i0}\\N{\\p1}m 0 0 l 10 10{\\p0}Second\\hline"));
    const auto active = subtitles.at(1500000);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0]->text, "First\nSecond line");
}
TEST(Subtitles, OverlappingTextRemainsUntilItsOwnEnd)
{
    iptv::Subtitles subtitles;
    subtitles.set_tracks({track(iptv::SubtitleCodec::text)});
    ASSERT_TRUE(subtitles.select(1));
    ASSERT_TRUE(text(subtitles, "First", 1000000, 3000000));
    ASSERT_TRUE(text(subtitles, "Second", 2000000, 1000000));
    EXPECT_EQ(subtitles.at(2000000).size(), 2u);
    const auto active = subtitles.at(3000000);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0]->text, "First");
}
TEST(Subtitles, LanguageChangesOffAndDiscontinuityClearOldCues)
{
    iptv::Subtitles subtitles;
    const auto english = track(iptv::SubtitleCodec::webvtt),
               spanish = track(iptv::SubtitleCodec::webvtt, 2);
    subtitles.set_tracks({english, spanish});
    ASSERT_TRUE(subtitles.select(1));
    ASSERT_TRUE(text(subtitles, "<b>English</b>"));
    subtitles.set_tracks({spanish, english});
    EXPECT_EQ(subtitles.state().selected, 1u);
    EXPECT_FALSE(subtitles.select(99));
    EXPECT_EQ(subtitles.at(1000000).size(), 1u);
    ASSERT_TRUE(subtitles.select(2));
    EXPECT_TRUE(subtitles.at(1000000).empty());
    ASSERT_TRUE(text(subtitles, "Wrong language", 1000000, 1000000, 1));
    EXPECT_TRUE(subtitles.at(1000000).empty());
    ASSERT_TRUE(text(subtitles, "Español", 1000000, 1000000, 2));
    ASSERT_EQ(subtitles.at(1000000).size(), 1u);
    subtitles.reset_timeline();
    EXPECT_TRUE(subtitles.at(1000000).empty());
    ASSERT_TRUE(text(subtitles, "Español", 1000000, 1000000, 2));
    ASSERT_TRUE(subtitles.select(0));
    EXPECT_TRUE(subtitles.at(1000000).empty());
    ASSERT_TRUE(subtitles.select(2));
    subtitles.set_tracks({english});
    EXPECT_EQ(subtitles.state().selected, 0u);
    subtitles.clear();
    EXPECT_TRUE(subtitles.state().tracks.empty());
}
TEST(Subtitles, SelectingAfterReadAheadReplaysTheCaptionForTheCurrentPicture)
{
    iptv::Subtitles subtitles;
    subtitles.set_tracks(
        {track(iptv::SubtitleCodec::subrip), track(iptv::SubtitleCodec::subrip, 2)});
    ASSERT_TRUE(text(subtitles, "English", 1000000, 3000000, 1));
    ASSERT_TRUE(text(subtitles, "Español", 1000000, 3000000, 2));
    ASSERT_TRUE(text(subtitles, "Next", 6000000, 1000000, 1));
    EXPECT_TRUE(subtitles.at(2000000).empty()); // Off, while the demuxer is ahead.
    ASSERT_TRUE(subtitles.select(2));
    ASSERT_EQ(subtitles.at(2000000).size(), 1u);
    EXPECT_EQ(subtitles.at(2000000)[0]->text, "Español");
    ASSERT_TRUE(subtitles.select(1));
    ASSERT_EQ(subtitles.at(2000000).size(), 1u);
    EXPECT_EQ(subtitles.at(2000000)[0]->text, "English");
    EXPECT_TRUE(subtitles.at(4000000).empty());
    ASSERT_EQ(subtitles.at(6000000).size(), 1u);
    EXPECT_EQ(subtitles.at(6000000)[0]->text, "Next");
}

TEST(Subtitles, DecodesDvbBitmapPaletteCoordinatesAndTimedClear)
{
    iptv::Subtitles subtitles;
    auto broadcast = track(iptv::SubtitleCodec::dvb);
    broadcast.extra = {0, 1, 0, 1};
    subtitles.set_tracks({broadcast});
    ASSERT_TRUE(subtitles.select(1));
    const auto packet = dvb();
    ASSERT_TRUE(subtitles.push(1, packet.data(), packet.size(), 1000000, 0));
    const auto active = subtitles.at(1000000);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0]->canvas_width, 720u);
    EXPECT_EQ(active[0]->canvas_height, 576u);
    ASSERT_EQ(active[0]->bitmaps.size(), 1u);
    const auto &bitmap = active[0]->bitmaps[0];
    EXPECT_EQ(bitmap.x, 10);
    EXPECT_EQ(bitmap.y, 20);
    EXPECT_EQ(bitmap.width, 4u);
    EXPECT_EQ(bitmap.height, 2u);
    ASSERT_EQ(bitmap.argb.size(), 8u);
    for (const auto color : bitmap.argb)
    {
        EXPECT_EQ(color >> 24, 255u);
        EXPECT_GT(color & 255, 240u);
    }
    const auto clear = dvb(true);
    ASSERT_TRUE(subtitles.push(1, clear.data(), clear.size(), 3000000, 0));
    EXPECT_EQ(subtitles.at(2999999).size(), 1u);
    EXPECT_TRUE(subtitles.at(3000000).empty());
    EXPECT_TRUE(subtitles.at(4000000).empty());
}
TEST(Subtitles, MalformedPacketsAndResourceLimitsAreRecoverable)
{
    iptv::Subtitles subtitles;
    subtitles.set_tracks({track(iptv::SubtitleCodec::mov_text)});
    ASSERT_TRUE(subtitles.select(1));
    const std::uint8_t malformed[] = {0, 100};
    EXPECT_FALSE(subtitles.push(1, malformed, sizeof(malformed), 1000000, 1000000));
    EXPECT_EQ(subtitles.state().error, iptv::SubtitleError::malformed);
    EXPECT_FALSE(subtitles.push(1, malformed, iptv::Subtitles::max_packet + 1, 1000000, 1000000));
    EXPECT_EQ(subtitles.state().error, iptv::SubtitleError::limit);
    const std::uint8_t valid[] = {0, 2, 'O', 'K'};
    ASSERT_TRUE(subtitles.push(1, valid, sizeof(valid), 1000000, 1000000));
    EXPECT_EQ(subtitles.state().error, iptv::SubtitleError::none);
    ASSERT_EQ(subtitles.at(1000000).size(), 1u);
    EXPECT_EQ(subtitles.at(1000000)[0]->text, "OK");
    EXPECT_TRUE(subtitles.push(1, valid, sizeof(valid), -1, 1000000));
    EXPECT_TRUE(
        subtitles.push(1, valid, sizeof(valid), std::numeric_limits<std::int64_t>::max(), 1000000));
}
TEST(Subtitles, CapsProviderTracksAndFutureQueueWithoutEvictingImminentDialogue)
{
    iptv::Subtitles subtitles;
    std::vector<iptv::SubtitleTrack> tracks;
    for (unsigned i = 1; i <= 40; ++i)
        tracks.push_back(track(iptv::SubtitleCodec::text, i));
    subtitles.set_tracks(tracks);
    ASSERT_EQ(subtitles.state().tracks.size(), 32u);
    EXPECT_FALSE(subtitles.select(33));
    ASSERT_TRUE(subtitles.select(1));
    for (unsigned i = 0; i < 512; ++i)
        ASSERT_TRUE(text(subtitles, "Future", 1000000 + i * INT64_C(1000000)));
    EXPECT_FALSE(text(subtitles, "Over capacity", 600000000));
    EXPECT_EQ(subtitles.state().error, iptv::SubtitleError::limit);
    EXPECT_EQ(subtitles.at(1000000).size(), 1u);
    EXPECT_TRUE(subtitles.at(599000000).empty());
    EXPECT_TRUE(text(subtitles, "Recovered", 600000000));
    ASSERT_EQ(subtitles.at(600000000).size(), 1u);
    EXPECT_EQ(subtitles.at(600000000)[0]->text, "Recovered");
}
} // namespace
