// ProsperoTV - Preview pixels and asynchronous focus/renderer ownership.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/preview.hpp"
#include "host_preview.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace
{
TEST(PreviewPixels, HdrPreviewMapsBlackPaperWhiteAndHighlightsToSdr)
{
    std::vector<std::uint16_t> samples(4 * 4 * 3 / 2, 512);
    for (unsigned y = 0; y < 4; ++y)
    {
        samples[y * 4] = 64;
        samples[y * 4 + 1] = 573; // About 203 nits.
        samples[y * 4 + 2] = 723; // About 1000 nits.
        samples[y * 4 + 3] = 940; // PQ peak.
    }
    iptv_native_picture_t picture{samples.data(), samples.size() * 2, 4, 4, 4, 4, 10, 0,
                                  {9, 16, 9, 1}};
    ptv::ImagePixels pixels;
    ASSERT_TRUE(ptv::preview_pixels(picture, &pixels));
    EXPECT_EQ(pixels.rgba[0], 0);
    EXPECT_NEAR(pixels.rgba[4], 187, 2);
    EXPECT_GT(pixels.rgba[8], pixels.rgba[4]);
    EXPECT_GT(pixels.rgba[12], pixels.rgba[8]);
    for (unsigned x = 0; x < 4; ++x)
    {
        EXPECT_EQ(pixels.rgba[x * 4], pixels.rgba[x * 4 + 1]);
        EXPECT_EQ(pixels.rgba[x * 4 + 1], pixels.rgba[x * 4 + 2]);
        EXPECT_EQ(pixels.rgba[x * 4 + 3], 255);
    }
}

TEST(PreviewPixels, ReadsPitchedNv12AndNativeMain10AndRejectsTruncatedPictures)
{
    std::vector<std::uint8_t> nv12(8 * 6, 128);
    // Four visible pixels on two rows, with row and surface-height padding.
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 4; ++x)
            nv12[y * 8 + x] = x < 2 ? 16 : 235;
    iptv_native_picture_t frame{nv12.data(), nv12.size(), 8, 4, 4, 2, 8, 0, {}};
    ptv::ImagePixels pixels;
    ASSERT_TRUE(ptv::preview_pixels(frame, &pixels));
    EXPECT_EQ(pixels.width, 4);
    EXPECT_EQ(pixels.height, 2);
    EXPECT_EQ(pixels.rgba[0], 0);
    EXPECT_EQ(pixels.rgba[8], 255);
    EXPECT_EQ(pixels.rgba[16], 0);
    EXPECT_EQ(pixels.rgba[24], 255);
    EXPECT_EQ(pixels.rgba[3], 255);
    std::vector<std::uint8_t> p010(nv12.size() * 2);
    for (std::size_t i = 0; i < nv12.size(); ++i)
    {
        const unsigned word = unsigned(nv12[i]) * 4 + 3;
        p010[i * 2] = word & 255;
        p010[i * 2 + 1] = word >> 8;
    }
    const auto expected = pixels.rgba;
    frame.data = p010.data();
    frame.bytes = p010.size();
    frame.bit_depth = 10;
    ASSERT_TRUE(ptv::preview_pixels(frame, &pixels));
    EXPECT_EQ(pixels.rgba, expected);
    --frame.bytes;
    EXPECT_FALSE(ptv::preview_pixels(frame, &pixels));
    frame.bytes = p010.size();
    frame.width = 9;
    EXPECT_FALSE(ptv::preview_pixels(frame, &pixels));
}

TEST(PreviewPixels, BoundsTheWorkForLargeAndPortraitPictures)
{
    std::vector<std::uint8_t> nv12(3840 * 2160 * 3 / 2, 128);
    iptv_native_picture_t frame{nv12.data(), nv12.size(), 3840, 2160, 3840, 2160, 8, 0, {}};
    ptv::ImagePixels pixels;
    ASSERT_TRUE(ptv::preview_pixels(frame, &pixels));
    EXPECT_EQ(pixels.width, 640);
    EXPECT_EQ(pixels.height, 360);
    frame.pitch = 1080;
    frame.surface_height = 1920;
    frame.width = 1080;
    frame.height = 1920;
    ASSERT_TRUE(ptv::preview_pixels(frame, &pixels));
    EXPECT_LE(pixels.width, 203);
    EXPECT_EQ(pixels.height, 360);
}

TEST(LivePreview, WaitsForFocusDiscardsOldFramesAndJoinsBeforeClosing)
{
    host::set_preview(true);
    ptv::LivePreview preview;
    unsigned uploads = 0, releases = 0;
    const auto renderer = std::this_thread::get_id();
    preview.configure(
        [&](std::uint32_t texture, const ptv::ImagePixels &pixels)
        {
            EXPECT_EQ(std::this_thread::get_id(), renderer);
            EXPECT_FALSE(pixels.rgba.empty());
            ++uploads;
            return texture ? texture : 19u;
        },
        [&](std::uint32_t texture)
        {
            EXPECT_EQ(std::this_thread::get_id(), renderer);
            EXPECT_EQ(texture, 19u);
            ++releases;
        });
    ptv::PlayRequest first;
    first.channel_id = "first";
    first.urls = {"https://stream.invalid/first.ts"};
    preview.update(first, 0.6f);
    EXPECT_EQ(host::preview_starts(), 0u);
    auto second = first;
    second.channel_id = "second";
    preview.update(second, 0.6f);
    EXPECT_EQ(host::preview_starts(), 0u);
    preview.update(second, 0.6f);
    for (int i = 0; i < 100 && !preview.find("second").id; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        preview.update(second, 0.005f);
    }
    EXPECT_EQ(host::preview_starts(), 1u);
    EXPECT_GT(uploads, 0u);
    EXPECT_EQ(preview.find("second").id, 19u);
    EXPECT_EQ(preview.find("first").id, 0u);
    preview.update(first, 0.1f);
    EXPECT_EQ(preview.find("second").id, 0u);
    EXPECT_EQ(preview.find("first").id, 0u);
    EXPECT_EQ(releases, 1u);
    for (int i = 0; i < 100 && host::preview_stops() == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        preview.update(first, 0.005f);
    }
    preview.update(first, 1.2f);
    for (int i = 0; i < 100 && host::preview_starts() != 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    preview.clear();
    EXPECT_EQ(host::preview_starts(), 2u);
    EXPECT_EQ(host::preview_stops(), 2u);
    EXPECT_EQ(preview.find("first").id, 0u);
    host::set_preview(false);
}
} // namespace
