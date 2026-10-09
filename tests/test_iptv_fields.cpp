// ProsperoTV - Real H.264 field metadata and reference-safe NV12 reconstruction.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_fields.h"
#include <gtest/gtest.h>
#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

TEST(Fields, HevcColorFollowsEachAccessUnitWithoutMistakingMain10ForHdr)
{
    std::unique_ptr<iptv_field_parser_t, decltype(&iptv_field_parser_destroy)> parser(
        iptv_field_parser_create(2), iptv_field_parser_destroy);
    ASSERT_TRUE(parser);
    const struct
    {
        const char *name;
        iptv_color_info_t color;
        bool hdr;
    } cases[] = {{"pq", {9, 16, 9, 1}, true},      {"sdr", {1, 1, 1, 1}, false},
                 {"full", {9, 16, 9, 2}, false},   {"hlg", {9, 18, 9, 1}, false},
                 {"unknown", {2, 2, 2, 1}, false}, {"pq", {9, 16, 9, 1}, true}};
    for (const auto &item : cases)
    {
        SCOPED_TRACE(item.name);
        std::ifstream file(std::string("build/media-tests/fixtures/color-") + item.name + ".hevc",
                           std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        ASSERT_FALSE(bytes.empty());
        const auto info = iptv_field_parse(parser.get(), bytes.data(), bytes.size());
        EXPECT_EQ(info.color.primaries, item.color.primaries);
        EXPECT_EQ(info.color.transfer, item.color.transfer);
        EXPECT_EQ(info.color.matrix, item.color.matrix);
        EXPECT_EQ(info.color.range, item.color.range);
        EXPECT_EQ(bool(iptv_color_is_hdr10(info.color)), item.hdr);
        EXPECT_EQ(info.first, 0u);
    }
    EXPECT_FALSE(iptv_color_is_hdr10(iptv_field_parse(parser.get(), nullptr, 0).color));
    EXPECT_EQ(iptv_field_parser_create(0), nullptr);
}

TEST(Fields, ReadsActualTopBottomAndProgressivePictures)
{
    for (const auto expected : {0u, 1u, 2u})
    {
        const auto name =
            std::string("build/media-tests/fixtures/fields-") + std::to_string(expected) + ".h264";
        std::ifstream file(name, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        ASSERT_FALSE(bytes.empty()) << name;
        std::vector<std::size_t> starts;
        for (std::size_t i = 0; i + 4 < bytes.size(); ++i)
            if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1 &&
                (bytes[i + 4] & 31) == 9)
                starts.push_back(i);
        ASSERT_EQ(starts.size(), 25u);
        starts.push_back(bytes.size());
        std::unique_ptr<iptv_field_parser_t, decltype(&iptv_field_parser_destroy)> parser(
            iptv_field_parser_create(1), iptv_field_parser_destroy);
        ASSERT_TRUE(parser);
        constexpr unsigned pitch = 176, height = 96;
        std::vector<std::uint8_t> surface(pitch * height * 3 / 2 + 16, 0xee);
        std::vector<std::vector<std::uint8_t>> pictures;
        unsigned decoded = 0;
        const auto decode = [&](const void *data, size_t bytes)
        {
            const int result = iptv_field_decode(parser.get(), data, bytes, surface.data(),
                                                 surface.size() - 16, pitch, height, 160, height);
            EXPECT_GE(result, 0);
            if (result > 0)
            {
                ++decoded;
                pictures.push_back(surface);
                EXPECT_EQ(result, expected ? 2 : 1);
                for (unsigned y = 0; y < height * 3 / 2; ++y)
                    for (unsigned x = 160; x < pitch; ++x)
                        EXPECT_EQ(surface[y * pitch + x], y < height ? 16 : 128);
                EXPECT_EQ(surface.back(), 0xee);
            }
            return result;
        };
        for (std::size_t i = 0; i + 1 < starts.size(); ++i)
        {
            const auto info =
                iptv_field_parse(parser.get(), bytes.data() + starts[i], starts[i + 1] - starts[i]);
            EXPECT_EQ(info.first, expected) << name << " picture " << i;
            EXPECT_EQ(info.field_picture, 0u); // MBAFF uses a complete coded picture.
            EXPECT_EQ(info.count, expected ? 2u : 0u);
            EXPECT_EQ(info.duration_us, expected ? 20000u : 0u);
            ASSERT_GE(decode(bytes.data() + starts[i], starts[i + 1] - starts[i]), 0);
        }
        for (unsigned i = 0; i < 25 && decode(nullptr, 0) > 0; ++i)
        {
        }
        EXPECT_EQ(decoded, 25u);
        const auto reference = pictures;
        iptv_field_decoder_reset(parser.get());
        for (std::size_t i = 0; i < 5; ++i)
            ASSERT_GE(decode(bytes.data() + starts[i], starts[i + 1] - starts[i]), 0);
        // Seek while frame threads still retain old pictures. Replaying the
        // keyframe must reproduce the exact sequence, without stale output.
        iptv_field_decoder_reset(parser.get());
        decoded = 0;
        pictures.clear();
        for (std::size_t i = 0; i + 1 < starts.size(); ++i)
            ASSERT_GE(decode(bytes.data() + starts[i], starts[i + 1] - starts[i]), 0);
        for (unsigned i = 0; i < 25 && decode(nullptr, 0) > 0; ++i)
        {
        }
        EXPECT_EQ(decoded, 25u);
        EXPECT_EQ(pictures, reference);
        EXPECT_LT(iptv_field_decode(parser.get(), nullptr, 0, surface.data(), 1, pitch, height, 160,
                                    height),
                  0);
        EXPECT_LT(iptv_field_decode(parser.get(), bytes.data(), SIZE_MAX, surface.data(),
                                    surface.size(), pitch, height, 160, height),
                  0);
        EXPECT_EQ(iptv_field_parse(parser.get(), nullptr, 4).first, 0u);
        EXPECT_EQ(iptv_field_parse(parser.get(), bytes.data(), SIZE_MAX).first, 0u);
    }
}

TEST(Fields, EachFieldKeepsItsOwnMomentAndLeavesSourceAndPaddingIntact)
{
    constexpr unsigned pitch = 12, height = 8, width = 8;
    std::array<std::uint8_t, pitch * height * 3 / 2> source{}, output;
    for (unsigned y = 0; y < height * 3 / 2; ++y)
        for (unsigned x = 0; x < pitch; ++x)
            source[y * pitch + x] = (y & 1) ? 180 + y * 2 : 40 + y * 4;
    const auto original = source;
    for (const unsigned field : {0u, 1u})
    {
        output.fill(0xee);
        ASSERT_EQ(iptv_field_bob(output.data(), output.size(), source.data(), source.size(), pitch,
                                 height, width, field),
                  0);
        unsigned base = 0;
        for (const unsigned rows : {height, height / 2})
        {
            for (unsigned y = 0; y < rows; ++y)
            {
                const unsigned a = (y & 1) == field ? y : y ? y - 1 : 1;
                const unsigned b = (y & 1) == field ? y : y + 1 < rows ? y + 1 : y - 1;
                const unsigned expected =
                    (source[(base + a) * pitch] + source[(base + b) * pitch] + 1) / 2;
                for (unsigned x = 0; x < pitch; ++x)
                    EXPECT_EQ(output[(base + y) * pitch + x], x < width ? expected : 0xee);
            }
            base += rows;
        }
        EXPECT_EQ(source, original);
    }
    EXPECT_NE(iptv_field_bob(source.data(), source.size(), source.data(), source.size(), pitch,
                             height, width, 0),
              0);
    EXPECT_NE(iptv_field_bob(output.data(), output.size() - 1, source.data(), source.size(), pitch,
                             height, width, 0),
              0);
    EXPECT_NE(iptv_field_bob(output.data(), output.size(), source.data(), source.size(), pitch,
                             height, width, 2),
              0);
}
