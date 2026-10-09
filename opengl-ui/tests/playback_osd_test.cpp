// ProsperoTV - Compositor bounds and the real baked font on video surfaces.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/playback_osd.hpp"
#include "core/save_file.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

TEST(VideoPanel, CompositeKeepsPaddingAndSupportsNativeLowBitMain10)
{
    ptv::VideoPanel panel;
    panel.reset(480, 270, 960, 540);
    std::fill(panel.pixels.begin(), panel.pixels.end(), 235);
    constexpr unsigned pitch = 40, sh = 20, vw = 32, vh = 16;
    constexpr unsigned count = pitch * sh * 3 / 2;
    std::vector<std::uint8_t> eight(count + 32, 77);
    std::vector<std::uint16_t> ten(count + 16, 77 * 4);
    ASSERT_TRUE(panel.composite(eight.data(), count, pitch, sh, vw, vh, 8));
    ASSERT_TRUE(panel.composite(ten.data(), count * 2, pitch, sh, vw, vh, 10));
    for (unsigned y = 0; y < sh; ++y)
        for (unsigned x = 0; x < pitch; ++x)
        {
            const unsigned expected = y >= 4 && y < 12 && x >= 8 && x < 24 ? 235 : 77;
            EXPECT_EQ(eight[y * pitch + x], expected);
            EXPECT_EQ(ten[y * pitch + x], expected * 4);
        }
    for (unsigned y = 0; y < sh / 2; ++y)
        for (unsigned x = 0; x < pitch; ++x)
        {
            const unsigned expected = y >= 2 && y < 6 && x >= 8 && x < 24 ? 128 : 77;
            EXPECT_EQ(eight[pitch * sh + y * pitch + x], expected);
            EXPECT_EQ(ten[pitch * sh + y * pitch + x], expected * 4);
        }
    EXPECT_EQ(eight.back(), 77);
    EXPECT_EQ(ten.back(), 308);
    const auto unchanged = eight;
    EXPECT_FALSE(panel.composite(eight.data(), count - 1, pitch, sh, vw, vh, 8));
    EXPECT_FALSE(panel.composite(eight.data(), count, pitch, sh, pitch + 1, vh, 8));
    EXPECT_FALSE(panel.composite(eight.data(), count, pitch, sh, vw, vh, 12));
    EXPECT_FALSE(panel.composite(eight.data(), count, pitch - 1, sh, vw, vh, 8));
    EXPECT_FALSE(panel.composite(eight.data(), count, pitch, sh, vw, sh + 1, 8));
    EXPECT_EQ(eight, unchanged);
    panel.reset(1900, 0, 200, 200);
    EXPECT_TRUE(panel.pixels.empty());
}

TEST(VideoPanel, SubtitleBitmapsScaleClipBlendAndLeavePaddingUntouched)
{
    constexpr unsigned pitch = 16, sh = 12, vw = 13, vh = 11, count = pitch * sh * 3 / 2;
    iptv::SubtitleCue cue;
    cue.canvas_width = cue.canvas_height = 5;
    iptv::SubtitleBitmap bitmap;
    bitmap.x = bitmap.y = 1;
    bitmap.width = bitmap.height = 3;
    bitmap.argb.assign(9, 0xffffffff);
    bitmap.argb[4] = 0; // Transparent hole must reveal the picture's luma/chroma.
    cue.bitmaps.push_back(bitmap);
    const auto opaque = [&](unsigned x, unsigned y)
    {
        if (x >= vw || y >= vh)
            return false;
        const auto sx = x * 5 / vw, sy = y * 5 / vh;
        return sx >= 1 && sx <= 3 && sy >= 1 && sy <= 3 && !(sx == 2 && sy == 2);
    };
    std::vector<std::uint8_t> eight(count + 20, 77);
    std::vector<std::uint16_t> ten(count + 10, 308);
    ASSERT_TRUE(ptv::composite_subtitle_bitmaps(cue, eight.data(), count, pitch, sh, vw, vh, 8));
    ASSERT_TRUE(ptv::composite_subtitle_bitmaps(cue, ten.data(), count * 2, pitch, sh, vw, vh, 10));
    for (unsigned y = 0; y < sh; ++y)
        for (unsigned x = 0; x < pitch; ++x)
        {
            EXPECT_EQ(eight[y * pitch + x], opaque(x, y) ? 235 : 77);
            EXPECT_EQ(ten[y * pitch + x], opaque(x, y) ? 940 : 308);
        }
    for (unsigned y = 0; y < sh / 2; ++y)
        for (unsigned x = 0; x < pitch; x += 2)
        {
            const unsigned alpha = opaque(x, y * 2) + opaque(x + 1, y * 2) + opaque(x, y * 2 + 1) +
                                   opaque(x + 1, y * 2 + 1);
            for (unsigned c = 0; c < 2; ++c)
            {
                EXPECT_EQ(eight[pitch * sh + y * pitch + x + c],
                          (77 * (4 - alpha) + 128 * alpha + 2) / 4);
                EXPECT_EQ(ten[pitch * sh + y * pitch + x + c],
                          (308 * (4 - alpha) + 512 * alpha + 2) / 4);
            }
        }
    EXPECT_EQ(eight.back(), 77);
    EXPECT_EQ(ten.back(), 308);
    const auto before = eight;
    EXPECT_FALSE(
        ptv::composite_subtitle_bitmaps(cue, eight.data(), count - 1, pitch, sh, vw, vh, 8));
    EXPECT_EQ(eight, before);
    cue.bitmaps[0].x = -1;
    EXPECT_TRUE(ptv::composite_subtitle_bitmaps(cue, eight.data(), count, pitch, sh, vw, vh, 8));
    cue.bitmaps[0].argb.clear();
    EXPECT_FALSE(ptv::composite_subtitle_bitmaps(cue, eight.data(), count, pitch, sh, vw, vh, 8));
}

TEST(VideoPanel, BakedUnicodeFontDrawsClippedAntialiasedText)
{
    ASSERT_NE(std::getenv("KIT_FONTS"), nullptr);
    std::string data;
    ASSERT_TRUE(hui::save::read_file(
        std::string(std::getenv("KIT_FONTS")) + "/inter-regular.huifont", &data));
    hui::gfx::Font font;
    ASSERT_TRUE(font.load(data));
    ptv::VideoPanel panel;
    panel.reset(0, 0, 400, 80);
    panel.text(font, "Français · Ελληνικά · Новости", -6, 45, 32, 380);
    EXPECT_GT(
        std::count_if(panel.pixels.begin(), panel.pixels.end(), [](auto v) { return v > 220; }),
        100);
    EXPECT_GT(std::count_if(panel.pixels.begin(), panel.pixels.end(),
                            [](auto v) { return v > 28 && v < 220; }),
              100);
    const auto before = panel.pixels;
    panel.text(font, "Beyond the panel", 600, 200, 32, 300);
    EXPECT_EQ(panel.pixels, before);
}
