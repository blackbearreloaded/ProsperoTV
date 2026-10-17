// ProsperoTV - Real MP4/Matroska packets through the existing stream parser.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_media.h"
#include "iptv_stream.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/crc.h>
#include <libavutil/imgutils.h>
}

namespace
{
struct Memory
{
    std::vector<std::uint8_t> bytes, transport;
    std::size_t at = 0, piece = 4096;
    bool stop = false, reject_output = false;
    bool vp9_output = false, stop_after_vp9 = false;
    std::vector<iptv::MediaVp9Packet> vp9;
    unsigned seeks = 0;
    iptv::Subtitles *subtitles = nullptr;
    unsigned subtitle_language = 0;
    std::string url;
    unsigned opened = 0, closed = 0, ranges = 0;
    Memory *owner = nullptr;
    std::vector<iptv::MediaAudioTrack> audio_tracks;
    std::vector<std::int64_t> subtitle_times;
    iptv::MediaTimeline timeline;
    std::vector<std::pair<unsigned, std::int64_t>> seek_script;
    bool take_seeks = true, seek_outputs = true, fail_seeks_after_open = false, armed = false;
    bool report_failures = true;
    unsigned asked = 0, armings = 0, begins = 0;
    std::size_t seek_after_cues = 0;
    std::size_t seek_after_bytes = 0, read_bytes = 0;
    bool waiting = false, refuse_waiting_seek = false;
    unsigned ended_reads = 0;
    std::vector<std::int64_t> committed, failed;
    std::vector<std::size_t> segments, subtitle_segments;
    explicit Memory(const char *name)
    {
        std::ifstream file(std::string("build/media-tests/fixtures/") + name, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), {});
        if (std::string_view(name).ends_with("master.m3u8"))
            url = std::string("https://fixture.test/") + name;
    }
    static int read(void *self, std::uint8_t *out, int cap)
    {
        auto &m = *static_cast<Memory *>(self);
        const auto count =
            std::min({m.bytes.size() - m.at, m.piece, static_cast<std::size_t>(cap)});
        auto &root = m.owner ? *m.owner : m;
        if (root.armed && root.seek_after_bytes && !root.waiting && !root.seek_script.empty() &&
            root.read_bytes + count >= root.seek_after_bytes)
        {
            root.waiting = true;
            ++root.ended_reads;
            return -1;
        }
        if (root.armed)
            root.read_bytes += count;
        if (count)
            std::memcpy(out, m.bytes.data() + m.at, count);
        m.at += count;
        return static_cast<int>(count);
    }
    int run(std::string *error, std::int64_t start_us = 0)
    {
        iptv::MediaInput input{
            this, read,
            [](void *self, std::int64_t offset)
            {
                auto &m = *static_cast<Memory *>(self);
                if (offset < 0 || static_cast<std::uint64_t>(offset) > m.bytes.size() ||
                    (m.fail_seeks_after_open && m.asked))
                    return std::int64_t(-1);
                ++m.seeks;
                m.at = static_cast<std::size_t>(offset);
                return offset;
            },
            [](void *self)
            { return static_cast<std::int64_t>(static_cast<Memory *>(self)->bytes.size()); },
            [](void *self) { return static_cast<Memory *>(self)->stop; }};
        if (!url.empty())
        {
            input.url = url.c_str();
            input.open_resource = [](void *self, const char *url, std::int64_t begin,
                                     std::int64_t end, iptv::MediaInput *resource)
            {
                auto &m = *static_cast<Memory *>(self);
                const std::string_view address(url), prefix("https://fixture.test/");
                if (begin || end >= 0)
                    ++m.ranges;
                if (!address.starts_with(prefix) || address.find("..") != std::string_view::npos)
                    return false;
                auto path = std::string(address.substr(prefix.size()));
                const bool redirect = path == "redirected-video.m3u8";
                if (redirect)
                    path = "hls-mpegts/video.m3u8";
                auto child = std::make_unique<Memory>(path.c_str());
                if (child->bytes.empty() || begin < 0 ||
                    static_cast<std::uint64_t>(begin) > child->bytes.size())
                    return false;
                if (end >= 0 && static_cast<std::uint64_t>(end) < child->bytes.size())
                    child->bytes.resize(static_cast<std::size_t>(end));
                child->at = static_cast<std::size_t>(begin);
                child->owner = &m;
                child->piece = m.piece;
                if (redirect)
                {
                    child->url = std::string(prefix) + path;
                    resource->url = child->url.c_str();
                }
                resource->read = read;
                resource->close = [](void *self)
                {
                    auto *child = static_cast<Memory *>(self);
                    ++child->owner->closed;
                    delete child;
                };
                resource->context = child.release();
                ++m.opened;
                return true;
            };
        }
        iptv::MediaOutput output{
            this,
            [](void *self, const std::uint8_t *bytes, std::size_t count)
            {
                auto &m = *static_cast<Memory *>(self);
                if (m.reject_output)
                    return false;
                m.transport.insert(m.transport.end(), bytes, bytes + count);
                return true;
            },
            [](void *self, const std::vector<iptv::SubtitleTrack> &tracks)
            {
                auto &m = *static_cast<Memory *>(self);
                if (!m.subtitles)
                    return;
                m.subtitles->set_tracks(tracks);
                ASSERT_LT(m.subtitle_language, tracks.size());
                EXPECT_TRUE(m.subtitles->select(tracks[m.subtitle_language].info.id));
            },
            [](void *self, std::uint32_t id, const std::uint8_t *bytes, std::size_t count,
               std::int64_t pts, std::int64_t duration)
            {
                auto &m = *static_cast<Memory *>(self);
                m.subtitle_times.push_back(pts);
                if (m.subtitles)
                    EXPECT_TRUE(m.subtitles->push(id, bytes, count, pts, duration));
            },
            [](void *self, const std::vector<iptv::MediaAudioTrack> &tracks)
            { static_cast<Memory *>(self)->audio_tracks = tracks; },
            [](void *self, const iptv::MediaTimeline &timeline)
            { static_cast<Memory *>(self)->timeline = timeline; }};
        if (vp9_output)
            output.vp9_packet = [](void *self, const iptv::MediaVp9Packet &packet)
            {
                auto &m = *static_cast<Memory *>(self);
                EXPECT_NE(packet.data, nullptr);
                EXPECT_GT(packet.bytes, 0u);
                m.vp9.push_back(packet);
                m.vp9.back().data = nullptr;
                if (m.stop_after_vp9)
                    m.stop = true;
                return !m.reject_output;
            };
        if (!seek_script.empty() && take_seeks)
            input.take_seek = [](void *self)
            {
                auto &m = *static_cast<Memory *>(self);
                EXPECT_TRUE(m.armed);
                ++m.asked;
                if (m.seek_after_bytes)
                {
                    if (!m.waiting || m.refuse_waiting_seek)
                        return std::int64_t(-1);
                    m.waiting = false;
                    const auto target = m.seek_script.front().second;
                    m.seek_script.erase(m.seek_script.begin());
                    return target;
                }
                if (m.seek_script.empty() ||
                    (m.seek_after_cues ? m.subtitle_times.size() < m.seek_after_cues
                                       : m.asked < m.seek_script.front().first))
                    return std::int64_t(-1);
                const auto target = m.seek_script.front().second;
                m.seek_script.erase(m.seek_script.begin());
                return target;
            };
        if (seek_after_bytes)
            input.seek_waiting = [](void *self)
            {
                auto &m = *static_cast<Memory *>(self);
                return m.waiting && !m.seek_script.empty();
            };
        if (!seek_script.empty() && seek_outputs)
        {
            output.seek_armed = [](void *self, bool armed)
            {
                auto &m = *static_cast<Memory *>(self);
                EXPECT_NE(m.armed, armed);
                m.armed = armed;
                m.armings += armed;
            };
            output.seek_begin = [](void *self) { ++static_cast<Memory *>(self)->begins; };
            output.seek_commit = [](void *self, std::int64_t target)
            {
                auto &m = *static_cast<Memory *>(self);
                EXPECT_EQ(m.begins, m.committed.size() + m.failed.size() + 1);
                m.committed.push_back(target);
                m.segments.push_back(m.transport.size());
                m.subtitle_segments.push_back(m.subtitle_times.size());
                return true;
            };
            output.seek_failed = [](void *self, std::int64_t target)
            {
                auto &m = *static_cast<Memory *>(self);
                EXPECT_FALSE(m.armed);
                m.failed.push_back(target);
            };
            if (!report_failures)
                output.seek_failed = nullptr;
        }
        const int result = iptv::ReadMedia(input, output, error, start_us);
        EXPECT_FALSE(armed);
        return result;
    }
};
struct Frames
{
    iptv_stream_format_t format{};
    unsigned video = 0, audio = 0;
    std::uint64_t first_pts = UINT64_MAX, last_pts = 0;
};
void check_transport(const Memory &memory, bool hevc, unsigned expected_frames = 0,
                     Frames *decoded = nullptr)
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
        [](void *self, const std::uint8_t *bytes, std::size_t count, std::uint64_t pts)
    {
        EXPECT_GT(count, 4u);
        EXPECT_EQ(bytes[0], 0);
        EXPECT_EQ(bytes[1], 0);
        auto &frames = *static_cast<Frames *>(self);
        ++frames.video;
        frames.first_pts = std::min(frames.first_pts, pts);
        frames.last_pts = std::max(frames.last_pts, pts);
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
    EXPECT_EQ(frames.video, expected_frames ? expected_frames : hevc ? 5u : 25u);
    if (!hevc)
        EXPECT_GE(frames.audio, expected_frames ? 46u * expected_frames / 25u : 46u);
    if (expected_frames)
        EXPECT_NEAR(frames.last_pts - frames.first_pts, (expected_frames - 1) * 40000, 40000);
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
    if (decoded)
        *decoded = frames;
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
TEST(Media, SeeksToKeyframesAndReadsMovieChaptersOnTheRealDemuxPath)
{
    for (const auto *name : {"chapters.mp4", "chapters.mkv"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        ASSERT_FALSE(memory.bytes.empty());
        std::string error;
        ASSERT_EQ(memory.run(&error, 2400000), 0) << error;
        EXPECT_TRUE(memory.timeline.seekable);
        EXPECT_NEAR(memory.timeline.duration_us, 6000000, 30000);
        ASSERT_EQ(memory.timeline.chapters.size(), 2u);
        EXPECT_EQ(memory.timeline.chapters[0].title, "Opening");
        EXPECT_EQ(memory.timeline.chapters[1].title, "Second act");
        EXPECT_EQ(memory.timeline.chapters[1].position_us, 2000000);
        Frames frames;
        check_transport(memory, false, 100, &frames);
        EXPECT_NEAR(frames.first_pts, 4000000, 30000); // Two-second transport lead-in.
        EXPECT_NEAR(frames.last_pts, 7960000, 30000);
    }
    Memory invalid("chapters.mp4");
    std::string error;
    EXPECT_EQ(invalid.run(&error, -1), -1);
    EXPECT_TRUE(invalid.transport.empty());
}

TEST(Media, SeeksFiniteTransportAndHlsWithTheirOriginalClocks)
{
    for (const auto *name : {"seek.ts", "hls-seek/master.m3u8"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        std::string error;
        ASSERT_EQ(memory.run(&error, 2400000), 0) << error;
        EXPECT_TRUE(memory.timeline.seekable);
        EXPECT_NEAR(memory.timeline.duration_us, 6000000, 100000);
        Frames frames;
        check_transport(memory, false, 100, &frames);
        EXPECT_NEAR(frames.first_pts, 4000000, 100000);
        EXPECT_EQ(memory.opened, memory.closed);
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
void decode_picture(AVCodecID codec_id, const std::vector<std::uint8_t> &bytes,
                    std::vector<std::uint8_t> *pixels)
{
    const auto free_context = [](AVCodecContext *p) { avcodec_free_context(&p); };
    const auto free_packet = [](AVPacket *p) { av_packet_free(&p); };
    const auto free_frame = [](AVFrame *p) { av_frame_free(&p); };
    const auto *codec = avcodec_find_decoder(codec_id);
    ASSERT_NE(codec, nullptr);
    std::unique_ptr<AVCodecContext, decltype(free_context)> context(avcodec_alloc_context3(codec));
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc());
    ASSERT_TRUE(context && packet && frame);
    context->thread_count = 1;
    ASSERT_EQ(avcodec_open2(context.get(), codec, nullptr), 0);
    ASSERT_EQ(av_new_packet(packet.get(), static_cast<int>(bytes.size())), 0);
    std::memcpy(packet->data, bytes.data(), bytes.size());
    ASSERT_EQ(avcodec_send_packet(context.get(), packet.get()), 0);
    auto result = avcodec_receive_frame(context.get(), frame.get());
    if (result == AVERROR(EAGAIN))
    {
        ASSERT_EQ(avcodec_send_packet(context.get(), nullptr), 0);
        result = avcodec_receive_frame(context.get(), frame.get());
    }
    ASSERT_EQ(result, 0);
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const int size = av_image_get_buffer_size(format, frame->width, frame->height, 1);
    ASSERT_GT(size, 0);
    pixels->resize(static_cast<std::size_t>(size));
    ASSERT_EQ(av_image_copy_to_buffer(pixels->data(), size, frame->data, frame->linesize, format,
                                      frame->width, frame->height, 1),
              size);
}

TEST(Media, InterlacedTransportPreservesDecodableAccessUnits)
{
    for (const auto *name : {"fields-0.ts", "fields-1.ts", "fields-2.ts"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        ASSERT_FALSE(memory.bytes.empty());
        std::vector<std::vector<std::uint8_t>> pictures;
        iptv_stream_backend_t backend{};
        backend.context = &pictures;
        backend.open = [](void *, const iptv_stream_format_t *) { return 0; };
        backend.submit_audio = [](void *, const std::uint8_t *, std::size_t, std::uint64_t)
        { return 0; };
        backend.submit_video =
            [](void *self, const std::uint8_t *data, std::size_t size, std::uint64_t)
        {
            static_cast<decltype(pictures) *>(self)->emplace_back(data, data + size);
            return 0;
        };
        backend.disable_audio = backend.drain = backend.discontinuity = [](void *) { return 0; };
        backend.close = [](void *) {};
        auto session = std::make_unique<iptv_stream_session_t>();
        iptv_stream_init(session.get());
        ASSERT_EQ(iptv_stream_open(session.get(), nullptr, &backend), IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_start(session.get()), IPTV_STREAM_OK);
        for (std::size_t at = 0; at < memory.bytes.size(); at += 137)
            ASSERT_EQ(iptv_stream_push(session.get(), memory.bytes.data() + at,
                                       std::min(std::size_t(137), memory.bytes.size() - at)),
                      IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_stop(session.get()), IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_cleanup(session.get()), IPTV_STREAM_OK);
        ASSERT_EQ(pictures.size(), 25u);
        std::vector<std::uint8_t> pixels;
        decode_picture(AV_CODEC_ID_H264, pictures.front(), &pixels);
        EXPECT_EQ(pixels.size(), 160u * 96u * 3u / 2u);
    }
}

TEST(Media, SeekRestoresConfigurationForFreshH264AndHevcDecoders)
{
    for (const auto mode : {0, 1, 2, 3, 4, 5})
    {
        const bool independent_download = mode >= 2;
        const bool fresh_playback = mode >= 4;
        const bool hevc = mode % 2;
        SCOPED_TRACE(fresh_playback ? "fresh playback parser" : "existing playback parser");
        SCOPED_TRACE(independent_download ? "download parser" : "playback parser");
        SCOPED_TRACE(hevc ? "HEVC" : "H264");
        Memory memory(hevc ? "hevc.mp4" : "h264-aac.mp4");
        std::string error;
        ASSERT_EQ(memory.run(&error), 0) << error;
        std::vector<std::vector<std::uint8_t>> pictures;
        std::vector<std::uint64_t> timestamps;
        struct Capture
        {
            decltype(pictures) &bytes;
            decltype(timestamps) &pts;
        } capture{pictures, timestamps};
        iptv_stream_backend_t backend{};
        backend.context = &capture;
        backend.open = [](void *, const iptv_stream_format_t *) { return 0; };
        backend.submit_video =
            [](void *self, const std::uint8_t *data, std::size_t size, std::uint64_t pts)
        {
            auto &capture = *static_cast<Capture *>(self);
            capture.bytes.emplace_back(data, data + size);
            capture.pts.push_back(pts);
            return 0;
        };
        backend.submit_audio = [](void *, const std::uint8_t *, std::size_t, std::uint64_t)
        { return 0; };
        backend.disable_audio = backend.drain = backend.discontinuity = [](void *) { return 0; };
        backend.close = [](void *) {};
        const auto close = [](iptv_stream_session_t *p)
        {
            (void)iptv_stream_cleanup(p);
            delete p;
        };
        std::unique_ptr<iptv_stream_session_t, decltype(close)> session(
            new iptv_stream_session_t{});
        iptv_stream_init(session.get());
        ASSERT_EQ(iptv_stream_open(session.get(), nullptr, &backend), IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_start(session.get()), IPTV_STREAM_OK);
        std::unique_ptr<iptv_stream_session_t, decltype(close)> download(
            new iptv_stream_session_t{});
        iptv_stream_init(download.get());
        ASSERT_EQ(iptv_stream_open(download.get(), nullptr, nullptr), IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_start(download.get()), IPTV_STREAM_OK);
        ASSERT_EQ(
            iptv_stream_scan(download.get(), memory.transport.data(), memory.transport.size(), 1),
            IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_push(session.get(), memory.transport.data(), memory.transport.size()),
                  IPTV_STREAM_OK);
        ASSERT_FALSE(pictures.empty());
        const std::vector<std::uint8_t> original_configuration = pictures.front();
        const auto original_pts = timestamps.front();
        Memory changed(hevc ? "hevc-config.mp4" : "h264-config.mp4");
        ASSERT_EQ(changed.run(&error), 0) << error;
        // Put the second encoder configuration later on the same transport
        // timeline. Change both PTS and DTS, preserving the wrapping encoding.
        for (std::size_t at = 0; at + 188 <= changed.transport.size(); at += 188)
        {
            auto *p = changed.transport.data() + at;
            if (!(p[1] & 0x40) || !(p[3] & 0x10))
                continue;
            const std::size_t pes = 4 + ((p[3] & 0x20) ? 1 + p[4] : 0);
            if (pes + 19 > 188 || p[pes] || p[pes + 1] || p[pes + 2] != 1 || !(p[pes + 7] & 0x80))
                continue;
            for (unsigned field = 0; field < ((p[pes + 7] & 0x40) ? 2u : 1u); ++field)
            {
                auto *t = p + pes + 9 + 5 * field;
                const auto ticks =
                    (((std::uint64_t(t[0] & 14) << 29) | (std::uint64_t(t[1]) << 22) |
                      (std::uint64_t(t[2] & 254) << 14) | (std::uint64_t(t[3]) << 7) |
                      (t[4] >> 1)) +
                     900000) &
                    ((UINT64_C(1) << 33) - 1);
                t[0] = (t[0] & 0xf1) | ((ticks >> 29) & 14);
                t[1] = ticks >> 22;
                t[2] = 1 | ((ticks >> 14) & 254);
                t[3] = ticks >> 7;
                t[4] = 1 | ((ticks << 1) & 254);
            }
        }
        pictures.clear();
        timestamps.clear();
        // Capture the second encoder's reference picture independently. Only
        // the selected history parser sees the skipped configuration change.
        std::unique_ptr<iptv_stream_session_t, decltype(close)> reference(
            new iptv_stream_session_t{});
        iptv_stream_init(reference.get());
        ASSERT_EQ(iptv_stream_open(reference.get(), nullptr, &backend), IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_start(reference.get()), IPTV_STREAM_OK);
        ASSERT_EQ(
            iptv_stream_push(reference.get(), changed.transport.data(), changed.transport.size()),
            IPTV_STREAM_OK);
        ASSERT_FALSE(pictures.empty());
        const std::array versions{original_configuration, pictures.front()};
        const std::array positions{original_pts, timestamps.front()};
        ASSERT_GT(positions[1], positions[0] + 1000000);
        reference.reset();
        pictures.clear();
        timestamps.clear();
        for (std::size_t at = 0; at < changed.transport.size();)
        {
            const auto bytes = std::min<std::size_t>(157, changed.transport.size() - at);
            ASSERT_EQ(iptv_stream_scan(independent_download ? download.get() : session.get(),
                                       changed.transport.data() + at, bytes,
                                       at + bytes == changed.transport.size()),
                      IPTV_STREAM_OK);
            at += bytes;
        }
        EXPECT_TRUE(pictures.empty());
        std::array<std::vector<std::uint8_t>, 2> parameter_sets;
        for (const unsigned version : {0u, 1u, 0u, 1u})
        {
            SCOPED_TRACE(version);
            const auto &configured = versions[version];
            std::vector<std::uint8_t> stripped;
            parameter_sets[version].clear();
            // Remove only parameter NALs from the real encoded keyframe. A fresh
            // decoder must receive their retained bytes from the application parser.
            const auto start_code = [&](std::size_t at, std::size_t *prefix)
            {
                for (; at + 3 <= configured.size(); ++at)
                    if (!configured[at] && !configured[at + 1])
                    {
                        if (configured[at + 2] == 1)
                        {
                            *prefix = 3;
                            return at;
                        }
                        if (at + 4 <= configured.size() && !configured[at + 2] &&
                            configured[at + 3] == 1)
                        {
                            *prefix = 4;
                            return at;
                        }
                    }
                return configured.size();
            };
            for (std::size_t at = 0, prefix = 0; at < configured.size();)
            {
                at = start_code(at, &prefix);
                ASSERT_LT(at + prefix, configured.size());
                std::size_t next_prefix = 0;
                const auto next = start_code(at + prefix, &next_prefix);
                const auto type =
                    hevc ? (configured[at + prefix] >> 1) & 0x3f : configured[at + prefix] & 0x1f;
                if (!(hevc ? type >= 32 && type <= 34 : type == 7 || type == 8))
                    stripped.insert(stripped.end(), configured.begin() + at,
                                    configured.begin() + next);
                else
                    parameter_sets[version].insert(parameter_sets[version].end(),
                                                   configured.begin() + at,
                                                   configured.begin() + next);
                at = next;
            }
            ASSERT_LT(stripped.size(), configured.size());
            if (fresh_playback)
            {
                session.reset(new iptv_stream_session_t{});
                iptv_stream_init(session.get());
                ASSERT_EQ(iptv_stream_open(session.get(), nullptr, &backend), IPTV_STREAM_OK);
                ASSERT_EQ(iptv_stream_start(session.get()), IPTV_STREAM_OK);
            }
            ASSERT_EQ(iptv_stream_reposition_from(
                          session.get(), independent_download ? download.get() : session.get(),
                          positions[version]),
                      IPTV_STREAM_OK);
            pictures.clear();
            const auto pid = session->telemetry.format.video_pid;
            unsigned counter = 0;
            for (unsigned repeat = 0; repeat < 3; ++repeat)
            {
                std::vector<std::uint8_t> pes{0, 0, 1, 0xe0, 0, 0, 0x80, 0, 0};
                pes.insert(pes.end(), stripped.begin(), stripped.end());
                for (std::size_t at = 0; at < pes.size();)
                {
                    const auto count = std::min<std::size_t>(184, pes.size() - at);
                    std::array<std::uint8_t, 188> packet;
                    packet.fill(0xff);
                    packet[0] = 0x47;
                    packet[1] = (pid >> 8) | (at ? 0 : 0x40);
                    packet[2] = pid;
                    packet[3] = (count == 184 ? 0x10 : 0x30) | (counter++ & 15);
                    if (count < 184)
                    {
                        packet[4] = 183 - count;
                        if (packet[4])
                            packet[5] = 0;
                    }
                    std::memcpy(packet.data() + 188 - count, pes.data() + at, count);
                    ASSERT_EQ(iptv_stream_push(session.get(), packet.data(), packet.size()),
                              IPTV_STREAM_OK);
                    at += count;
                }
            }
            ASSERT_FALSE(pictures.empty());
            const auto codec = hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
            std::vector<std::uint8_t> original, replay;
            decode_picture(codec, configured, &original);
            decode_picture(codec, pictures.front(), &replay);
            ASSERT_FALSE(original.empty());
            EXPECT_EQ(replay, original);
        }
        EXPECT_NE(parameter_sets[0], parameter_sets[1]);
    }
}

TEST(Media, ReadsMoviesWithManySubtitleLanguagesButRetainsAStreamLimit)
{
    std::string error;
    for (const auto *name : {"many-subtitles-40.mkv", "many-subtitles-40.mp4"})
    {
        SCOPED_TRACE(name);
        Memory movie(name);
        ASSERT_FALSE(movie.bytes.empty());
        ASSERT_EQ(movie.run(&error), 0) << error;
        check_transport(movie, false);
    }
    Memory excessive("many-subtitles-128.mkv");
    ASSERT_FALSE(excessive.bytes.empty());
    EXPECT_EQ(excessive.run(&error), -1);
    EXPECT_FALSE(error.empty());
}
TEST(Media, ReadsReorderedHevcWhenParameterSetsArriveInTheFirstVideoPacket)
{
    Memory movie("hevc-inband.mkv");
    // Keep the length-prefix configuration, but make the demuxer learn the
    // parameter sets (and reorder delay) from the video instead of CodecPrivate.
    const std::uint8_t codec_private[] = {0x63, 0xa2};
    const auto found = std::search(movie.bytes.begin(), movie.bytes.end(),
                                   std::begin(codec_private), std::end(codec_private));
    ASSERT_NE(found, movie.bytes.end());
    auto at = static_cast<std::size_t>(found - movie.bytes.begin()) + 2;
    ASSERT_LT(at, movie.bytes.size());
    unsigned size_bytes = 1;
    for (unsigned mask = 0x80; mask && !(movie.bytes[at] & mask); mask >>= 1)
        ++size_bytes;
    ASSERT_LE(size_bytes, 8u);
    at += size_bytes;
    ASSERT_LT(at + 22, movie.bytes.size());
    ASSERT_EQ(movie.bytes[at], 1); // HEVC configuration record.
    movie.bytes[at + 22] = 0;      // No out-of-band parameter-set arrays.
    std::string error;
    ASSERT_EQ(movie.run(&error), 0) << error;
    check_transport(movie, true);
}
TEST(Media, PreservesBothLanguagesAndSwitchesRealAudioWithoutReopeningVideo)
{
    for (const auto *name : {"two-audio.mp4", "two-audio.mkv", "hls-mpegts/master.m3u8",
                             "hls-fmp4/master.m3u8", "hls-ranged/master.m3u8"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        ASSERT_FALSE(memory.bytes.empty());
        std::string error;
        ASSERT_EQ(memory.run(&error), 0) << error;
        EXPECT_EQ(memory.opened, memory.closed);
        if (!memory.url.empty())
            EXPECT_GE(memory.opened, 6u);
        if (memory.url.find("hls-ranged") != memory.url.npos)
            EXPECT_GE(memory.ranges, 6u);
        struct Counts
        {
            unsigned opens = 0, videos = 0, audios = 0, switches = 0;
        } counts;
        iptv_stream_backend_t backend{};
        backend.context = &counts;
        backend.open = [](void *p, const iptv_stream_format_t *)
        {
            ++static_cast<Counts *>(p)->opens;
            return 0;
        };
        backend.submit_video = [](void *p, const std::uint8_t *, std::size_t, std::uint64_t)
        {
            ++static_cast<Counts *>(p)->videos;
            return 0;
        };
        backend.submit_audio = [](void *p, const std::uint8_t *, std::size_t, std::uint64_t)
        {
            ++static_cast<Counts *>(p)->audios;
            return 0;
        };
        backend.select_audio = [](void *p, std::uint32_t)
        {
            ++static_cast<Counts *>(p)->switches;
            return 0;
        };
        backend.disable_audio = [](void *) { return 0; };
        backend.drain = [](void *) { return 0; };
        backend.close = [](void *) {};
        iptv_stream_session_t session{};
        iptv_stream_init(&session);
        ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), 0);
        ASSERT_EQ(iptv_stream_start(&session), 0);
        bool switched = false;
        for (std::size_t at = 0; at < memory.transport.size(); at += 188)
        {
            ASSERT_EQ(iptv_stream_push(&session, memory.transport.data() + at,
                                       std::min<std::size_t>(188, memory.transport.size() - at)),
                      0)
                << session.telemetry.last_error;
            if (counts.audios >= 5 && !switched)
            {
                iptv_stream_audio_track_t tracks[2]{};
                std::uint32_t selected = 0;
                ASSERT_EQ(iptv_stream_audio_tracks(&session, tracks, 2, &selected), 2u);
                EXPECT_STREQ(tracks[0].language, "eng");
                EXPECT_STREQ(tracks[1].language, "spa");
                EXPECT_EQ(selected, tracks[0].pid);
                ASSERT_EQ(iptv_stream_select_audio(&session, tracks[1].pid), 0);
                switched = true;
            }
        }
        EXPECT_TRUE(switched);
        EXPECT_EQ(iptv_stream_stop(&session), 0);
        EXPECT_EQ(counts.opens, 1u);
        EXPECT_EQ(counts.videos, 25u);
        EXPECT_GT(counts.audios, 25u);
        EXPECT_EQ(counts.switches, 1u);
        EXPECT_EQ(session.telemetry.continuity_errors, 0u);
        EXPECT_EQ(iptv_stream_cleanup(&session), 0);
    }
}
TEST(Media, DtsSoundIsLabelledSoTheTransportParserPlaysIt)
{
    for (const auto *name : {"h264-dts.mkv", "h264-dts.mp4"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        ASSERT_FALSE(memory.bytes.empty());
        std::string error;
        ASSERT_EQ(memory.run(&error), 0) << error;
        // Every programme table carries the registration once, with a valid CRC.
        unsigned tables = 0;
        for (std::size_t at = 0; at + 188 <= memory.transport.size(); at += 188)
        {
            const auto *packet = memory.transport.data() + at;
            const unsigned pid = static_cast<unsigned>((packet[1] & 0x1f) << 8 | packet[2]);
            if (pid <= 0x1f || !(packet[1] & 0x40) || packet[5] != 0x02)
                continue;
            ++tables;
            const auto *section = packet + 5;
            const std::size_t length =
                static_cast<std::size_t>((section[1] & 0x0f) << 8 | section[2]);
            const std::size_t info =
                static_cast<std::size_t>((section[10] & 0x0f) << 8 | section[11]);
            EXPECT_EQ(info, 6u);
            EXPECT_EQ(std::memcmp(section + 12, "\x05\x04HDMV", 6), 0);
            const std::uint32_t crc = av_crc(av_crc_get_table(AV_CRC_32_IEEE), UINT32_MAX, section,
                                             static_cast<int>(length - 1));
            for (unsigned i = 0; i < 4; ++i)
                EXPECT_EQ(section[length - 1 + i], static_cast<std::uint8_t>(crc >> (8 * i)));
        }
        EXPECT_GE(tables, 1u);
        struct Counts
        {
            unsigned videos = 0, audios = 0;
            std::uint32_t audio_type = 0;
        } counts;
        iptv_stream_backend_t backend{};
        backend.context = &counts;
        backend.open = [](void *p, const iptv_stream_format_t *format)
        {
            static_cast<Counts *>(p)->audio_type = format->audio_stream_type;
            return 0;
        };
        backend.submit_video = [](void *p, const std::uint8_t *, std::size_t, std::uint64_t)
        {
            ++static_cast<Counts *>(p)->videos;
            return 0;
        };
        backend.submit_audio = [](void *p, const std::uint8_t *, std::size_t, std::uint64_t)
        {
            ++static_cast<Counts *>(p)->audios;
            return 0;
        };
        backend.select_audio = [](void *, std::uint32_t) { return 0; };
        backend.disable_audio = [](void *) { return 0; };
        backend.drain = [](void *) { return 0; };
        backend.close = [](void *) {};
        iptv_stream_session_t session{};
        iptv_stream_init(&session);
        ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), 0);
        ASSERT_EQ(iptv_stream_start(&session), 0);
        for (std::size_t at = 0; at < memory.transport.size(); at += 188)
            ASSERT_EQ(iptv_stream_push(&session, memory.transport.data() + at,
                                       std::min<std::size_t>(188, memory.transport.size() - at)),
                      0)
                << session.telemetry.last_error;
        iptv_stream_audio_track_t tracks[2]{};
        std::uint32_t selected = 0;
        ASSERT_EQ(iptv_stream_audio_tracks(&session, tracks, 2, &selected), 1u);
        EXPECT_EQ(tracks[0].stream_type, 0x82u);
        EXPECT_EQ(iptv_stream_stop(&session), 0);
        EXPECT_EQ(counts.audio_type, 0x82u);
        EXPECT_EQ(counts.videos, 25u);
        EXPECT_GT(counts.audios, 25u);
        EXPECT_EQ(session.telemetry.continuity_errors, 0u);
        EXPECT_EQ(iptv_stream_cleanup(&session), 0);
    }
}
TEST(Media, EmbeddedSubtitlesUseTheSameTimelineAsRemuxedVideo)
{
    for (const auto *name : {"subtitles.mp4", "subtitles.mkv", "hls-mpegts/subtitles-master.m3u8",
                             "hls-fmp4/subtitles-master.m3u8", "hls-ranged/subtitles-master.m3u8"})
        for (unsigned language = 0; language < 2; ++language)
        {
            SCOPED_TRACE(name);
            SCOPED_TRACE(language);
            iptv::Subtitles subtitles;
            Memory memory(name);
            memory.subtitles = &subtitles;
            memory.piece = 7; // Sniffed WebVTT headers may span HTTP reads.
            memory.subtitle_language = language;
            std::string error;
            ASSERT_FALSE(memory.bytes.empty());
            ASSERT_EQ(memory.run(&error), 0) << error;
            EXPECT_EQ(memory.opened, memory.closed);
            if (!memory.url.empty())
            {
                ASSERT_EQ(memory.audio_tracks.size(), 2u);
                EXPECT_EQ(memory.audio_tracks[0].pid, 0x101u);
                EXPECT_EQ(memory.audio_tracks[0].language, "en-US");
                EXPECT_EQ(memory.audio_tracks[0].title, "English");
                EXPECT_EQ(memory.audio_tracks[1].language, "es");
                EXPECT_EQ(memory.audio_tracks[1].title, "Español");
                EXPECT_EQ(memory.audio_tracks[1].audio_type, 3u);
            }
            const auto state = subtitles.state();
            ASSERT_EQ(state.tracks.size(), 2u);
            EXPECT_EQ(state.tracks[0].language, "eng");
            EXPECT_EQ(state.tracks[1].language, "spa");
            EXPECT_EQ(state.selected, state.tracks[language].id);
            if (!memory.url.empty())
            {
                EXPECT_EQ(state.tracks[0].title, "English CC");
                EXPECT_TRUE(state.tracks[0].hearing_impaired);
                EXPECT_TRUE(state.tracks[1].forced);
            }
            std::uint64_t first_pts = IPTV_STREAM_PTS_UNKNOWN;
            iptv_stream_backend_t backend{};
            backend.context = &first_pts;
            backend.open = [](void *, const iptv_stream_format_t *) { return 0; };
            backend.submit_video =
                [](void *self, const std::uint8_t *, std::size_t, std::uint64_t pts)
            {
                auto &first = *static_cast<std::uint64_t *>(self);
                if (first == IPTV_STREAM_PTS_UNKNOWN)
                    first = pts;
                return 0;
            };
            backend.submit_audio = [](void *, const std::uint8_t *, std::size_t, std::uint64_t)
            { return 0; };
            backend.disable_audio = [](void *) { return 0; };
            backend.drain = [](void *) { return 0; };
            backend.close = [](void *) {};
            backend.subtitle_tracks = [](void *, const iptv_stream_subtitle_track_t *, std::size_t)
            { ADD_FAILURE() << "An empty remux PMT must not replace container subtitle tracks"; };
            iptv_stream_session_t session{};
            iptv_stream_init(&session);
            ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), 0);
            ASSERT_EQ(iptv_stream_start(&session), 0);
            ASSERT_EQ(iptv_stream_push(&session, memory.transport.data(), memory.transport.size()),
                      0);
            ASSERT_EQ(iptv_stream_stop(&session), 0);
            EXPECT_EQ(iptv_stream_cleanup(&session), 0);
            ASSERT_NE(first_pts, IPTV_STREAM_PTS_UNKNOWN);
            SCOPED_TRACE("video first pts=" + std::to_string(first_pts));
            SCOPED_TRACE(::testing::PrintToString(memory.subtitle_times));
            EXPECT_TRUE(subtitles.at(first_pts).empty());
            const auto first = subtitles.at(first_pts + 250000);
            ASSERT_EQ(first.size(), 1u);
            EXPECT_EQ(first[0]->text, language ? "Hola, mundo!" : "Hello, world!");
            EXPECT_TRUE(subtitles.at(first_pts + 625000).empty());
            const auto second = subtitles.at(first_pts + 750000);
            ASSERT_EQ(second.size(), 1u);
            EXPECT_EQ(second[0]->text, language ? "Otra línea" : "Second line");
            EXPECT_TRUE(subtitles.at(first_pts + 1000000).empty());
        }
}
TEST(Media, HlsProviderClockResetKeepsVideoAndExternalSubtitlesPlayable)
{
    iptv::Subtitles subtitles;
    Memory memory("hls-reset/master.m3u8");
    memory.subtitles = &subtitles;
    std::string error;
    const auto result = memory.run(&error);
    SCOPED_TRACE(::testing::PrintToString(memory.subtitle_times));
    ASSERT_EQ(result, 0) << error;
    EXPECT_EQ(memory.opened, memory.closed);
    Frames frames;
    check_transport(memory, false, 50, &frames);
    ASSERT_EQ(memory.subtitle_times.size(), 2u);
    EXPECT_NEAR(memory.subtitle_times[1] - memory.subtitle_times[0], 1000000, 40000);
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto cues = subtitles.at(frames.first_pts + 250000 + i * 1000000);
        ASSERT_EQ(cues.size(), 1u);
        EXPECT_EQ(cues.front()->text, i ? "new timeline" : "old timeline");
    }
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
TEST(Media, MapsWebVttClocksAndPreservesCueTextAcrossClockWrap)
{
    std::string result;
    ASSERT_TRUE(iptv::NormalizeHlsWebVtt(
        "\xef\xbb\xbfWEBVTT\r\nX-TIMESTAMP-MAP=MPEGTS:180000,LOCAL:00:10.000\r\n\r\n"
        "NOTE ignored --> comment\r\n\r\nidentifier\r\n00:10.250 --> 00:10.750 align:start\r\n"
        "A --> B\r\n中文\r\n",
        result));
    EXPECT_NE(result.find("00:00:02.250 --> 00:00:02.750 align:start"), result.npos);
    EXPECT_NE(result.find("A --> B\n中文\n"), result.npos);
    EXPECT_EQ(result.find("X-TIMESTAMP-MAP"), result.npos);
    ASSERT_TRUE(
        iptv::NormalizeHlsWebVtt("WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00.000,MPEGTS:8589916592\n\n"
                                 "00:00.250 --> 00:00.750\nAfter wrap\n",
                                 result));
    EXPECT_NE(result.find("00:00:00.050 --> 00:00:00.550"), result.npos);
    ASSERT_TRUE(iptv::NormalizeHlsWebVtt("WEBVTT\n\n00:00.250 --> 00:00.750\nNo map\n", result));
    EXPECT_NE(result.find("00:00:00.250 --> 00:00:00.750"), result.npos);
    for (const auto *invalid :
         {"WEBVTTx\n\n", "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00.000,MPEGTS:8589934592\n\n",
          "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00.000\n\n",
          "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00.000,MPEGTS:-1\n\n",
          "WEBVTT\n\n00:61.000 --> 00:62.000\nBad seconds\n",
          "WEBVTT\n\n00:01.000 --> 00:00.000\nBackwards\n"})
        EXPECT_FALSE(iptv::NormalizeHlsWebVtt(invalid, result)) << invalid;
    EXPECT_FALSE(iptv::NormalizeHlsWebVtt(std::string(1024 * 1024 + 1, 'x'), result));
}
TEST(Media, HlsClosesEveryResourceOnFailureAndNeverOpensLocalPaths)
{
    std::string error;
    Memory failed("hls-fmp4/master.m3u8");
    failed.reject_output = true;
    EXPECT_EQ(failed.run(&error), -1);
    EXPECT_GT(failed.opened, 0u);
    EXPECT_EQ(failed.opened, failed.closed);
    Memory denied("hls-mpegts/master.m3u8");
    const std::string manifest =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000\nfile:///secret-name/credentials.ts\n";
    denied.bytes.assign(manifest.begin(), manifest.end());
    EXPECT_EQ(denied.run(&error), -1);
    EXPECT_EQ(denied.opened, 0u);
    EXPECT_EQ(error.find("secret-name"), error.npos);
    EXPECT_EQ(error.find("credentials"), error.npos);
}
TEST(Media, HlsResolvesSegmentsAgainstTheRedirectedPlaylist)
{
    Memory memory("hls-mpegts/master.m3u8");
    std::string manifest(memory.bytes.begin(), memory.bytes.end());
    const auto at = manifest.find("video.m3u8");
    ASSERT_NE(at, manifest.npos);
    manifest.replace(at, 10, "https://fixture.test/redirected-video.m3u8");
    memory.bytes.assign(manifest.begin(), manifest.end());
    std::string error;
    ASSERT_EQ(memory.run(&error), 0) << error;
    EXPECT_EQ(memory.opened, memory.closed);
    check_transport(memory, false);
}
struct Segment
{
    std::vector<std::uint64_t> pts;
    std::vector<std::uint8_t> first_picture;
    unsigned audio = 0;
};
struct Played
{
    std::vector<Segment> segments;
    unsigned opens = 0, discontinuities = 0;
    std::uint64_t continuity_errors = 0;
};
Played play_segments(const Memory &memory)
{
    Played played;
    played.segments.emplace_back();
    iptv_stream_backend_t backend{};
    backend.context = &played;
    backend.open = [](void *self, const iptv_stream_format_t *)
    {
        ++static_cast<Played *>(self)->opens;
        return 0;
    };
    backend.submit_video =
        [](void *self, const std::uint8_t *bytes, std::size_t count, std::uint64_t pts)
    {
        auto &segment = static_cast<Played *>(self)->segments.back();
        if (segment.pts.empty())
            segment.first_picture.assign(bytes, bytes + count);
        segment.pts.push_back(pts);
        return 0;
    };
    backend.submit_audio = [](void *self, const std::uint8_t *, std::size_t, std::uint64_t)
    {
        ++static_cast<Played *>(self)->segments.back().audio;
        return 0;
    };
    backend.disable_audio = backend.drain = [](void *) { return 0; };
    backend.discontinuity = [](void *self)
    {
        ++static_cast<Played *>(self)->discontinuities;
        return 0;
    };
    backend.close = [](void *) {};
    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    EXPECT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    EXPECT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
    std::vector<std::size_t> bounds = memory.segments;
    bounds.push_back(memory.transport.size());
    std::size_t at = 0;
    for (std::size_t i = 0; i < bounds.size(); ++i)
    {
        if (i)
        {
            EXPECT_EQ(iptv_stream_discontinuity(&session), IPTV_STREAM_OK);
            played.segments.emplace_back();
        }
        EXPECT_EQ(bounds[i] % 188, 0u);
        EXPECT_EQ(iptv_stream_push(&session, memory.transport.data() + at, bounds[i] - at),
                  IPTV_STREAM_OK)
            << session.telemetry.last_error;
        at = bounds[i];
    }
    EXPECT_EQ(iptv_stream_stop(&session), IPTV_STREAM_OK) << session.telemetry.last_error;
    played.continuity_errors = session.telemetry.continuity_errors;
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
    return played;
}
void expect_fresh_tables(const Memory &memory)
{
    std::vector<std::size_t> starts{0};
    starts.insert(starts.end(), memory.segments.begin(), memory.segments.end());
    for (const auto start : starts)
    {
        SCOPED_TRACE("transport at " + std::to_string(start));
        bool pat = false, pmt = false;
        for (std::size_t at = start; at < start + 4 * 188 && at + 188 <= memory.transport.size();
             at += 188)
        {
            const auto *p = memory.transport.data() + at;
            ASSERT_EQ(p[0], 0x47);
            const unsigned pid = (p[1] & 0x1f) << 8 | p[2];
            pat |= pid == 0 && (p[3] & 15) == 0;
            pmt |= pid == 0x1000 && (p[3] & 15) == 0;
        }
        EXPECT_TRUE(pat && pmt);
    }
}

void expect_keyframe_landing(std::uint64_t first_pts, std::int64_t target)
{
    const auto at = static_cast<std::uint64_t>(target) + 2000000;
    EXPECT_LE(first_pts, at + 25000);
    EXPECT_GT(first_pts + 1000000, at);
}

TEST(Media, SeeksInsideTheOpenVideoForwardAndBackOnEveryContainer)
{
    for (const auto *name : {"chapters.mp4", "chapters.mkv", "seek.ts", "hls-seek/master.m3u8"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        ASSERT_FALSE(memory.bytes.empty());
        memory.seek_script = {{40, 4400000}, {60, 400000}};
        std::string error;
        ASSERT_EQ(memory.run(&error, 1500000), 0) << error;
        EXPECT_EQ(memory.armings, 1u);
        EXPECT_EQ(memory.begins, 2u);
        EXPECT_EQ(memory.committed, (std::vector<std::int64_t>{4400000, 400000}));
        EXPECT_TRUE(memory.failed.empty());
        EXPECT_EQ(memory.opened, memory.closed);
        if (!memory.url.empty())
            EXPECT_GT(memory.opened, 2u);
        expect_fresh_tables(memory);
        const auto played = play_segments(memory);
        EXPECT_EQ(played.opens, 1u);
        EXPECT_EQ(played.discontinuities, 2u);
        EXPECT_EQ(played.continuity_errors, 0u);
        ASSERT_EQ(played.segments.size(), 3u);
        const std::uint64_t starts[] = {1500000, 4400000, 400000};
        for (std::size_t i = 0; i < played.segments.size(); ++i)
        {
            SCOPED_TRACE(i);
            const auto &segment = played.segments[i];
            ASSERT_FALSE(segment.pts.empty());
            EXPECT_GT(segment.audio, 0u);
            const auto first = *std::min_element(segment.pts.begin(), segment.pts.end());
            expect_keyframe_landing(first, starts[i]);
            EXPECT_EQ(segment.pts.front(), first);
            std::vector<std::uint8_t> pixels;
            decode_picture(AV_CODEC_ID_H264, segment.first_picture, &pixels);
            EXPECT_EQ(pixels.size(), 320u * 180u * 3u / 2u);
        }
        ASSERT_FALSE(played.segments.back().pts.empty());
        EXPECT_NEAR(played.segments.back().pts.size(), 140u, 25u);
        EXPECT_NEAR(
            *std::max_element(played.segments.back().pts.begin(), played.segments.back().pts.end()),
            7960000, 100000);
    }
}

TEST(Media, ASeekEndsTheReadInProgressInsteadOfWaitingForIt)
{
    for (const auto *name : {"chapters.mp4", "chapters.mkv", "seek.ts", "hls-seek/master.m3u8"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        ASSERT_FALSE(memory.bytes.empty());
        memory.piece = 4096;
        memory.seek_script = {{0, 4400000}};
        memory.seek_after_bytes = 20000;
        std::string error;
        ASSERT_EQ(memory.run(&error), 0) << error;
        EXPECT_EQ(memory.ended_reads, 1u);
        EXPECT_EQ(memory.committed, std::vector<std::int64_t>{4400000});
        EXPECT_TRUE(memory.failed.empty());
        EXPECT_EQ(memory.opened, memory.closed);
        expect_fresh_tables(memory);
        const auto played = play_segments(memory);
        EXPECT_EQ(played.opens, 1u);
        EXPECT_EQ(played.continuity_errors, 0u);
        ASSERT_EQ(played.segments.size(), 2u);
        ASSERT_FALSE(played.segments[1].pts.empty());
        expect_keyframe_landing(played.segments[1].pts.front(), 4400000);
        EXPECT_NEAR(played.segments[1].pts.back(), 7960000, 100000);
        std::vector<std::uint8_t> pixels;
        decode_picture(AV_CODEC_ID_H264, played.segments[1].first_picture, &pixels);
        EXPECT_FALSE(pixels.empty());
    }
    Memory reopen("chapters.mkv");
    reopen.seek_script = {{0, 4400000}};
    reopen.seek_after_bytes = 20000;
    reopen.refuse_waiting_seek = true;
    std::string error;
    EXPECT_EQ(reopen.run(&error), 1);
    EXPECT_EQ(reopen.ended_reads, 1u);
    EXPECT_TRUE(reopen.committed.empty());
}

TEST(Media, SeekToTheBeginningAndRapidSeeksStayDecodable)
{
    for (const auto *name : {"chapters.mp4", "seek.ts", "hls-seek/master.m3u8"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        memory.seek_script = {{30, 0}, {60, 3000000}, {61, 5500000}, {90, 2000000}};
        std::string error;
        ASSERT_EQ(memory.run(&error, 2400000), 0) << error;
        EXPECT_EQ(memory.committed, (std::vector<std::int64_t>{0, 3000000, 5500000, 2000000}));
        EXPECT_EQ(memory.opened, memory.closed);
        expect_fresh_tables(memory);
        const auto played = play_segments(memory);
        EXPECT_EQ(played.opens, 1u);
        EXPECT_EQ(played.continuity_errors, 0u);
        ASSERT_EQ(played.segments.size(), 5u);
        ASSERT_FALSE(played.segments[1].pts.empty());
        expect_keyframe_landing(played.segments[1].pts.front(), 0);
        ASSERT_FALSE(played.segments[3].pts.empty());
        expect_keyframe_landing(played.segments[3].pts.front(), 5500000);
        ASSERT_FALSE(played.segments.back().pts.empty());
        expect_keyframe_landing(played.segments.back().pts.front(), 2000000);
        EXPECT_NEAR(played.segments.back().pts.back(), 7960000, 100000);
        for (const auto &segment : played.segments)
            if (!segment.first_picture.empty())
            {
                std::vector<std::uint8_t> pixels;
                decode_picture(AV_CODEC_ID_H264, segment.first_picture, &pixels);
                EXPECT_FALSE(pixels.empty());
            }
    }
}

TEST(Media, SubtitlesAfterAnInlineSeekKeepTheVideoTimeline)
{
    for (const auto *name : {"subtitles.mp4", "subtitles.mkv", "hls-mpegts/subtitles-master.m3u8"})
    {
        SCOPED_TRACE(name);
        Memory memory(name);
        ASSERT_FALSE(memory.bytes.empty());
        iptv::Subtitles subtitles;
        memory.subtitles = &subtitles;
        memory.seek_script = {{50, 0}};
        std::string error;
        ASSERT_EQ(memory.run(&error), 0) << error;
        ASSERT_EQ(memory.committed.size(), 1u);
        EXPECT_EQ(memory.opened, memory.closed);
        SCOPED_TRACE(::testing::PrintToString(memory.subtitle_times));
        ASSERT_EQ(memory.subtitle_segments.size(), 1u);
        const auto split = memory.subtitle_segments[0];
        ASSERT_GE(split, 2u);
        const auto &times = memory.subtitle_times;
        const auto again = std::min(split, times.size() - split);
        ASSERT_GE(again, 2u);
        EXPECT_TRUE(std::equal(times.begin(), times.begin() + again, times.begin() + split));
        const auto played = play_segments(memory);
        ASSERT_EQ(played.segments.size(), 2u);
        ASSERT_FALSE(played.segments[1].pts.empty());
        const auto first_pts = played.segments[1].pts.front();
        EXPECT_TRUE(subtitles.at(first_pts).empty());
        const auto cues = subtitles.at(first_pts + 250000);
        ASSERT_EQ(cues.size(), 1u);
        EXPECT_EQ(cues[0]->text, "Hello, world!");
    }
}

TEST(Media, FailedInlineSeekAsksForAReopenAndNullCallbacksKeepTodaysPath)
{
    std::string error;
    Memory failing("seek.ts");
    failing.seek_script = {{5, 1000000}};
    failing.fail_seeks_after_open = true;
    EXPECT_EQ(failing.run(&error, 5000000), 1);
    EXPECT_EQ(failing.begins, 1u);
    EXPECT_TRUE(failing.committed.empty());
    EXPECT_EQ(failing.failed, std::vector<std::int64_t>{1000000});
    Memory unreported("seek.ts");
    unreported.seek_script = {{5, 1000000}};
    unreported.fail_seeks_after_open = true;
    unreported.report_failures = false;
    EXPECT_EQ(unreported.run(&error, 5000000), -1);
    EXPECT_EQ(error, "The video could not seek to that position.");
    Memory end("hls-seek/master.m3u8");
    end.seek_script = {{5, 5999999}};
    EXPECT_EQ(end.run(&error, 1000000), 1);
    EXPECT_TRUE(end.committed.empty());
    EXPECT_EQ(end.failed, std::vector<std::int64_t>{5999999});
    EXPECT_EQ(end.opened, end.closed);
    for (const auto *name : {"chapters.mp4", "chapters.mkv", "seek.ts", "hls-seek/master.m3u8"})
    {
        SCOPED_TRACE(name);
        Memory plain(name);
        ASSERT_EQ(plain.run(&error, 1000000), 0) << error;
        Memory no_input(name);
        no_input.seek_script = {{20, 4400000}};
        no_input.take_seeks = false;
        ASSERT_EQ(no_input.run(&error, 1000000), 0) << error;
        EXPECT_EQ(no_input.armings, 0u);
        EXPECT_EQ(no_input.transport, plain.transport);
        Memory no_output(name);
        no_output.seek_script = {{20, 4400000}};
        no_output.seek_outputs = false;
        ASSERT_EQ(no_output.run(&error, 1000000), 0) << error;
        EXPECT_EQ(no_output.asked, 0u);
        EXPECT_EQ(no_output.transport, plain.transport);
        Memory unused(name);
        unused.seek_script = {{UINT32_MAX, 0}};
        ASSERT_EQ(unused.run(&error, 1000000), 0) << error;
        EXPECT_GT(unused.asked, 50u);
        EXPECT_EQ(unused.transport, plain.transport);
    }
}
} // namespace

TEST(Media, Vp9WebmSeeksToKeyframeWithTimeline)
{
    Memory media("seek.webm");
    media.vp9_output = true;
    std::string error;
    ASSERT_FALSE(media.bytes.empty());
    ASSERT_EQ(media.run(&error, 2400000), 0) << error;
    EXPECT_TRUE(media.transport.empty());
    EXPECT_EQ(media.timeline.duration_us, 6000000);
    ASSERT_EQ(media.timeline.chapters.size(), 2u);
    EXPECT_EQ(media.timeline.chapters[1].position_us, 2000000);
    EXPECT_TRUE(media.timeline.seekable);
    ASSERT_EQ(media.vp9.size(), 100u);
    EXPECT_EQ(media.vp9.front().pts_us, 4000000u);
    EXPECT_EQ(media.vp9.back().pts_us, 7960000u);
    EXPECT_TRUE(media.vp9.front().keyframe);
    EXPECT_EQ(media.vp9.front().width, 320u);
    EXPECT_EQ(media.vp9.front().height, 180u);
}

TEST(Media, Vp9WebmRequiresConsumerAndHonorsStopAndFailure)
{
    std::string error;
    Memory unsupported("seek.webm");
    EXPECT_EQ(unsupported.run(&error), -1);
    Memory rejected("seek.webm");
    rejected.vp9_output = rejected.reject_output = true;
    EXPECT_EQ(rejected.run(&error), -1);
    EXPECT_EQ(rejected.vp9.size(), 1u);
    Memory stopped("seek.webm");
    stopped.vp9_output = stopped.stop_after_vp9 = true;
    EXPECT_EQ(stopped.run(&error), 1);
    EXPECT_EQ(stopped.vp9.size(), 1u);
}
