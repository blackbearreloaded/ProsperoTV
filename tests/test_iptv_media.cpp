// ProsperoTV - Real MP4/Matroska packets through the existing stream parser.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_media.h"
#include "iptv_stream.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
struct Memory
{
    std::vector<std::uint8_t> bytes, transport;
    std::size_t at = 0, piece = 4096;
    bool stop = false, reject_output = false;
    unsigned seeks = 0;
    explicit Memory(const char *name)
    {
        std::ifstream file(std::string("build/media-tests/fixtures/") + name, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), {});
    }
    int run(std::string *error)
    {
        const iptv::MediaInput input{
            this,
            [](void *self, std::uint8_t *out, int cap)
            {
                auto &m = *static_cast<Memory *>(self);
                const auto count =
                    std::min({m.bytes.size() - m.at, m.piece, static_cast<std::size_t>(cap)});
                std::memcpy(out, m.bytes.data() + m.at, count);
                m.at += count;
                return static_cast<int>(count);
            },
            [](void *self, std::int64_t offset)
            {
                auto &m = *static_cast<Memory *>(self);
                if (offset < 0 || static_cast<std::uint64_t>(offset) > m.bytes.size())
                    return std::int64_t(-1);
                ++m.seeks;
                m.at = static_cast<std::size_t>(offset);
                return offset;
            },
            [](void *self)
            { return static_cast<std::int64_t>(static_cast<Memory *>(self)->bytes.size()); },
            [](void *self) { return static_cast<Memory *>(self)->stop; }};
        const iptv::MediaOutput output{
            this, [](void *self, const std::uint8_t *bytes, std::size_t count)
            {
                auto &m = *static_cast<Memory *>(self);
                if (m.reject_output)
                    return false;
                m.transport.insert(m.transport.end(), bytes, bytes + count);
                return true;
            }};
        return iptv::ReadMedia(input, output, error);
    }
};
struct Frames
{
    iptv_stream_format_t format{};
    unsigned video = 0, audio = 0;
};
void check_transport(const Memory &memory, bool hevc)
{
    Frames frames;
    iptv_stream_backend_t backend{};
    backend.context = &frames;
    backend.open = [](void *self, const iptv_stream_format_t *format)
    {
        static_cast<Frames *>(self)->format = *format;
        return 0;
    };
    backend.submit_video =
        [](void *self, const std::uint8_t *bytes, std::size_t count, std::uint64_t)
    {
        EXPECT_GT(count, 4u);
        EXPECT_EQ(bytes[0], 0);
        EXPECT_EQ(bytes[1], 0);
        ++static_cast<Frames *>(self)->video;
        return 0;
    };
    backend.submit_audio =
        [](void *self, const std::uint8_t *bytes, std::size_t count, std::uint64_t)
    {
        EXPECT_GT(count, 7u);
        EXPECT_EQ(bytes[0], 0xff);
        EXPECT_EQ(bytes[1] & 0xf6, 0xf0);
        ++static_cast<Frames *>(self)->audio;
        return 0;
    };
    backend.drain = [](void *) { return 0; };
    backend.close = [](void *) {};
    backend.disable_audio = [](void *) { return 0; };
    backend.discontinuity = [](void *) { return 0; };
    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
    const int pushed = iptv_stream_push(&session, memory.transport.data(), memory.transport.size());
    EXPECT_EQ(pushed, IPTV_STREAM_OK) << session.telemetry.last_error;
    EXPECT_EQ(iptv_stream_stop(&session), IPTV_STREAM_OK) << session.telemetry.last_error;
    EXPECT_EQ(frames.format.video_codec, hevc ? IPTV_STREAM_VIDEO_HEVC : IPTV_STREAM_VIDEO_H264);
    EXPECT_EQ(frames.format.visible_width, hevc ? 160u : 320u);
    EXPECT_EQ(frames.video, hevc ? 5u : 25u);
    if (!hevc)
        EXPECT_GE(frames.audio, 46u);
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
}

TEST(Media, ReadsMp4MoovAtEitherEndAndMatroskaWithAacSound)
{
    for (const auto *name : {"h264-aac.mp4", "h264-aac-fast.mp4", "h264-aac.mkv"})
        for (const auto piece : {13u, 65536u})
        {
            SCOPED_TRACE(name);
            Memory memory(name);
            ASSERT_FALSE(memory.bytes.empty());
            ASSERT_TRUE(iptv::LooksLikeMedia(memory.bytes.data(), memory.bytes.size()));
            memory.piece = piece;
            std::string error;
            ASSERT_EQ(memory.run(&error), 0) << error;
            ASSERT_FALSE(memory.transport.empty());
            check_transport(memory, false);
        }
}
TEST(Media, ConvertsHevcLengthPrefixedPacketsForTheNativeDecoder)
{
    Memory memory("hevc.mp4");
    ASSERT_FALSE(memory.bytes.empty());
    std::string error;
    ASSERT_EQ(memory.run(&error), 0) << error;
    check_transport(memory, true);
}
TEST(Media, StopsOnCancellationOrOutputFailureAndRejectsNonMedia)
{
    Memory cancelled("h264-aac.mp4");
    cancelled.stop = true;
    std::string error;
    EXPECT_EQ(cancelled.run(&error), 1);
    EXPECT_TRUE(cancelled.transport.empty());
    Memory failed("h264-aac.mp4");
    failed.reject_output = true;
    EXPECT_EQ(failed.run(&error), -1);
    EXPECT_FALSE(error.empty());
    Memory invalid("h264-aac.mp4");
    invalid.bytes.assign(1200, 'x');
    EXPECT_EQ(invalid.run(&error), -1);
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(iptv::LooksLikeMedia(invalid.bytes.data(), invalid.bytes.size()));
}
} // namespace
