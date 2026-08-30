/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_webm.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kEbml = 0x1a45dfa3u;
constexpr std::uint32_t kDocType = 0x4282u;
constexpr std::uint32_t kSegment = 0x18538067u;
constexpr std::uint32_t kInfo = 0x1549a966u;
constexpr std::uint32_t kTimestampScale = 0x2ad7b1u;
constexpr std::uint32_t kTracks = 0x1654ae6bu;
constexpr std::uint32_t kTrackEntry = 0xaeu;
constexpr std::uint32_t kTrackNumber = 0xd7u;
constexpr std::uint32_t kTrackType = 0x83u;
constexpr std::uint32_t kCodecId = 0x86u;
constexpr std::uint32_t kCodecPrivate = 0x63a2u;
constexpr std::uint32_t kVideo = 0xe0u;
constexpr std::uint32_t kPixelWidth = 0xb0u;
constexpr std::uint32_t kPixelHeight = 0xbau;
constexpr std::uint32_t kDisplayWidth = 0x54b0u;
constexpr std::uint32_t kDisplayHeight = 0x54bau;
constexpr std::uint32_t kContentEncodings = 0x6d80u;
constexpr std::uint32_t kCluster = 0x1f43b675u;
constexpr std::uint32_t kClusterTimestamp = 0xe7u;
constexpr std::uint32_t kSimpleBlock = 0xa3u;
constexpr std::uint32_t kBlockGroup = 0xa0u;
constexpr std::uint32_t kBlock = 0xa1u;
constexpr std::uint32_t kEncryptedBlock = 0xafu;

void Append(Bytes *output, const Bytes &value)
{
    output->insert(output->end(), value.begin(), value.end());
}

void AppendId(Bytes *output, std::uint32_t id)
{
    unsigned bytes = 1u;
    while (bytes < 4u && id >= (UINT32_C(1) << (bytes * 8u)))
        ++bytes;
    for (unsigned index = bytes; index; --index)
        output->push_back(static_cast<std::uint8_t>(id >> ((index - 1u) * 8u)));
}

void AppendSize(Bytes *output, std::uint64_t size)
{
    unsigned bytes = 1u;
    while (bytes < 8u && size >= ((UINT64_C(1) << (bytes * 7u)) - 1u))
        ++bytes;
    ASSERT_LT(size, (UINT64_C(1) << (bytes * 7u)) - 1u);
    Bytes encoded(bytes);
    for (unsigned index = bytes; index; --index)
    {
        encoded[index - 1u] = static_cast<std::uint8_t>(size);
        size >>= 8u;
    }
    encoded[0] |= static_cast<std::uint8_t>(UINT32_C(1) << (8u - bytes));
    Append(output, encoded);
}

Bytes Element(std::uint32_t id, const Bytes &payload)
{
    Bytes value;
    AppendId(&value, id);
    AppendSize(&value, payload.size());
    Append(&value, payload);
    return value;
}

Bytes UnknownMaster(std::uint32_t id, const Bytes &payload)
{
    Bytes value;
    AppendId(&value, id);
    value.push_back(0xffu);
    Append(&value, payload);
    return value;
}

Bytes UInt(std::uint64_t value)
{
    unsigned bytes = 1u;
    while (bytes < 8u && value >= (UINT64_C(1) << (bytes * 8u)))
        ++bytes;
    Bytes encoded(bytes);
    for (unsigned index = bytes; index; --index)
    {
        encoded[index - 1u] = static_cast<std::uint8_t>(value);
        value >>= 8u;
    }
    return encoded;
}

Bytes Text(const char *value)
{
    const auto *first = reinterpret_cast<const std::uint8_t *>(value);
    return Bytes(first, first + std::strlen(value));
}

Bytes BlockPayload(std::int16_t relative, std::uint8_t flags, const Bytes &frame)
{
    Bytes value{0x82u, static_cast<std::uint8_t>(static_cast<std::uint16_t>(relative) >> 8u),
                static_cast<std::uint8_t>(relative), flags};
    Append(&value, frame);
    return value;
}

struct DocumentOptions
{
    const char *codec = "V_VP9";
    Bytes codec_private{};
    bool include_codec_private = false;
    bool content_encoding = false;
    std::uint32_t width = 1920u;
    std::uint32_t height = 1080u;
    bool display_geometry = true;
    std::uint8_t block_flags = 0x80u;
    bool second_block = true;
    bool encrypted_block = false;
};

Bytes Document(const DocumentOptions &options = {})
{
    const Bytes header = Element(kEbml, Element(kDocType, Text("webm")));

    Bytes info;
    Append(&info, Element(kTimestampScale, UInt(500000u)));

    Bytes video;
    Append(&video, Element(kPixelWidth, UInt(options.width)));
    Append(&video, Element(kPixelHeight, UInt(options.height)));
    if (options.display_geometry)
    {
        Append(&video, Element(kDisplayWidth, UInt(1280u)));
        Append(&video, Element(kDisplayHeight, UInt(720u)));
    }

    Bytes track;
    Append(&track, Element(kTrackNumber, UInt(2u)));
    Append(&track, Element(kTrackType, UInt(1u)));
    Append(&track, Element(kCodecId, Text(options.codec)));
    if (options.include_codec_private)
        Append(&track, Element(kCodecPrivate, options.codec_private));
    Append(&track, Element(kVideo, video));
    if (options.content_encoding)
        Append(&track, Element(kContentEncodings, {}));

    Bytes segment;
    Append(&segment, Element(kInfo, info));
    Append(&segment, Element(kTracks, Element(kTrackEntry, track)));

    Bytes cluster;
    Append(&cluster, Element(kClusterTimestamp, UInt(10u)));
    Append(&cluster,
           Element(kSimpleBlock, BlockPayload(-2, options.block_flags, {0x82u, 0x49u, 0x83u})));
    if (options.second_block)
    {
        const Bytes grouped = Element(kBlock, BlockPayload(4, 0u, {0x11u, 0x22u}));
        Append(&cluster, Element(kBlockGroup, grouped));
    }
    if (options.encrypted_block)
        Append(&cluster, Element(kEncryptedBlock, BlockPayload(5, 0u, {0x33u})));
    Append(&segment, UnknownMaster(kCluster, cluster));

    Bytes document = header;
    Append(&document, UnknownMaster(kSegment, segment));
    return document;
}

struct Capture
{
    std::vector<Bytes> frames;
    std::vector<std::uint64_t> timestamps;
    std::vector<iptv_webm_block_kind_t> kinds;
};

int CaptureBlock(void *context, const iptv_webm_video_info_t *, const iptv_webm_block_t *block)
{
    auto *capture = static_cast<Capture *>(context);
    capture->frames.emplace_back(block->data, block->data + block->bytes);
    capture->timestamps.push_back(block->pts_us);
    capture->kinds.push_back(block->kind);
    return 0;
}

struct Parser
{
    iptv_webm_parser_t value{};
    Capture capture{};

    explicit Parser(const iptv_webm_limits_t *limits = nullptr)
    {
        iptv_webm_init(&value);
        EXPECT_EQ(iptv_webm_open(&value, limits, CaptureBlock, &capture), IPTV_WEBM_OK);
    }

    ~Parser()
    {
        EXPECT_EQ(iptv_webm_cleanup(&value), IPTV_WEBM_OK);
    }
};

TEST(IptvWebmTest, ParsesSplitUnknownSizeContainersAndBothBlockForms)
{
    const Bytes document = Document({.codec_private = {1u, 1u, 0u}, .include_codec_private = true});
    Parser parser;
    for (const std::uint8_t byte : document)
        ASSERT_EQ(iptv_webm_push(&parser.value, &byte, 1u), IPTV_WEBM_OK);
    ASSERT_EQ(iptv_webm_finish(&parser.value), IPTV_WEBM_OK);

    const iptv_webm_video_info_t *video = iptv_webm_video(&parser.value);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->track_number, 2u);
    EXPECT_EQ(video->profile, 0u);
    EXPECT_EQ(video->profile_present, 1u);
    EXPECT_EQ(video->pixel_width, 1920u);
    EXPECT_EQ(video->pixel_height, 1080u);
    EXPECT_EQ(video->display_width, 1280u);
    EXPECT_EQ(video->display_height, 720u);
    EXPECT_EQ(video->timestamp_scale_ns, 500000u);

    ASSERT_EQ(parser.capture.frames.size(), 2u);
    EXPECT_EQ(parser.capture.frames[0], (Bytes{0x82u, 0x49u, 0x83u}));
    EXPECT_EQ(parser.capture.frames[1], (Bytes{0x11u, 0x22u}));
    EXPECT_EQ(parser.capture.timestamps, (std::vector<std::uint64_t>{4000u, 7000u}));
    EXPECT_EQ(parser.capture.kinds,
              (std::vector<iptv_webm_block_kind_t>{IPTV_WEBM_SIMPLE_BLOCK, IPTV_WEBM_BLOCK}));
}

TEST(IptvWebmTest, DefaultsMissingProfileAndDisplayGeometry)
{
    DocumentOptions options;
    options.display_geometry = false;
    options.second_block = false;
    const Bytes document = Document(options);
    Parser parser;
    ASSERT_EQ(iptv_webm_push(&parser.value, document.data(), document.size()), IPTV_WEBM_OK);
    ASSERT_EQ(iptv_webm_finish(&parser.value), IPTV_WEBM_OK);

    const iptv_webm_video_info_t *video = iptv_webm_video(&parser.value);
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->profile, 0u);
    EXPECT_EQ(video->profile_present, 0u);
    EXPECT_EQ(video->display_width, video->pixel_width);
    EXPECT_EQ(video->display_height, video->pixel_height);
}

TEST(IptvWebmTest, RejectsMalformedAndDeclaredOversizeElements)
{
    {
        Parser parser;
        const std::uint8_t invalid_id = 0u;
        EXPECT_EQ(iptv_webm_push(&parser.value, &invalid_id, 1u), IPTV_WEBM_MALFORMED);
    }
    {
        iptv_webm_limits_t limits{};
        iptv_webm_default_limits(&limits);
        limits.max_retained_bytes = 64u;
        limits.max_element_bytes = 16u;
        Parser parser(&limits);
        const std::uint8_t oversized[] = {0xecu, 0x91u};
        EXPECT_EQ(iptv_webm_push(&parser.value, oversized, sizeof(oversized)),
                  IPTV_WEBM_ELEMENT_LIMIT);
    }
    {
        Bytes truncated = Document();
        truncated.pop_back();
        Parser parser;
        EXPECT_EQ(iptv_webm_push(&parser.value, truncated.data(), truncated.size()), IPTV_WEBM_OK);
        EXPECT_EQ(iptv_webm_finish(&parser.value), IPTV_WEBM_TRUNCATED);
    }
}

TEST(IptvWebmTest, RejectsWrongCodecAndNonzeroVp9Profile)
{
    {
        DocumentOptions options;
        options.codec = "V_VP8";
        const Bytes document = Document(options);
        Parser parser;
        EXPECT_EQ(iptv_webm_push(&parser.value, document.data(), document.size()),
                  IPTV_WEBM_UNSUPPORTED_CODEC);
    }
    {
        DocumentOptions options;
        options.codec_private = {1u, 1u, 1u};
        options.include_codec_private = true;
        const Bytes document = Document(options);
        Parser parser;
        EXPECT_EQ(iptv_webm_push(&parser.value, document.data(), document.size()),
                  IPTV_WEBM_UNSUPPORTED_PROFILE);
    }
}

TEST(IptvWebmTest, RejectsLacedVideo)
{
    DocumentOptions options;
    options.block_flags = 0x82u;
    const Bytes document = Document(options);
    Parser parser;
    EXPECT_EQ(iptv_webm_push(&parser.value, document.data(), document.size()),
              IPTV_WEBM_UNSUPPORTED_LACING);
}

TEST(IptvWebmTest, RejectsContentEncodingAndEncryptedBlocks)
{
    {
        DocumentOptions options;
        options.content_encoding = true;
        const Bytes document = Document(options);
        Parser parser;
        EXPECT_EQ(iptv_webm_push(&parser.value, document.data(), document.size()),
                  IPTV_WEBM_UNSUPPORTED_CONTENT_ENCODING);
    }
    {
        DocumentOptions options;
        options.encrypted_block = true;
        const Bytes document = Document(options);
        Parser parser;
        EXPECT_EQ(iptv_webm_push(&parser.value, document.data(), document.size()),
                  IPTV_WEBM_UNSUPPORTED_CONTENT_ENCODING);
    }
}

TEST(IptvWebmTest, EnforcesDimensionAndNestingLimits)
{
    {
        iptv_webm_limits_t limits{};
        iptv_webm_default_limits(&limits);
        limits.max_width = 1280u;
        const Bytes document = Document();
        Parser parser(&limits);
        EXPECT_EQ(iptv_webm_push(&parser.value, document.data(), document.size()),
                  IPTV_WEBM_DIMENSION_LIMIT);
    }
    {
        iptv_webm_limits_t limits{};
        iptv_webm_default_limits(&limits);
        limits.max_nesting = 2u;
        const Bytes document = Document();
        Parser parser(&limits);
        EXPECT_EQ(iptv_webm_push(&parser.value, document.data(), document.size()),
                  IPTV_WEBM_NESTING_LIMIT);
    }
}

} // namespace
