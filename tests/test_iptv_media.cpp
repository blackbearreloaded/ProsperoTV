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
#include <libavutil/imgutils.h>
}

namespace
{
struct Memory
{
    std::vector<std::uint8_t> bytes, transport;
    std::size_t at = 0, piece = 4096;
    bool stop = false, reject_output = false;
    unsigned seeks = 0;
    iptv::Subtitles *subtitles = nullptr;
    unsigned subtitle_language = 0;
    std::string url;
    unsigned opened = 0, closed = 0, ranges = 0;
    Memory *owner = nullptr;
    std::vector<iptv::MediaAudioTrack> audio_tracks;
    std::vector<std::int64_t> subtitle_times;
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
        if (count)
            std::memcpy(out, m.bytes.data() + m.at, count);
        m.at += count;
        return static_cast<int>(count);
    }
    int run(std::string *error)
    {
        iptv::MediaInput input{
            this, read,
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
        const iptv::MediaOutput output{
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
            { static_cast<Memory *>(self)->audio_tracks = tracks; }};
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
} // namespace
