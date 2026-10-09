/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_stream.h"
#include "iptv_timeshift.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace
{

using Packet = std::array<std::uint8_t, IPTV_STREAM_TS_PACKET_BYTES>;

std::uint32_t MpegCrc(const std::vector<std::uint8_t> &bytes)
{
    std::uint32_t crc = UINT32_C(0xffffffff);
    for (const std::uint8_t byte : bytes)
    {
        crc ^= static_cast<std::uint32_t>(byte) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & UINT32_C(0x80000000)) ? (crc << 1) ^ UINT32_C(0x04c11db7) : crc << 1;
    }
    return crc;
}

void AppendCrc(std::vector<std::uint8_t> *section)
{
    const std::uint32_t crc = MpegCrc(*section);
    section->push_back(static_cast<std::uint8_t>(crc >> 24));
    section->push_back(static_cast<std::uint8_t>(crc >> 16));
    section->push_back(static_cast<std::uint8_t>(crc >> 8));
    section->push_back(static_cast<std::uint8_t>(crc));
    EXPECT_EQ(MpegCrc(*section), 0u);
}

Packet PsiPacket(std::uint16_t pid, const std::vector<std::uint8_t> &section,
                 std::uint8_t counter = 0)
{
    Packet packet{};
    packet.fill(0xff);
    packet[0] = 0x47;
    packet[1] = static_cast<std::uint8_t>(0x40u | (pid >> 8));
    packet[2] = static_cast<std::uint8_t>(pid);
    packet[3] = static_cast<std::uint8_t>(0x10u | (counter & 0x0fu));
    packet[4] = 0;
    EXPECT_LE(section.size(), packet.size() - 5u);
    std::memcpy(packet.data() + 5u, section.data(), section.size());
    return packet;
}

std::vector<std::uint8_t> PatSection()
{
    std::vector<std::uint8_t> section{0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1,
                                      0x00, 0x00, 0x00, 0x01, 0xe1, 0x00};
    AppendCrc(&section);
    return section;
}

std::vector<std::uint8_t> PmtSection(std::uint8_t audio_type, std::uint16_t audio_pid = 0x111,
                                     std::uint8_t video_type = 0x1b,
                                     const std::vector<std::uint8_t> &descriptors = {})
{
    std::vector<std::uint8_t> section{0x02, 0xb0,       0x17, 0x00, 0x01, 0xc1, 0x00, 0x00,
                                      0xe1, 0x10,       0xf0, 0x00, 0x1b, 0xe1, 0x10, 0xf0,
                                      0x00, audio_type, 0x00, 0x00, 0xf0, 0x00};
    section[18] = static_cast<std::uint8_t>(0xe0u | (audio_pid >> 8));
    section[19] = static_cast<std::uint8_t>(audio_pid);
    section[12] = video_type;
    section[2] += descriptors.size();
    section[21] = descriptors.size();
    section.insert(section.end(), descriptors.begin(), descriptors.end());
    AppendCrc(&section);
    return section;
}

Packet UnsupportedAacPacket()
{
    Packet packet{};
    packet.fill(0xff);
    packet[0] = 0x47;
    packet[1] = 0x41;
    packet[2] = 0x11;
    packet[3] = 0x10;
    const std::uint8_t pes[] = {
        0x00,
        0x00,
        0x01,
        0xc0,
        0x00,
        0x0a,
        0x80,
        0x00,
        0x00,
        // AAC Main, 48 kHz, stereo, seven-byte ADTS framing fixture.
        0xff,
        0xf1,
        0x0c,
        0x80,
        0x00,
        0xff,
        0xfc,
    };
    std::memcpy(packet.data() + 4u, pes, sizeof(pes));
    return packet;
}

Packet MalformedAacPacket()
{
    Packet packet = UnsupportedAacPacket();
    packet[4u + 9u + 2u] = 0x4c; // AAC-LC, 48 kHz, stereo.
    packet[4u + 9u + 5u] = 0xdf; // Declares six bytes, shorter than the header.
    return packet;
}

Packet PesPacket(std::uint16_t pid, std::uint8_t counter, std::uint8_t stream_id,
                 const std::vector<std::uint8_t> &payload)
{
    Packet packet{};
    packet.fill(0xff);
    packet[0] = 0x47;
    packet[1] = static_cast<std::uint8_t>(0x40u | (pid >> 8));
    packet[2] = static_cast<std::uint8_t>(pid);
    packet[3] = static_cast<std::uint8_t>(0x10u | counter);
    const std::size_t packet_length = 3u + payload.size();
    EXPECT_LE(9u + payload.size(), packet.size() - 4u);
    packet[4] = 0;
    packet[5] = 0;
    packet[6] = 1;
    packet[7] = stream_id;
    packet[8] = static_cast<std::uint8_t>(packet_length >> 8);
    packet[9] = static_cast<std::uint8_t>(packet_length);
    packet[10] = 0x80;
    packet[11] = 0;
    packet[12] = 0;
    if (!payload.empty())
        std::memcpy(packet.data() + 13u, payload.data(), payload.size());
    return packet;
}

Packet H264Packet(std::uint8_t counter, bool parameter_sets, std::uint8_t nal_header = 0x65)
{
    std::vector<std::uint8_t> payload;
    if (parameter_sets)
    {
        const std::uint8_t setup[] = {
            0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x2a, 0xac, 0xb4, 0x03,
            0xc0, 0x11, 0x3f, 0x2e, 0x02, 0xd4, 0x08, 0x08, 0x05, 0x00, 0x00,
            0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x78, 0x8f, 0x18, 0x32,
            0xa0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xef, 0x03, 0xb2, 0xc8, 0xb0,
        };
        payload.insert(payload.end(), std::begin(setup), std::end(setup));
    }
    std::uint8_t frame[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x80, 0x00, 0x00, 0x00,
                            0x01, 0x09, 0xf0, 0x00, 0x00, 0x00, 0x01, 0x09, 0xf0};
    frame[4] = nal_header;
    payload.insert(payload.end(), std::begin(frame), std::end(frame));
    return PesPacket(0x110, counter, 0xe0, payload);
}

Packet StampedPacket(Packet packet, std::uint64_t pts)
{
    const auto length = static_cast<unsigned>(packet[8]) * 256u + packet[9];
    std::memmove(packet.data() + 18, packet.data() + 13, length - 3);
    packet[8] = static_cast<std::uint8_t>((length + 5) >> 8);
    packet[9] = static_cast<std::uint8_t>(length + 5);
    packet[11] = 0x80;
    packet[12] = 5;
    packet[13] = 0x21 | static_cast<std::uint8_t>((pts >> 29) & 0x0e);
    packet[14] = static_cast<std::uint8_t>(pts >> 22);
    packet[15] = 1 | static_cast<std::uint8_t>((pts >> 14) & 0xfe);
    packet[16] = static_cast<std::uint8_t>(pts >> 7);
    packet[17] = 1 | static_cast<std::uint8_t>((pts << 1) & 0xfe);
    return packet;
}

void AppendPacket(std::vector<std::uint8_t> *bytes, const Packet &packet)
{
    bytes->insert(bytes->end(), packet.begin(), packet.end());
}

struct FakeBackend
{
    unsigned opens = 0;
    unsigned opened_level = 0;
    unsigned videos = 0;
    unsigned audios = 0;
    unsigned disables = 0;
    unsigned discontinuities = 0;
    unsigned drains = 0;
    unsigned closes = 0;
    int drain_result = 0;
    int select_result = 0;
    unsigned programme_boundaries = 0;
    int boundary_result = 0;
    std::vector<std::uint32_t> audio_selections;
    std::vector<std::vector<std::uint8_t>> video_packets;
    std::vector<iptv_stream_subtitle_track_t> subtitle_tracks;
    unsigned subtitle_updates = 0, subtitle_resets = 0;
    struct Caption
    {
        iptv_stream_subtitle_track_t track;
        std::vector<std::uint8_t> bytes;
        std::uint64_t pts;
    };
    std::vector<Caption> captions;
};

int FakeOpen(void *context, const iptv_stream_format_t *format)
{
    auto *fake = static_cast<FakeBackend *>(context);
    ++fake->opens;
    fake->opened_level = format->video_level;
    EXPECT_EQ(format->video_codec, IPTV_STREAM_VIDEO_H264);
    EXPECT_EQ(format->visible_width, 1920u);
    EXPECT_EQ(format->visible_height, 1080u);
    return 0;
}

int FakeVideo(void *context, const std::uint8_t *bytes, std::size_t count, std::uint64_t)
{
    auto &fake = *static_cast<FakeBackend *>(context);
    ++fake.videos;
    fake.video_packets.emplace_back(bytes, bytes + count);
    return 0;
}

int FakeAudio(void *context, const std::uint8_t *, std::size_t, std::uint64_t)
{
    ++static_cast<FakeBackend *>(context)->audios;
    return 0;
}

int FakeDisableAudio(void *context)
{
    ++static_cast<FakeBackend *>(context)->disables;
    return 0;
}

int FakeSelectAudio(void *context, std::uint32_t type)
{
    auto &fake = *static_cast<FakeBackend *>(context);
    fake.audio_selections.push_back(type);
    return fake.select_result;
}

int FakeDiscontinuity(void *context)
{
    ++static_cast<FakeBackend *>(context)->discontinuities;
    return 0;
}

int FakeDrain(void *context)
{
    auto *fake = static_cast<FakeBackend *>(context);
    ++fake->drains;
    return fake->drain_result;
}

void FakeClose(void *context)
{
    ++static_cast<FakeBackend *>(context)->closes;
}

std::vector<std::uint8_t> StreamBytes(std::uint8_t audio_type, const Packet &third)
{
    const Packet pat = PsiPacket(0, PatSection());
    const Packet pmt = PsiPacket(0x100, PmtSection(audio_type));
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), pat.begin(), pat.end());
    bytes.insert(bytes.end(), pmt.begin(), pmt.end());
    bytes.insert(bytes.end(), third.begin(), third.end());
    return bytes;
}

std::vector<std::uint8_t> TrackPmt(const std::vector<iptv_stream_audio_track_t> &tracks)
{
    auto section = PmtSection(0x0f);
    section.resize(17); // Keep the PMT header and video entry.
    for (const auto &track : tracks)
    {
        section.insert(section.end(),
                       {static_cast<std::uint8_t>(track.stream_type),
                        static_cast<std::uint8_t>(0xe0 | (track.pid >> 8)),
                        static_cast<std::uint8_t>(track.pid), 0xf0, 6, 0x0a, 4,
                        static_cast<std::uint8_t>(track.language[0]),
                        static_cast<std::uint8_t>(track.language[1]),
                        static_cast<std::uint8_t>(track.language[2]), track.audio_type});
    }
    const auto length = section.size() + 1; // CRC plus bytes following section_length.
    section[1] = static_cast<std::uint8_t>(0xb0 | (length >> 8));
    section[2] = static_cast<std::uint8_t>(length);
    AppendCrc(&section);
    return section;
}

std::vector<std::uint8_t> ProgrammePmt(const std::vector<iptv_stream_audio_track_t> &audio,
                                       const std::vector<iptv_stream_subtitle_track_t> &subtitles)
{
    auto pmt = TrackPmt(audio);
    pmt.resize(pmt.size() - 4);
    for (const auto &track : subtitles)
        pmt.insert(pmt.end(), {6, static_cast<std::uint8_t>(0xe0 | (track.pid >> 8)),
                               static_cast<std::uint8_t>(track.pid), 0xf0, 10, 0x59, 8,
                               static_cast<std::uint8_t>(track.language[0]),
                               static_cast<std::uint8_t>(track.language[1]),
                               static_cast<std::uint8_t>(track.language[2]), track.subtitling_type,
                               static_cast<std::uint8_t>(track.composition_page >> 8),
                               static_cast<std::uint8_t>(track.composition_page),
                               static_cast<std::uint8_t>(track.ancillary_page >> 8),
                               static_cast<std::uint8_t>(track.ancillary_page)});
    const auto length = pmt.size() + 1;
    pmt[1] = static_cast<std::uint8_t>(0xb0 | (length >> 8));
    pmt[2] = static_cast<std::uint8_t>(length);
    AppendCrc(&pmt);
    return pmt;
}

class AudioSelectionTest : public testing::Test
{
  protected:
    FakeBackend fake;
    iptv_stream_session_t session{};
    std::uint8_t pmt_counter = 0;
    void SetUp() override
    {
        iptv_stream_backend_t backend{};
        backend.context = &fake;
        backend.open = FakeOpen;
        backend.submit_video = FakeVideo;
        backend.submit_audio = FakeAudio;
        backend.disable_audio = FakeDisableAudio;
        backend.select_audio = FakeSelectAudio;
        backend.programme_boundary = [](void *ctx)
        {
            auto &state = *static_cast<FakeBackend *>(ctx);
            ++state.programme_boundaries;
            return state.boundary_result;
        };
        backend.discontinuity = FakeDiscontinuity;
        backend.drain = FakeDrain;
        backend.close = FakeClose;
        backend.subtitle_tracks =
            [](void *ctx, const iptv_stream_subtitle_track_t *tracks, std::size_t count)
        {
            auto &state = *static_cast<FakeBackend *>(ctx);
            state.subtitle_tracks.assign(tracks, tracks + count);
            ++state.subtitle_updates;
        };
        backend.subtitle_packet = [](void *ctx, const iptv_stream_subtitle_track_t *track,
                                     const std::uint8_t *data, std::size_t bytes, std::uint64_t pts)
        {
            static_cast<FakeBackend *>(ctx)->captions.push_back(
                {*track, {data, data + bytes}, pts});
        };
        backend.subtitle_reset = [](void *ctx)
        { ++static_cast<FakeBackend *>(ctx)->subtitle_resets; };
        iptv_stream_init(&session);
        ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
    }
    void TearDown() override
    {
        (void)iptv_stream_cleanup(&session);
    }
    int pmt(const std::vector<iptv_stream_audio_track_t> &tracks, bool start = false)
    {
        return section(TrackPmt(tracks), start);
    }
    int section(const std::vector<std::uint8_t> &section, bool start = false)
    {
        std::vector<std::uint8_t> bytes;
        if (start)
            AppendPacket(&bytes, PsiPacket(0, PatSection()));
        for (std::size_t at = 0; at < section.size();)
        {
            Packet packet;
            packet.fill(0xff);
            packet[0] = 0x47;
            packet[1] = at ? 0x01 : 0x41;
            packet[2] = 0;
            packet[3] = 0x10 | (pmt_counter++ & 0x0f);
            const std::size_t offset = at ? 4 : 5;
            if (!at)
                packet[4] = 0;
            const auto count = std::min(section.size() - at, packet.size() - offset);
            std::memcpy(packet.data() + offset, section.data() + at, count);
            AppendPacket(&bytes, packet);
            at += count;
        }
        if (start)
            AppendPacket(&bytes, H264Packet(0, true));
        return iptv_stream_push(&session, bytes.data(), bytes.size());
    }
    int audio(std::uint32_t pid, unsigned counter)
    {
        auto packet = UnsupportedAacPacket();
        packet[1] = static_cast<std::uint8_t>(0x40 | (pid >> 8));
        packet[2] = static_cast<std::uint8_t>(pid);
        packet[3] = 0x10 | (counter & 0x0f);
        return iptv_stream_push(&session, packet.data(), packet.size());
    }
};

TEST_F(AudioSelectionTest, LargeSlicesWithoutDelimitersKeepTheirPictureBoundaries)
{
    ASSERT_EQ(pmt({{0x111, 0x0f, "eng", 0}}, true), IPTV_STREAM_OK);
    std::vector<std::uint8_t> first_picture;
    std::vector<std::uint8_t> last_picture;
    for (unsigned i = 0; i < 3; ++i)
    {
        std::vector<std::uint8_t> nal{0, 0,    0,
                                      1, 0x41, static_cast<std::uint8_t>(i == 1 ? 0x40 : 0x80)};
        nal.resize(106, 0xaa); // Larger than the old 64-byte slice-header buffer.
        if (i < 2)
            first_picture.insert(first_picture.end(), nal.begin(), nal.end());
        else
            last_picture = nal;
        const auto packet = PesPacket(0x110, i + 1, 0xe0, nal);
        ASSERT_EQ(iptv_stream_push(&session, packet.data(), packet.size()), IPTV_STREAM_OK);
    }
    ASSERT_EQ(iptv_stream_stop(&session), IPTV_STREAM_OK);
    ASSERT_EQ(fake.video_packets.size(), 3u); // Startup, two-slice picture, next picture.
    const auto &combined = fake.video_packets[1];
    ASSERT_GE(combined.size(), first_picture.size());
    EXPECT_TRUE(std::equal(first_picture.rbegin(), first_picture.rend(), combined.rbegin()));
    EXPECT_EQ(fake.video_packets[2], last_picture);
}

class SubtitleStreamTest : public AudioSelectionTest
{
  protected:
    int subtitles(const std::vector<iptv_stream_subtitle_track_t> &tracks, bool start = false)
    {
        return section(ProgrammePmt({{0x111, 0x0f, "eng", 0}}, tracks), start);
    }
    std::vector<std::uint8_t> pes(std::uint64_t pts = 90000)
    {
        return {0,
                0,
                1,
                0xbd,
                0,
                18,
                0x80,
                0x80,
                5,
                static_cast<std::uint8_t>(0x21 | ((pts >> 29) & 0x0e)),
                static_cast<std::uint8_t>(pts >> 22),
                static_cast<std::uint8_t>(1 | ((pts >> 14) & 0xfe)),
                static_cast<std::uint8_t>(pts >> 7),
                static_cast<std::uint8_t>(1 | ((pts << 1) & 0xfe)),
                0x20,
                0,
                0x0f,
                0x80,
                0,
                1,
                0,
                1,
                0xff,
                0xff}; // Segment body ends in 0xff.
    }
    int packet(std::uint16_t pid, unsigned counter, bool start,
               const std::vector<std::uint8_t> &data)
    {
        EXPECT_LE(data.size(), 183u);
        Packet packet{};
        packet.fill(0xff);
        packet[0] = 0x47;
        packet[1] = static_cast<std::uint8_t>((start ? 0x40 : 0) | (pid >> 8));
        packet[2] = static_cast<std::uint8_t>(pid);
        packet[3] = static_cast<std::uint8_t>(0x30 | (counter & 0xf));
        packet[4] = static_cast<std::uint8_t>(183 - data.size());
        if (packet[4])
            packet[5] = 0;
        std::copy(data.begin(), data.end(), packet.end() - data.size());
        return iptv_stream_push(&session, packet.data(), packet.size());
    }
};

TEST_F(SubtitleStreamTest, DiscoversLanguagePagesAndAssemblesSplitHeadersAcrossRecurringPmt)
{
    const std::vector<iptv_stream_subtitle_track_t> tracks{
        {0x120, 1, 2, "ENG", 0x10}, {0x120, 3, 4, "spa", 0x20}, {0x121, 5, 6, "fra", 0x14}};
    ASSERT_EQ(subtitles(tracks, true), 0);
    ASSERT_EQ(fake.subtitle_tracks.size(), 3u);
    EXPECT_STREQ(fake.subtitle_tracks[0].language, "eng");
    EXPECT_EQ(fake.subtitle_tracks[1].subtitling_type, 0x20);
    EXPECT_EQ(fake.subtitle_tracks[1].ancillary_page, 4);
    const auto bytes = pes();
    ASSERT_EQ(packet(0x120, 0, true, {bytes.begin(), bytes.begin() + 2}), 0);
    ASSERT_EQ(packet(0x120, 0, true, {bytes.begin(), bytes.begin() + 2}), 0); // Duplicate.
    ASSERT_EQ(subtitles(tracks), 0);
    EXPECT_EQ(fake.subtitle_updates, 1u);
    ASSERT_EQ(packet(0x120, 1, false, {bytes.begin() + 2, bytes.begin() + 7}), 0);
    ASSERT_EQ(packet(0x120, 2, false, {bytes.begin() + 7, bytes.end()}), 0);
    ASSERT_EQ(fake.captions.size(), 2u);
    EXPECT_EQ(fake.captions[0].bytes, (std::vector<std::uint8_t>{0x0f, 0x80, 0, 1, 0, 1, 0xff}));
    EXPECT_EQ(fake.captions[0].pts, 1000000u);
    EXPECT_EQ(fake.captions[1].track.composition_page, 3);
    EXPECT_EQ(fake.subtitle_resets, 0u);
    EXPECT_EQ(session.telemetry.duplicate_packets, 1u);
    EXPECT_EQ(session.telemetry.continuity_errors, 0u);
    ASSERT_EQ(audio(0x111, 0), 0);
    EXPECT_EQ(fake.audios, 1u);
    EXPECT_EQ(fake.videos, 1u);
    EXPECT_EQ(fake.opens, 1u);
    ASSERT_EQ(subtitles({tracks[2]}), 0);
    ASSERT_EQ(packet(0x120, 3, true, bytes), 0);
    EXPECT_EQ(fake.captions.size(), 2u); // Removed PIDs cannot deliver stale language data.
    ASSERT_EQ(subtitles({}), 0);
    EXPECT_TRUE(fake.subtitle_tracks.empty());
    EXPECT_EQ(fake.subtitle_updates, 3u);
}

TEST_F(SubtitleStreamTest, BrokenSubtitlesAndPacketLossRecoverWithoutDisablingAudioOrVideo)
{
    ASSERT_EQ(subtitles({{0x120, 1, 1, "eng", 0x10}}, true), 0);
    unsigned counter = 0;
    auto valid = pes();
    for (unsigned corruption = 0; corruption < 6; ++corruption)
    {
        auto broken = valid;
        switch (corruption)
        {
        case 0:
            broken[5] = 0;
            break; // Unbounded private stream is invalid.
        case 1:
            broken[14] = 0;
            break; // Wrong data_identifier.
        case 2:
            broken[21] = 100;
            break; // Segment extends beyond the PES.
        case 3:
            broken[9] &= ~1u;
            break; // Broken PTS marker.
        case 4:
            broken[7] = 0;
            break; // No timed caption.
        case 5:
            broken[8] = 255;
            break; // Optional header extends beyond PES.
        }
        ASSERT_EQ(packet(0x120, counter++, true, broken), 0);
        EXPECT_EQ(fake.captions.size(), corruption);
        ASSERT_EQ(packet(0x120, counter++, true, valid), 0);
        EXPECT_EQ(fake.captions.size(), corruption + 1u);
    }
    ASSERT_EQ(packet(0x120, counter++, true, {valid.begin(), valid.begin() + 16}), 0);
    ++counter; // One lost packet discards the incomplete PES and decoder page state.
    ASSERT_EQ(packet(0x120, counter++, false, {valid.begin() + 16, valid.end()}), 0);
    EXPECT_EQ(fake.captions.size(), 6u);
    ASSERT_EQ(packet(0x120, counter++, true, valid), 0);
    EXPECT_EQ(fake.captions.size(), 7u);
    EXPECT_EQ(fake.subtitle_resets, 7u);
    EXPECT_EQ(session.telemetry.continuity_errors, 1u);
    for (unsigned corruption = 0; corruption < 3; ++corruption)
    {
        auto broken = PesPacket(0x120, counter++ & 15, 0xbd, {});
        if (corruption == 0)
            broken[3] |= 0x80; // Scrambled captions cannot stop a clear video PID.
        else if (corruption == 1)
            broken[3] &= 0x0f; // Reserved adaptation mode.
        else
        {
            broken[3] |= 0x20;
            broken[4] = 255; // Adaptation extends beyond the TS packet.
        }
        ASSERT_EQ(iptv_stream_push(&session, broken.data(), broken.size()), 0);
    }
    EXPECT_EQ(fake.subtitle_resets, 10u);
    ASSERT_EQ(packet(0x120, counter++, true, valid), 0);
    EXPECT_EQ(fake.captions.size(), 8u);
    ASSERT_EQ(audio(0x111, 0), 0);
    const auto video = H264Packet(1, false);
    ASSERT_EQ(iptv_stream_push(&session, video.data(), video.size()), 0);
    EXPECT_EQ(fake.audios, 1u);
    EXPECT_EQ(fake.videos, 2u);
    EXPECT_EQ(session.telemetry.audio_disabled, 0u);
    EXPECT_EQ(session.telemetry.state, IPTV_STREAM_STATE_PLAYING);
}

TEST_F(SubtitleStreamTest, BoundsTracksReusesRemovedPidSlotsAndRetainsContinuityForEveryLanguage)
{
    std::vector<iptv_stream_subtitle_track_t> tracks;
    for (unsigned i = 0; i < 40; ++i)
        tracks.push_back({0x120 + i, 1, 1, "eng", 0x10});
    ASSERT_EQ(subtitles(tracks, true), 0);
    EXPECT_EQ(fake.subtitle_tracks.size(), IPTV_STREAM_MAX_SUBTITLE_TRACKS);
    for (unsigned i = 0; i < 32; ++i)
    {
        ASSERT_EQ(packet(0x120 + i, 0, true, pes()), 0);
        ASSERT_EQ(packet(0x120 + i, 0, true, pes()), 0);
    }
    EXPECT_EQ(fake.captions.size(), 32u);
    EXPECT_EQ(session.telemetry.duplicate_packets, 32u);
    for (auto &track : tracks)
        track.pid += 100;
    ASSERT_EQ(subtitles(tracks), 0);
    for (unsigned i = 0; i < 32; ++i)
    {
        ASSERT_EQ(packet(0x120 + 100 + i, 0, true, pes()), 0);
        ASSERT_EQ(packet(0x120 + 100 + i, 0, true, pes()), 0);
    }
    EXPECT_EQ(fake.captions.size(), 64u);
    EXPECT_EQ(session.telemetry.duplicate_packets, 64u);
    ASSERT_EQ(iptv_stream_discontinuity(&session), 0);
    EXPECT_EQ(fake.subtitle_resets, 1u);
    // Reacquire TS sync after a discontinuity before resuming the same PID/counter.
    ASSERT_EQ(subtitles(tracks, true), 0);
    ASSERT_EQ(packet(0x120 + 100, 0, true, pes(45000)), 0);
    ASSERT_EQ(fake.captions.size(), 65u);
    EXPECT_EQ(fake.captions.back().pts, 500000u);
}

TEST_F(SubtitleStreamTest, CaptionsShareTheVideoEpochOnBothSidesOfClockWrap)
{
    ASSERT_EQ(subtitles({{0x120, 1, 1, "eng", 0x10}, {0x121, 1, 1, "fra", 0x10}}, true), 0);
    const auto wrap = UINT64_C(1) << 33;
    const auto usec = [](std::uint64_t pts) { return pts / 90 * 1000 + pts % 90 * 1000 / 90; };
    auto video = StampedPacket(H264Packet(1, false), wrap - 90000);
    ASSERT_EQ(iptv_stream_push(&session, video.data(), video.size()), 0);
    ASSERT_EQ(packet(0x120, 0, true, pes(90000)), 0);
    ASSERT_EQ(fake.captions.size(), 1u);
    EXPECT_EQ(fake.captions.back().pts, usec(wrap + 90000));
    video = StampedPacket(H264Packet(2, false), 45000);
    ASSERT_EQ(iptv_stream_push(&session, video.data(), video.size()), 0);
    ASSERT_EQ(packet(0x121, 0, true, pes(wrap - 45000)), 0);
    EXPECT_EQ(fake.captions.back().pts, usec(wrap - 45000));
    ASSERT_EQ(packet(0x121, 1, true, pes(90000)), 0);
    EXPECT_EQ(fake.captions.back().pts, usec(wrap + 90000));
}

TEST_F(SubtitleStreamTest, RepositionRestoresVideoAudioAndSubtitleClockEpochs)
{
    ASSERT_EQ(subtitles({{0x120, 1, 1, "eng", 0x10}}, true), 0);
    const auto wrap = UINT64_C(1) << 33;
    const auto usec = [](std::uint64_t ticks)
    { return ticks / 90 * 1000 + ticks % 90 * 1000 / 90; };
    // Move in both directions, including exactly at a wrap and a non-integral
    // microsecond tick. All three streams must retain the history's epoch.
    for (const auto ticks : {wrap + 90000, wrap - 90000, wrap, 2 * wrap + 1})
    {
        ASSERT_EQ(iptv_stream_reposition(&session, usec(ticks)), IPTV_STREAM_OK);
        auto video = StampedPacket(H264Packet(0, false), ticks & (wrap - 1));
        ASSERT_EQ(iptv_stream_push(&session, video.data(), video.size()), 0);
        const auto audio = StampedPacket(UnsupportedAacPacket(), (ticks - 45000) & (wrap - 1));
        ASSERT_EQ(iptv_stream_push(&session, audio.data(), audio.size()), 0);
        ASSERT_EQ(packet(0x120, 0, true, pes((ticks + 45000) & (wrap - 1))), 0);
        // Transport synchronization requires three packets after repositioning.
        EXPECT_EQ(session.telemetry.last_video_pts_us, usec(ticks));
        EXPECT_EQ(session.telemetry.last_audio_pts_us, usec(ticks - 45000));
        ASSERT_FALSE(fake.captions.empty());
        EXPECT_EQ(fake.captions.back().pts, usec(ticks + 45000));
    }
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.discontinuities, 4u);
    EXPECT_EQ(fake.subtitle_resets, 4u);
    EXPECT_EQ(iptv_stream_reposition(&session, IPTV_STREAM_PTS_UNKNOWN),
              IPTV_STREAM_INVALID_ARGUMENT);
    EXPECT_EQ(fake.discontinuities, 4u);
    EXPECT_EQ(iptv_stream_reposition(nullptr, 0), IPTV_STREAM_INVALID_ARGUMENT);
}

TEST_F(AudioSelectionTest, ChangesOnlyAudioAndRemembersSelectionAcrossProviderReordering)
{
    ASSERT_EQ(pmt({{0x111, 0x0f, "ENG", 0}, {0x112, 0x0f, "spa", 3}}, true), 0);
    iptv_stream_audio_track_t tracks[2]{};
    std::uint32_t selected = 0;
    EXPECT_EQ(iptv_stream_audio_tracks(&session, tracks, 2, &selected), 2u);
    EXPECT_STREQ(tracks[0].language, "eng");
    EXPECT_STREQ(tracks[1].language, "spa");
    EXPECT_EQ(tracks[1].audio_type, 3);
    EXPECT_EQ(selected, 0x111u);
    ASSERT_EQ(audio(0x111, 0), 0);
    ASSERT_EQ(iptv_stream_select_audio(&session, 0x112), 0);
    ASSERT_EQ(audio(0x111, 1), 0); // The old language is no longer delivered.
    ASSERT_EQ(audio(0x112, 9), 0);
    EXPECT_EQ(fake.audios, 2u);
    ASSERT_EQ(pmt({{0x112, 0x0f, "spa", 3}, {0x111, 0x0f, "eng", 0}}), 0);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0x112u);
    EXPECT_EQ(fake.audio_selections.size(), 1u);
    ASSERT_EQ(iptv_stream_select_audio(&session, 0x111), 0);
    ASSERT_EQ(audio(0x111, 8), 0); // Counters advanced while the PID was unselected.
    EXPECT_EQ(session.telemetry.continuity_errors, 0u);
    EXPECT_EQ(fake.audios, 3u);
    ASSERT_EQ(iptv_stream_select_audio(&session, 0), 0);
    ASSERT_EQ(audio(0x111, 9), 0);
    ASSERT_EQ(pmt({{0x112, 0x0f, "spa", 0}}), 0);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0u);
    EXPECT_EQ(fake.audios, 3u);
    EXPECT_EQ(fake.audio_selections, (std::vector<std::uint32_t>{0x0f, 0x0f, 0}));
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.closes, 0u);
    EXPECT_EQ(fake.videos, 1u);
}

TEST_F(AudioSelectionTest, RemovedLanguageFallsBackAndFailedDecoderCanBeRetried)
{
    ASSERT_EQ(pmt({{0x111, 0x0f, "eng", 0}, {0x112, 0x0f, "spa", 0}}, true), 0);
    ASSERT_EQ(iptv_stream_select_audio(&session, 0x112), 0);
    ASSERT_EQ(pmt({{0x113, 0x0f, "fra", 0}}), 0);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0x113u);
    EXPECT_EQ(iptv_stream_select_audio(&session, 0x444), IPTV_STREAM_INVALID_ARGUMENT);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0x113u);
    ASSERT_EQ(iptv_stream_select_audio(&session, 0), 0);
    fake.select_result = -1;
    EXPECT_EQ(iptv_stream_select_audio(&session, 0x113), IPTV_STREAM_NATIVE_ERROR);
    EXPECT_EQ(session.telemetry.state, IPTV_STREAM_STATE_PLAYING);
    EXPECT_EQ(session.telemetry.audio_disabled, 1u);
    EXPECT_NE(std::strstr(session.telemetry.audio_warning, "choose another track"), nullptr);
    fake.select_result = 0;
    EXPECT_EQ(iptv_stream_select_audio(&session, 0x113), 0);
    EXPECT_EQ(session.telemetry.audio_disabled, 0u);
    EXPECT_EQ(session.telemetry.audio_warning[0], '\0');
    ASSERT_EQ(audio(0x113, 0), 0);
    EXPECT_EQ(fake.audios, 1u);
    EXPECT_EQ(fake.opens, 1u);
}

TEST_F(AudioSelectionTest, OffBeforeDiscoveryStaysOffAndTrackCopiesAreBounded)
{
    ASSERT_EQ(iptv_stream_select_audio(&session, 0), 0);
    std::vector<iptv_stream_audio_track_t> tracks;
    for (unsigned i = 0; i < 40; ++i)
        tracks.push_back({0x111 + i, 0x0f, "eng", 0});
    ASSERT_EQ(pmt(tracks, true), 0);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0u);
    struct
    {
        iptv_stream_audio_track_t track{};
        std::uint32_t guard = 123456;
    } copy;
    EXPECT_EQ(iptv_stream_audio_tracks(&session, &copy.track, 1, nullptr),
              IPTV_STREAM_MAX_AUDIO_TRACKS);
    EXPECT_EQ(copy.guard, 123456u);
    EXPECT_EQ(copy.track.pid, 0x111u);
    EXPECT_EQ(iptv_stream_select_audio(&session, 0x111 + 32), IPTV_STREAM_INVALID_ARGUMENT);
    EXPECT_EQ(iptv_stream_select_audio(&session, 0x111 + 31), 0);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0x111u + 31);
}

TEST_F(AudioSelectionTest, RejectsDuplicateAudioPidsWithoutChangingDecoder)
{
    ASSERT_EQ(pmt({{0x111, 0x0f, "eng", 0}}, true), 0);
    EXPECT_EQ(pmt({{0x112, 0x0f, "eng", 0}, {0x112, 0x0f, "spa", 0}}), IPTV_STREAM_MALFORMED_TS);
    EXPECT_TRUE(fake.audio_selections.empty());
}

TEST_F(AudioSelectionTest, AlignsNewAudioWithVideoOnEitherSideOfTransportClockWrap)
{
    const auto stamped = StampedPacket;
    const std::uint64_t wrap = UINT64_C(1) << 33;
    ASSERT_EQ(pmt({{0x111, 0x0f, "eng", 0}, {0x112, 0x0f, "spa", 0}}, true), 0);
    for (const auto &packet :
         {stamped(H264Packet(1, false), wrap - 90000), stamped(H264Packet(2, false), 90000)})
        ASSERT_EQ(iptv_stream_push(&session, packet.data(), packet.size()), 0);
    ASSERT_EQ(iptv_stream_select_audio(&session, 0x112), 0);
    auto packet = stamped(UnsupportedAacPacket(), 90000);
    packet[2] = 0x12;
    ASSERT_EQ(iptv_stream_push(&session, packet.data(), packet.size()), 0);
    const auto usec = [](std::uint64_t ticks)
    { return (ticks / 90) * 1000 + (ticks % 90) * 1000 / 90; };
    EXPECT_EQ(session.telemetry.last_audio_pts_us, usec(wrap + 90000));
    ASSERT_EQ(iptv_stream_select_audio(&session, 0x111), 0);
    packet = stamped(UnsupportedAacPacket(), wrap - 45000);
    ASSERT_EQ(iptv_stream_push(&session, packet.data(), packet.size()), 0);
    EXPECT_EQ(session.telemetry.last_audio_pts_us, usec(wrap - 45000));
}

TEST_F(AudioSelectionTest, RejectsMalformedLanguageDescriptors)
{
    ASSERT_EQ(pmt({{0x111, 0x0f, "eng", 0}}, true), 0);
    const auto packet =
        PsiPacket(0x100, PmtSection(0x0f, 0x112, 0x1b, {0x0a, 3, 'e', 'n', 'g'}), pmt_counter++);
    EXPECT_EQ(iptv_stream_push(&session, packet.data(), packet.size()), IPTV_STREAM_MALFORMED_TS);
    EXPECT_TRUE(fake.audio_selections.empty());
}

TEST(IptvStreamTest, RoutesAacMainToAudioBackend)
{
    FakeBackend fake;
    iptv_stream_backend_t backend{};
    backend.context = &fake;
    backend.open = FakeOpen;
    backend.submit_video = FakeVideo;
    backend.submit_audio = FakeAudio;
    backend.disable_audio = FakeDisableAudio;
    backend.drain = FakeDrain;
    backend.close = FakeClose;
    backend.hardware_validated = 1;

    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);

    std::vector<std::uint8_t> bytes;
    AppendPacket(&bytes, PsiPacket(0, PatSection()));
    AppendPacket(&bytes, PsiPacket(0x100, PmtSection(0x0f)));
    AppendPacket(&bytes, H264Packet(0, true));
    AppendPacket(&bytes, UnsupportedAacPacket());
    AppendPacket(&bytes, H264Packet(1, false));
    EXPECT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
    const iptv_stream_telemetry_t *telemetry = iptv_stream_telemetry(&session);
    ASSERT_NE(telemetry, nullptr);
    EXPECT_EQ(telemetry->state, IPTV_STREAM_STATE_PLAYING);
    EXPECT_EQ(telemetry->format.video_codec, IPTV_STREAM_VIDEO_H264);
    EXPECT_EQ(telemetry->audio_disabled, 0u);
    EXPECT_EQ(telemetry->error_count, 0u);
    EXPECT_EQ(telemetry->video_access_units, 2u);
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.videos, 2u);
    EXPECT_EQ(fake.audios, 1u);
    EXPECT_EQ(fake.disables, 0u);

    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
    EXPECT_EQ(fake.drains, 1u);
    EXPECT_EQ(fake.closes, 1u);
}

TEST(IptvStreamTest, DisablesMalformedAacWithoutFailingVideoSession)
{
    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, nullptr), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);

    const auto bytes = StreamBytes(0x0f, MalformedAacPacket());
    EXPECT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
    const iptv_stream_telemetry_t *telemetry = iptv_stream_telemetry(&session);
    ASSERT_NE(telemetry, nullptr);
    EXPECT_EQ(telemetry->audio_disabled, 1u);
    EXPECT_NE(std::strstr(telemetry->audio_warning, "silent video"), nullptr);
    EXPECT_EQ(telemetry->error_count, 0u);
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
}

TEST(IptvStreamTest, SelectsAndReassemblesMp2ForBothTransportTypes)
{
    for (const std::uint8_t type : {0x03, 0x04})
    {
        FakeBackend fake;
        iptv_stream_backend_t backend{};
        backend.context = &fake;
        backend.open = FakeOpen;
        backend.submit_video = FakeVideo;
        backend.submit_audio = FakeAudio;
        backend.disable_audio = FakeDisableAudio;
        backend.drain = FakeDrain;
        backend.close = FakeClose;
        iptv_stream_session_t session{};
        iptv_stream_init(&session);
        ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
        auto bytes = StreamBytes(type, H264Packet(0, true));
        // MPEG-1 Layer II 32 kbps at 48 kHz: a 96-byte frame, split across PES.
        std::vector<std::uint8_t> frame(96, 0);
        frame[0] = 0xff;
        frame[1] = 0xfd;
        frame[2] = 0x14;
        AppendPacket(&bytes, PesPacket(0x111, 0, 0xc0, {frame.begin(), frame.begin() + 2}));
        AppendPacket(&bytes, PesPacket(0x111, 1, 0xc0, {frame.begin() + 2, frame.end()}));
        AppendPacket(&bytes, PesPacket(0x111, 2, 0xc0, frame));
        for (const auto byte : bytes)
            ASSERT_EQ(iptv_stream_push(&session, &byte, 1), IPTV_STREAM_OK);
        const auto *telemetry = iptv_stream_telemetry(&session);
        EXPECT_EQ(telemetry->format.audio_stream_type, type);
        EXPECT_EQ(telemetry->format.audio_pid, 0x111u);
        EXPECT_EQ(telemetry->format.audio_sample_rate, 48000u);
        EXPECT_EQ(telemetry->format.audio_channels, 2u);
        EXPECT_EQ(fake.audios, 2u);
        EXPECT_EQ(fake.disables, 0u);
        EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
    }
}

TEST(IptvStreamTest, SelectsDolbyDescriptorsAndLatmAndReassemblesPrivatePes)
{
    for (const unsigned type : {0x81u, 0x87u, 0x11u})
    {
        for (const bool private_stream : {false, true})
        {
            if (private_stream && type == 0x11u)
                continue;
            FakeBackend fake;
            iptv_stream_backend_t backend{};
            backend.context = &fake;
            backend.open = FakeOpen;
            backend.submit_video = FakeVideo;
            backend.submit_audio = FakeAudio;
            backend.disable_audio = FakeDisableAudio;
            backend.drain = FakeDrain;
            backend.close = FakeClose;
            iptv_stream_session_t session{};
            iptv_stream_init(&session);
            ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
            ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
            std::vector<std::uint8_t> bytes;
            AppendPacket(&bytes, PsiPacket(0, PatSection()));
            const std::vector<std::uint8_t> descriptors =
                private_stream
                    ? std::vector<std::uint8_t>{static_cast<uint8_t>(type == 0x81u ? 0x6a : 0x7a),
                                                0}
                    : std::vector<std::uint8_t>{};
            AppendPacket(&bytes, PsiPacket(0x100, PmtSection(private_stream ? 6 : type, 0x111, 0x1b,
                                                             descriptors)));
            AppendPacket(&bytes, H264Packet(0, true));
            std::vector<std::uint8_t> frame(type == 0x81u ? 128 : 16, 0);
            if (type == 0x11u)
            {
                frame[0] = 0x56;
                frame[1] = 0xe0;
                frame[2] = 13;
            }
            else
            {
                frame[0] = 0x0b;
                frame[1] = 0x77;
                frame[5] = type == 0x81u ? 0x40 : 0x80;
                if (type == 0x87u)
                {
                    frame[3] = 7;
                    frame[4] = 0x30;
                }
            }
            AppendPacket(&bytes, PesPacket(0x111, 0, 0xbd, {frame.begin(), frame.begin() + 3}));
            AppendPacket(&bytes, PesPacket(0x111, 1, 0xbd, {frame.begin() + 3, frame.end()}));
            AppendPacket(&bytes, PesPacket(0x111, 2, 0xbd, frame));
            for (const auto byte : bytes)
                ASSERT_EQ(iptv_stream_push(&session, &byte, 1), IPTV_STREAM_OK);
            EXPECT_EQ(iptv_stream_telemetry(&session)->format.audio_stream_type, type);
            EXPECT_EQ(fake.audios, 2u);
            EXPECT_EQ(fake.disables, 0u);
            EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
        }
    }
}

TEST(IptvStreamTest, ResetsTransportTimelineWithoutReopeningBackend)
{
    FakeBackend fake;
    iptv_stream_backend_t backend{};
    backend.context = &fake;
    backend.open = FakeOpen;
    backend.submit_video = FakeVideo;
    backend.submit_audio = FakeAudio;
    backend.disable_audio = FakeDisableAudio;
    backend.discontinuity = FakeDiscontinuity;
    backend.drain = FakeDrain;
    backend.close = FakeClose;

    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
    const auto first = StreamBytes(0x0f, H264Packet(0, true));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_discontinuity(&session), IPTV_STREAM_OK);
    const auto second = StreamBytes(0x0f, H264Packet(0, true));
    ASSERT_EQ(iptv_stream_push(&session, second.data(), second.size()), IPTV_STREAM_OK);

    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.discontinuities, 1u);
    EXPECT_EQ(fake.videos, 2u);
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
}

TEST_F(AudioSelectionTest, RepositionRestoresMissingParametersOnlyOnTheFirstPicture)
{
    ASSERT_EQ(pmt({{0x111, 0x0f, "eng", 0}}, true), IPTV_STREAM_OK);
    ASSERT_EQ(fake.video_packets.size(), 1u);
    const auto configured = fake.video_packets.front();
    for (const bool repeated_headers : {false, true, false})
    {
        ASSERT_EQ(iptv_stream_reposition(&session, 1000000), IPTV_STREAM_OK);
        const auto restart = StreamBytes(0x0f, H264Packet(0, repeated_headers));
        for (const auto byte : restart)
            ASSERT_EQ(iptv_stream_push(&session, &byte, 1), IPTV_STREAM_OK);
        EXPECT_EQ(fake.video_packets.back(), configured);
        const auto following = H264Packet(1, false);
        ASSERT_EQ(iptv_stream_push(&session, following.data(), following.size()), IPTV_STREAM_OK);
        EXPECT_LT(fake.video_packets.back().size(), configured.size());
    }
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.discontinuities, 3u);
}

Packet ConfigurationPacket(unsigned counter, unsigned version, std::uint64_t second,
                           unsigned level = 42)
{
    auto packet = H264Packet(counter & 15, true);
    const auto sps = std::find(packet.begin() + 13, packet.end(), 0x67);
    EXPECT_GE(std::distance(sps, packet.end()), 4);
    if (std::distance(sps, packet.end()) >= 4)
        sps[3] = static_cast<std::uint8_t>(level);
    const auto pps = std::find(packet.begin() + 13, packet.end(), 0x68);
    EXPECT_GE(std::distance(pps, packet.end()), 6);
    if (std::distance(pps, packet.end()) >= 6)
        pps[5] = static_cast<std::uint8_t>(0x80 | (version & 0x7f));
    return StampedPacket(packet, second * 90000);
}

int FixturePpsVersion(const std::vector<std::uint8_t> &picture)
{
    const auto pps = std::find(picture.begin(), picture.end(), 0x68);
    EXPECT_GE(std::distance(pps, picture.end()), 6);
    return std::distance(pps, picture.end()) >= 6 ? pps[5] : -1;
}

TEST_F(AudioSelectionTest, HistoricalFormatChangeRequiresFreshBackend)
{
    iptv::Timeshift history(188 * 32);
    ASSERT_TRUE(history.enable_replay());
    const auto initial = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_TRUE(history.append(initial.data(), initial.size()));
    ASSERT_EQ(iptv_stream_push(&session, initial.data(), initial.size()), IPTV_STREAM_OK);
    ASSERT_EQ(fake.opened_level, 42u);
    const auto changed = ConfigurationPacket(1, 2, 2, 41);
    ASSERT_TRUE(history.append(changed.data(), changed.size()));
    for (unsigned second = 3; second <= 10; ++second)
    {
        const auto next = StampedPacket(H264Packet(second - 1, false), second * 90000);
        ASSERT_TRUE(history.append(next.data(), next.size()));
    }
    const auto forward = history.seek_next(2000000);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->pts_us, 2000000u);      // No five-second fallback into the old setup.
    EXPECT_FALSE(history.seek_next(11000000)); // Wait for downloaded configuration metadata.
    for (const unsigned target : {8u, 1u})
    {
        const auto position = history.seek(target * 1000000);
        ASSERT_TRUE(position);
        const auto videos = fake.videos;
        ASSERT_EQ(history.reposition(&session, *position), IPTV_STREAM_REOPEN_REQUIRED);
        EXPECT_EQ(session.telemetry.reopen_pts_us, position->pts_us);
        EXPECT_EQ(fake.videos, videos); // Never submit the new setup to an incompatible decoder.
        ASSERT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
        SetUp();
        ASSERT_EQ(history.reposition(&session, *position), IPTV_STREAM_OK);
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto picture =
                StampedPacket(H264Packet(i, false), position->pts_us * 90 / 1000 + i * 90000);
            ASSERT_EQ(iptv_stream_push(&session, picture.data(), picture.size()), IPTV_STREAM_OK);
        }
        EXPECT_EQ(fake.opened_level, target == 8 ? 41u : 42u);
        EXPECT_GT(fake.videos, videos);
    }
    EXPECT_EQ(fake.opens, 3u);
}

TEST_F(AudioSelectionTest, InBandFormatChangeReportsItsOwnClockForHistoryRecovery)
{
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    const auto changed = ConfigurationPacket(1, 2, 2, 41);
    ASSERT_EQ(iptv_stream_push(&session, changed.data(), changed.size()),
              IPTV_STREAM_REOPEN_REQUIRED);
    EXPECT_EQ(session.telemetry.reopen_pts_us, 2000000u);
    EXPECT_EQ(fake.opened_level, 42u);
}

TEST_F(AudioSelectionTest, VideoPidChangeReopensFromTheNewHistoryTimeline)
{
    iptv::Timeshift history(188 * 32);
    ASSERT_TRUE(history.enable_replay());
    const auto initial = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_TRUE(history.append(initial.data(), initial.size()));
    ASSERT_EQ(iptv_stream_push(&session, initial.data(), initial.size()), IPTV_STREAM_OK);
    const auto old = history.seek(1000000);
    ASSERT_TRUE(old);
    auto programme = PmtSection(0x0f);
    programme.resize(programme.size() - 4);
    programme[9] = programme[14] = 0x20; // PCR and video move from 0x110 to 0x120.
    AppendCrc(&programme);
    const auto pmt = PsiPacket(0x100, programme, 1);
    ASSERT_TRUE(history.append(pmt.data(), pmt.size()));
    for (unsigned second = 2; second <= 6; ++second)
    {
        auto picture = ConfigurationPacket(second - 2, 2, second);
        picture[2] = 0x20;
        ASSERT_TRUE(history.append(picture.data(), picture.size()));
    }
    EXPECT_NE(history.range().generation, old->generation);
    EXPECT_EQ(history.reposition(&session, *old), IPTV_STREAM_INVALID_ARGUMENT);
    const auto next = history.seek_next(2000000);
    ASSERT_TRUE(next);
    ASSERT_EQ(history.reposition(&session, *next), IPTV_STREAM_REOPEN_REQUIRED);
    EXPECT_EQ(session.telemetry.reopen_pts_us, next->pts_us);
    ASSERT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
    SetUp();
    ASSERT_EQ(history.reposition(&session, *next), IPTV_STREAM_OK);
    EXPECT_EQ(session.telemetry.format.video_pid, 0x120u);
    for (unsigned i = 0; i < 3; ++i)
    {
        auto picture = StampedPacket(H264Packet(i, false), (2 + i) * 90000);
        picture[2] = 0x20;
        ASSERT_EQ(iptv_stream_push(&session, picture.data(), picture.size()), IPTV_STREAM_OK);
    }
    EXPECT_EQ(fake.opens, 2u);
}

TEST_F(AudioSelectionTest, CodecChangeExpiresHistoryAndRequestsNativeReopen)
{
    iptv::Timeshift history(188 * 32);
    ASSERT_TRUE(history.enable_replay());
    const auto initial = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_TRUE(history.append(initial.data(), initial.size()));
    ASSERT_EQ(iptv_stream_push(&session, initial.data(), initial.size()), IPTV_STREAM_OK);
    const auto generation = history.range().generation;
    const auto pmt = PsiPacket(0x100, PmtSection(0x0f, 0x111, 0x24), 1);
    ASSERT_TRUE(history.append(pmt.data(), pmt.size()));
    EXPECT_NE(history.range().generation, generation);
    EXPECT_FALSE(history.range().timed); // Wait for the new codec's complete setup.
    EXPECT_EQ(history.range().replay_result, IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_push(&session, pmt.data(), pmt.size()), IPTV_STREAM_REOPEN_REQUIRED);
    EXPECT_EQ(session.telemetry.reopen_pts_us, 1000001u);
    EXPECT_EQ(fake.opens, 1u);
}

TEST_F(SubtitleStreamTest, HistoryRestoresProgrammeTracksWithoutRetainedTables)
{
    iptv::Timeshift history(188 * 32);
    ASSERT_TRUE(history.enable_replay());
    auto initial = std::vector<std::uint8_t>{};
    AppendPacket(&initial, PsiPacket(0, PatSection()));
    AppendPacket(&initial, PsiPacket(0x100, ProgrammePmt({{0x111, 0x0f, "eng", 0}},
                                                         {{0x120, 1, 2, "eng", 0x10}})));
    AppendPacket(&initial, ConfigurationPacket(0, 1, 1));
    ASSERT_TRUE(history.append(initial.data(), initial.size()));
    ASSERT_EQ(iptv_stream_push(&session, initial.data(), initial.size()), IPTV_STREAM_OK);
    auto changed = std::vector<std::uint8_t>{};
    auto pending = ConfigurationPacket(1, 1, 2);
    const auto length = (unsigned(pending[8]) * 256 + pending[9]) - 12;
    pending[8] = length >> 8;
    pending[9] = length & 255;
    std::fill(pending.begin() + 10 + length, pending.end(), 0xff);
    AppendPacket(&changed, pending); // Old picture completes after the new PMT arrives.
    auto table =
        PsiPacket(0x100, ProgrammePmt({{0x113, 0x0f, "jpn", 0}}, {{0x130, 3, 4, "fra", 0x10}}));
    table[3] = (table[3] & 0xf0) | 1;
    AppendPacket(&changed, table);
    // Video settings stay identical: track changes alone must be retained.
    for (unsigned second = 3; second <= 10; ++second)
        AppendPacket(&changed, StampedPacket(H264Packet((second - 1) & 15, false), second * 90000));
    ASSERT_TRUE(history.append(changed.data(), changed.size()));
    for (const unsigned second : {8u, 7u, 1u, 8u})
    {
        const auto position = history.seek(second * 1000000);
        ASSERT_TRUE(position);
        ASSERT_EQ(position->pts_us, (second == 1 ? 1u : second - 5) * 1000000u);
        ASSERT_EQ(history.reposition(&session, *position), IPTV_STREAM_OK);
        std::array<iptv_stream_audio_track_t, 2> tracks{};
        std::uint32_t selected = 0;
        ASSERT_EQ(iptv_stream_audio_tracks(&session, tracks.data(), tracks.size(), &selected), 1u);
        const bool original = second != 8;
        EXPECT_EQ(selected, original ? 0x111u : 0x113u);
        EXPECT_STREQ(tracks[0].language, original ? "eng" : "jpn");
        ASSERT_EQ(fake.subtitle_tracks.size(), 1u);
        EXPECT_EQ(fake.subtitle_tracks[0].pid, original ? 0x120u : 0x130u);
        EXPECT_EQ(fake.subtitle_tracks[0].composition_page, original ? 1u : 3u);
        EXPECT_STREQ(fake.subtitle_tracks[0].language, original ? "eng" : "fra");
        std::vector<std::uint8_t> pictures;
        for (unsigned i = 0; i < 3; ++i)
            AppendPacket(&pictures, StampedPacket(H264Packet(i, false),
                                                  (position->pts_us / 1000000 + i) * 90000));
        ASSERT_EQ(iptv_stream_push(&session, pictures.data(), pictures.size()), IPTV_STREAM_OK);
        const auto played_audio = fake.audios;
        ASSERT_EQ(audio(selected, 0), IPTV_STREAM_OK);
        EXPECT_GT(fake.audios, played_audio);
        const auto subtitle_pid = original ? 0x120u : 0x130u;
        ASSERT_EQ(packet(subtitle_pid, 0, true, pes()), IPTV_STREAM_OK);
        ASSERT_FALSE(fake.captions.empty());
        EXPECT_EQ(fake.captions.back().track.pid, subtitle_pid);
    }
    for (unsigned second = 11; second <= 60; ++second)
    {
        const auto next = StampedPacket(H264Packet((second - 1) & 15, false), second * 90000);
        ASSERT_TRUE(history.append(next.data(), next.size()));
    }
    ASSERT_GT(history.range().begin, initial.size() + 2 * 188);
    ASSERT_EQ(iptv_stream_select_audio(&session, 0), IPTV_STREAM_OK);
    const auto retained = history.seek(history.range().first_pts_us);
    ASSERT_TRUE(retained);
    ASSERT_EQ(history.reposition(&session, *retained), IPTV_STREAM_OK);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0u);
    EXPECT_EQ(fake.subtitle_tracks[0].pid, 0x130u);
    EXPECT_EQ(fake.programme_boundaries, 0u); // Seeks must not wait on paused old queues.
}

TEST_F(AudioSelectionTest, AutomaticProgrammeChangeWaitsBeforeReplacingTracks)
{
    ASSERT_EQ(pmt({{0x111, 0x0f, "eng", 0}}, true), IPTV_STREAM_OK);
    const auto video = H264Packet(0, true);
    ASSERT_EQ(iptv_stream_push(&session, video.data(), video.size()), IPTV_STREAM_OK);
    ASSERT_EQ(fake.opens, 1u);
    ASSERT_EQ(pmt({{0x111, 0x0f, "eng", 0}}), IPTV_STREAM_OK);
    EXPECT_EQ(fake.programme_boundaries, 0u);
    ASSERT_EQ(pmt({{0x113, 0x0f, "fra", 0}}), IPTV_STREAM_OK);
    EXPECT_EQ(fake.programme_boundaries, 1u);
    const auto selections = fake.audio_selections.size();
    fake.boundary_result = -1;
    EXPECT_EQ(pmt({{0x114, 0x0f, "jpn", 0}}), IPTV_STREAM_NATIVE_ERROR);
    EXPECT_EQ(fake.programme_boundaries, 2u);
    EXPECT_EQ(fake.audio_selections.size(), selections);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0x113u);
}

TEST_F(AudioSelectionTest, DownloadConfigurationSurvivesUnreadTransportExpiry)
{
    const auto close = [](iptv_stream_session_t *p)
    {
        (void)iptv_stream_cleanup(p);
        delete p;
    };
    std::unique_ptr<iptv_stream_session_t, decltype(close)> download(new iptv_stream_session_t{});
    iptv_stream_init(download.get());
    ASSERT_EQ(iptv_stream_open(download.get(), nullptr, nullptr), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(download.get()), IPTV_STREAM_OK);
    iptv::Timeshift history(188 * 16);
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_scan(download.get(), first.data(), first.size(), 0), IPTV_STREAM_OK);
    ASSERT_TRUE(history.append(first.data(), first.size()));
    const auto unread = history.range().end;
    const auto changed = ConfigurationPacket(1, 2, 2);
    ASSERT_EQ(iptv_stream_scan(download.get(), changed.data(), changed.size(), 0), IPTV_STREAM_OK);
    ASSERT_TRUE(history.append(changed.data(), changed.size()));
    // Download runs while playback remains paused on configuration 1. Headers
    // for configuration 2 are overwritten; only headerless pictures survive.
    for (unsigned second = 3; second <= 30; ++second)
    {
        const auto next = StampedPacket(H264Packet((second - 1) & 15, false), second * 90000);
        ASSERT_EQ(iptv_stream_scan(download.get(), next.data(), next.size(), 0), IPTV_STREAM_OK);
        ASSERT_TRUE(history.append(next.data(), next.size()));
    }
    ASSERT_GT(history.range().begin, unread + changed.size());
    ASSERT_EQ(fake.videos, 1u);
    ASSERT_EQ(FixturePpsVersion(fake.video_packets.back()), 0x81);
    const auto resume = history.seek(history.range().first_pts_us);
    ASSERT_TRUE(resume);
    ASSERT_EQ(iptv_stream_reposition_from(&session, download.get(), resume->pts_us),
              IPTV_STREAM_OK);
    download.reset(); // Playback owns its restored parameters, not source pointers.
    std::array<std::uint8_t, 188 * 16> bytes{};
    const auto retained = history.read(resume->offset, bytes.data(), bytes.size());
    ASSERT_GT(retained.bytes, 0u);
    ASSERT_EQ(iptv_stream_push(&session, bytes.data(), retained.bytes), IPTV_STREAM_OK);
    ASSERT_GT(fake.videos, 1u);
    EXPECT_EQ(FixturePpsVersion(fake.video_packets[1]), 0x82);
    EXPECT_EQ(fake.discontinuities, 1u);
    // Local history must not silently restore the stale configuration 1 after
    // an external restore. Configuration 2 remains usable after source cleanup.
    EXPECT_EQ(iptv_stream_reposition(&session, 1000000), IPTV_STREAM_INVALID_ARGUMENT);
    ASSERT_EQ(iptv_stream_reposition(&session, resume->pts_us), IPTV_STREAM_OK);
    const auto before_replay = fake.video_packets.size();
    ASSERT_EQ(iptv_stream_push(&session, bytes.data(), retained.bytes), IPTV_STREAM_OK);
    ASSERT_GT(fake.video_packets.size(), before_replay);
    EXPECT_EQ(FixturePpsVersion(fake.video_packets[before_replay]), 0x82);
}

TEST_F(AudioSelectionTest, DownloadReplayRejectsUnavailableHistoryBeforeResettingPlayback)
{
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    const auto close = [](iptv_stream_session_t *p)
    {
        (void)iptv_stream_cleanup(p);
        delete p;
    };
    std::unique_ptr<iptv_stream_session_t, decltype(close)> download(new iptv_stream_session_t{});
    iptv_stream_init(download.get());
    ASSERT_EQ(iptv_stream_open(download.get(), nullptr, nullptr), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(download.get()), IPTV_STREAM_OK);
    EXPECT_EQ(iptv_stream_reposition_from(&session, nullptr, 1000000),
              IPTV_STREAM_INVALID_ARGUMENT);
    EXPECT_EQ(iptv_stream_reposition_from(&session, download.get(), 1000000),
              IPTV_STREAM_INVALID_STATE);
    ASSERT_EQ(iptv_stream_scan(download.get(), first.data(), first.size(), 0), IPTV_STREAM_OK);
    for (const auto pts : std::array<std::uint64_t, 4>{0, 999999, 1000001, UINT64_MAX})
        EXPECT_EQ(iptv_stream_reposition_from(&session, download.get(), pts),
                  IPTV_STREAM_INVALID_ARGUMENT);
    for (unsigned i = 1; i < 400; ++i)
    {
        const auto next = ConfigurationPacket(i, i, i + 1);
        ASSERT_EQ(iptv_stream_scan(download.get(), next.data(), next.size(), 0), IPTV_STREAM_OK);
    }
    const auto floor = iptv_stream_replay_start(download.get());
    ASSERT_GT(floor, 1000000u);
    EXPECT_EQ(iptv_stream_reposition_from(&session, download.get(), floor - 1),
              IPTV_STREAM_INVALID_ARGUMENT);
    EXPECT_EQ(fake.discontinuities, 0u);
    EXPECT_EQ(session.telemetry.last_video_pts_us, 1000000u);
    ASSERT_EQ(iptv_stream_reposition_from(&session, download.get(), floor), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_discontinuity(download.get()), IPTV_STREAM_OK);
    EXPECT_EQ(iptv_stream_reposition_from(&session, download.get(), floor),
              IPTV_STREAM_INVALID_STATE);
    EXPECT_EQ(fake.discontinuities, 1u);
}

TEST_F(AudioSelectionTest, LiveHistoryStartsAfterProgramHeadersHaveExpired)
{
    iptv::Timeshift history(188 * 16);
    ASSERT_TRUE(history.enable_replay());
    std::vector<std::uint8_t> input;
    AppendPacket(&input, PsiPacket(0, PatSection()));
    AppendPacket(&input,
                 PsiPacket(0x100, TrackPmt({{0x111, 0x0f, "eng", 0}, {0x112, 0x0f, "spa", 0}})));
    AppendPacket(&input, ConfigurationPacket(0, 2, 1));
    for (unsigned second = 2; second <= 45; ++second)
        AppendPacket(&input, StampedPacket(H264Packet((second - 1) & 15, false), second * 90000));
    ASSERT_TRUE(history.append(input.data(), input.size()));
    ASSERT_GT(history.range().begin, 188u * 3);
    const auto position = history.seek(history.range().first_pts_us);
    ASSERT_TRUE(position);
    ASSERT_EQ(history.reposition(&session, *position), IPTV_STREAM_OK);
    std::array<std::uint8_t, 188 * 16> bytes{};
    const auto retained = history.read(position->offset, bytes.data(), bytes.size());
    ASSERT_GT(retained.bytes, 0u);
    ASSERT_EQ(iptv_stream_push(&session, bytes.data(), retained.bytes), IPTV_STREAM_OK);
    ASSERT_EQ(fake.opens, 1u);
    ASSERT_GT(fake.video_packets.size(), 0u);
    EXPECT_EQ(FixturePpsVersion(fake.video_packets.front()), 0x82);
    std::array<iptv_stream_audio_track_t, 2> tracks{};
    std::uint32_t selected = 0;
    ASSERT_EQ(iptv_stream_audio_tracks(&session, tracks.data(), tracks.size(), &selected), 2u);
    EXPECT_EQ(selected, 0x111u);
    EXPECT_STREQ(tracks[1].language, "spa");
    EXPECT_EQ(iptv_stream_select_audio(&session, 0x112), IPTV_STREAM_OK);
}

TEST_F(AudioSelectionTest, LiveHistoryRestoresMissingCodecHeadersAfterEarlyProgramTables)
{
    const auto early = StreamBytes(0x0f, H264Packet(0, false));
    ASSERT_EQ(iptv_stream_push(&session, early.data(), early.size()), IPTV_STREAM_OK);
    ASSERT_GT(session.telemetry.pmt_sections, 0u);
    ASSERT_EQ(fake.opens, 0u);
    ASSERT_EQ(iptv_stream_select_audio(&session, 0), IPTV_STREAM_OK);
    iptv::Timeshift history(188 * 16);
    ASSERT_TRUE(history.enable_replay());
    auto input = StreamBytes(0x0f, ConfigurationPacket(0, 2, 1));
    for (unsigned second = 2; second <= 4; ++second)
        AppendPacket(&input, StampedPacket(H264Packet(second - 1, false), second * 90000));
    ASSERT_TRUE(history.append(input.data(), input.size()));
    const auto position = history.seek(2000000);
    ASSERT_TRUE(position);
    ASSERT_EQ(history.reposition(&session, *position), IPTV_STREAM_OK);
    std::array<std::uint8_t, 188 * 16> bytes{};
    const auto retained = history.read(position->offset, bytes.data(), bytes.size());
    ASSERT_EQ(iptv_stream_push(&session, bytes.data(), retained.bytes), IPTV_STREAM_OK);
    ASSERT_EQ(fake.opens, 1u);
    ASSERT_FALSE(fake.video_packets.empty());
    EXPECT_EQ(FixturePpsVersion(fake.video_packets.front()), 0x82);
    EXPECT_EQ(session.telemetry.format.audio_pid, 0u);
    EXPECT_EQ(fake.disables, 1u);
}

TEST_F(AudioSelectionTest, LiveHistoryRestoresUnreadSettingsAfterOverwriteAndProviderReset)
{
    iptv::Timeshift history(188 * 16);
    ASSERT_TRUE(history.enable_replay());
    EXPECT_FALSE(history.enable_replay());
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_TRUE(history.append(first.data(), first.size()));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    const auto old = history.seek(1000000);
    ASSERT_TRUE(old);
    std::vector<std::uint8_t> unread;
    AppendPacket(&unread, ConfigurationPacket(1, 2, 2));
    for (unsigned second = 3; second <= 45; ++second)
        AppendPacket(&unread, StampedPacket(H264Packet((second - 1) & 15, false), second * 90000));
    // One append larger than the entire ring must still retain the settings
    // from its overwritten prefix, just like a series of network reads.
    ASSERT_TRUE(history.append(unread.data(), unread.size()));
    ASSERT_GT(history.range().begin, first.size() + 188);
    ASSERT_EQ(history.range().replay_result, IPTV_STREAM_OK);
    EXPECT_EQ(fake.videos, 1u);
    EXPECT_EQ(history.reposition(&session, *old), IPTV_STREAM_INVALID_ARGUMENT);
    EXPECT_EQ(fake.discontinuities, 0u);
    auto resume = history.seek(history.range().first_pts_us);
    ASSERT_TRUE(resume);
    ASSERT_EQ(history.reposition(&session, *resume), IPTV_STREAM_OK);
    std::array<std::uint8_t, 188 * 16> bytes{};
    auto retained = history.read(resume->offset, bytes.data(), bytes.size());
    ASSERT_GT(retained.bytes, 0u);
    ASSERT_EQ(iptv_stream_push(&session, bytes.data(), retained.bytes), IPTV_STREAM_OK);
    ASSERT_GT(fake.video_packets.size(), 1u);
    EXPECT_EQ(FixturePpsVersion(fake.video_packets[1]), 0x82);

    for (const bool explicit_reset : {false, true})
    {
        const auto generation = history.range().generation;
        const auto previous = *resume;
        const unsigned version = explicit_reset ? 4 : 3;
        if (explicit_reset)
        {
            history.discontinuity();
            EXPECT_FALSE(history.range().timed);
            EXPECT_FALSE(history.seek(1000000));
        }
        std::vector<std::uint8_t> reset = StreamBytes(0x0f, ConfigurationPacket(0, version, 1));
        AppendPacket(&reset, StampedPacket(H264Packet(1, false), 2 * 90000));
        AppendPacket(&reset, StampedPacket(H264Packet(2, false), 3 * 90000));
        ASSERT_TRUE(history.append(reset.data(), reset.size()));
        EXPECT_EQ(history.range().generation, generation + 1);
        ASSERT_EQ(history.range().replay_result, IPTV_STREAM_OK);
        EXPECT_EQ(history.reposition(&session, previous), IPTV_STREAM_INVALID_ARGUMENT);
        resume = history.seek(1000000);
        ASSERT_TRUE(resume);
        ASSERT_EQ(history.reposition(&session, *resume), IPTV_STREAM_OK);
        retained = history.read(resume->offset, bytes.data(), bytes.size());
        const auto before = fake.video_packets.size();
        ASSERT_EQ(iptv_stream_push(&session, bytes.data(), retained.bytes), IPTV_STREAM_OK);
        ASSERT_GT(fake.video_packets.size(), before);
        EXPECT_EQ(FixturePpsVersion(fake.video_packets[before]), 0x80 | version);
    }
}

TEST_F(AudioSelectionTest, LiveHistoryDoesNotSeekPastTheCompletedMetadata)
{
    iptv::Timeshift history(188 * 32);
    ASSERT_TRUE(history.enable_replay());
    EXPECT_FALSE(history.range().timed);
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_TRUE(history.append(first.data(), first.size()));
    auto pending = ConfigurationPacket(1, 2, 20);
    const auto length = (unsigned(pending[8]) * 256 + pending[9]) - 12;
    pending[8] = length >> 8;
    pending[9] = length & 255;
    std::fill(pending.begin() + 10 + length, pending.end(), 0xff);
    ASSERT_TRUE(history.append(pending.data(), pending.size()));
    EXPECT_EQ(history.range().last_pts_us, 1000000u);
    auto position = history.seek(UINT64_MAX);
    ASSERT_TRUE(position);
    EXPECT_EQ(position->pts_us, 1000000u);
    const auto next = StampedPacket(H264Packet(2, false), 21 * 90000);
    ASSERT_TRUE(history.append(next.data(), next.size()));
    EXPECT_GE(history.range().last_pts_us, 20000000u);
    const auto following = StampedPacket(H264Packet(3, false), 26 * 90000);
    ASSERT_TRUE(history.append(following.data(), following.size()));
    position = history.seek(UINT64_MAX);
    ASSERT_TRUE(position);
    EXPECT_GE(position->pts_us, 20000000u);
    EXPECT_EQ(fake.videos, 0u);
}

TEST_F(AudioSelectionTest, RepeatedSeeksRestoreHistoricalParameterIdsInBothDirections)
{
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    ASSERT_EQ(FixturePpsVersion(fake.video_packets.back()), 0x81);
    const auto changed = ConfigurationPacket(1, 2, 20);
    ASSERT_EQ(iptv_stream_push(&session, changed.data(), changed.size()), IPTV_STREAM_OK);
    ASSERT_EQ(FixturePpsVersion(fake.video_packets.back()), 0x82);
    EXPECT_EQ(session.telemetry.last_video_pts_us, 20000000u);
    for (const auto second : {1u, 20u, 3u, 25u, 1u, 20u})
    {
        SCOPED_TRACE(second);
        ASSERT_EQ(iptv_stream_reposition(&session, second * 1000000ull), IPTV_STREAM_OK);
        const auto replay = StreamBytes(0x0f, StampedPacket(H264Packet(0, false), second * 90000));
        ASSERT_EQ(iptv_stream_push(&session, replay.data(), replay.size()), IPTV_STREAM_OK);
        EXPECT_EQ(FixturePpsVersion(fake.video_packets.back()), second < 20 ? 0x81 : 0x82);
        EXPECT_EQ(session.telemetry.last_video_pts_us, second * 1000000ull);
    }
    // Provider resets reuse timestamps; their old versions must not reappear.
    ASSERT_EQ(iptv_stream_discontinuity(&session), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_reposition(&session, 1000000), IPTV_STREAM_OK);
    const auto reset = StreamBytes(0x0f, StampedPacket(H264Packet(0, false), 90000));
    ASSERT_EQ(iptv_stream_push(&session, reset.data(), reset.size()), IPTV_STREAM_OK);
    EXPECT_EQ(FixturePpsVersion(fake.video_packets.back()), 0x82);
    EXPECT_EQ(fake.opens, 1u);
}

TEST_F(AudioSelectionTest, ForwardScanRetainsSkippedConfigurationWithoutPlayingSkippedFrames)
{
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    ASSERT_EQ(fake.videos, 1u);
    std::vector<std::uint8_t> skipped;
    auto changed = ConfigurationPacket(1, 2, 20);
    // Leave the final picture pending until the scan reaches its target boundary.
    const auto length = (unsigned(changed[8]) * 256 + changed[9]) - 12;
    changed[8] = length >> 8;
    changed[9] = length & 255;
    std::fill(changed.begin() + 10 + length, changed.end(), 0xff);
    AppendPacket(&skipped, changed);
    AppendPacket(&skipped, StampedPacket(UnsupportedAacPacket(), 20 * 90000));
    for (std::size_t at = 0; at < skipped.size();)
    {
        const auto bytes = std::min<std::size_t>(7, skipped.size() - at);
        ASSERT_EQ(iptv_stream_scan(&session, skipped.data() + at, bytes, 0), IPTV_STREAM_OK);
        at += bytes;
    }
    EXPECT_EQ(session.telemetry.last_video_pts_us, 1000000u);
    ASSERT_EQ(iptv_stream_scan(&session, nullptr, 0, 1), IPTV_STREAM_OK);
    EXPECT_EQ(session.telemetry.last_video_pts_us, 20000000u);
    EXPECT_EQ(fake.videos, 1u);
    EXPECT_EQ(fake.audios, 0u);
    EXPECT_EQ(fake.opens, 1u);
    for (const unsigned second : {20u, 1u, 20u})
    {
        ASSERT_EQ(iptv_stream_reposition(&session, second * 1000000ull), IPTV_STREAM_OK);
        const auto replay = StreamBytes(0x0f, StampedPacket(H264Packet(0, false), second * 90000));
        ASSERT_EQ(iptv_stream_push(&session, replay.data(), replay.size()), IPTV_STREAM_OK);
        EXPECT_EQ(FixturePpsVersion(fake.video_packets.back()), second == 1 ? 0x81 : 0x82);
    }
    EXPECT_EQ(fake.videos, 4u); // Scan mode does not leak into subsequent playback.
    EXPECT_EQ(iptv_stream_scan(nullptr, nullptr, 0, 1), IPTV_STREAM_INVALID_ARGUMENT);
    EXPECT_EQ(iptv_stream_scan(&session, nullptr, 1, 0), IPTV_STREAM_INVALID_ARGUMENT);
}

TEST_F(AudioSelectionTest, ForwardScanBeforeFirstPictureDoesNotOpenADecoder)
{
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 1));
    ASSERT_EQ(iptv_stream_scan(&session, first.data(), first.size(), 1), IPTV_STREAM_OK);
    EXPECT_EQ(fake.opens, 0u);
    EXPECT_EQ(fake.videos, 0u);
    ASSERT_EQ(iptv_stream_reposition(&session, 1000000), IPTV_STREAM_OK);
    const auto replay = StreamBytes(0x0f, StampedPacket(H264Packet(0, false), 90000));
    ASSERT_EQ(iptv_stream_push(&session, replay.data(), replay.size()), IPTV_STREAM_OK);
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.videos, 1u);
    EXPECT_EQ(FixturePpsVersion(fake.video_packets.back()), 0x81);
}

TEST_F(AudioSelectionTest, ConfigurationRetentionRejectsExpiredSeeksWithoutResettingPlayback)
{
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 0, 1));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    for (unsigned i = 1; i < 400; ++i)
    {
        const auto next = ConfigurationPacket(i, i, i + 1);
        ASSERT_EQ(iptv_stream_push(&session, next.data(), next.size()), IPTV_STREAM_OK);
    }
    const auto first_retained = iptv_stream_replay_start(&session);
    ASSERT_GT(first_retained, 100000000u);
    ASSERT_LT(first_retained, 400000000u);
    EXPECT_EQ(iptv_stream_reposition(&session, first_retained - 1), IPTV_STREAM_INVALID_ARGUMENT);
    EXPECT_EQ(fake.discontinuities, 0u);
    EXPECT_EQ(session.telemetry.state, IPTV_STREAM_STATE_PLAYING);
    ASSERT_EQ(iptv_stream_reposition(&session, first_retained), IPTV_STREAM_OK);
    const auto replay =
        StreamBytes(0x0f, StampedPacket(H264Packet(0, false), first_retained * 9 / 100));
    ASSERT_EQ(iptv_stream_push(&session, replay.data(), replay.size()), IPTV_STREAM_OK);
    EXPECT_EQ(FixturePpsVersion(fake.video_packets.back()),
              0x80 | ((first_retained / 1000000 - 1) & 0x7f));
    ASSERT_EQ(iptv_stream_discontinuity(&session), IPTV_STREAM_OK);
    EXPECT_EQ(iptv_stream_replay_start(&session), 0u);
}

TEST_F(AudioSelectionTest, UntimedConfigurationChangesExpireAmbiguousOldVersions)
{
    const auto first = StreamBytes(0x0f, ConfigurationPacket(0, 1, 10));
    ASSERT_EQ(iptv_stream_push(&session, first.data(), first.size()), IPTV_STREAM_OK);
    const auto untimed = H264Packet(1, true);
    ASSERT_EQ(iptv_stream_push(&session, untimed.data(), untimed.size()), IPTV_STREAM_OK);
    ASSERT_EQ(FixturePpsVersion(fake.video_packets.back()), 0xb0);
    EXPECT_GT(iptv_stream_replay_start(&session), 10000000u);
    EXPECT_EQ(iptv_stream_reposition(&session, 10000000), IPTV_STREAM_INVALID_ARGUMENT);
    const auto next = StampedPacket(H264Packet(2, false), 11 * 90000);
    ASSERT_EQ(iptv_stream_push(&session, next.data(), next.size()), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_reposition(&session, 11000000), IPTV_STREAM_OK);
    const auto replay = StreamBytes(0x0f, StampedPacket(H264Packet(0, false), 11 * 90000));
    ASSERT_EQ(iptv_stream_push(&session, replay.data(), replay.size()), IPTV_STREAM_OK);
    EXPECT_EQ(FixturePpsVersion(fake.video_packets.back()), 0xb0);
}

TEST(IptvStreamTest, AudioProgramChangeDoesNotStopOpenedVideoBackend)
{
    FakeBackend fake;
    iptv_stream_backend_t backend{};
    backend.context = &fake;
    backend.open = FakeOpen;
    backend.submit_video = FakeVideo;
    backend.submit_audio = FakeAudio;
    backend.disable_audio = FakeDisableAudio;
    backend.drain = FakeDrain;
    backend.close = FakeClose;

    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);

    std::vector<std::uint8_t> bytes;
    AppendPacket(&bytes, PsiPacket(0, PatSection()));
    AppendPacket(&bytes, PsiPacket(0x100, PmtSection(0x0f)));
    AppendPacket(&bytes, H264Packet(0, true));
    AppendPacket(&bytes, PsiPacket(0x100, PmtSection(0x0f, 0x112), 1));
    AppendPacket(&bytes, H264Packet(1, false));
    EXPECT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
    const iptv_stream_telemetry_t *telemetry = iptv_stream_telemetry(&session);
    ASSERT_NE(telemetry, nullptr);
    EXPECT_EQ(telemetry->state, IPTV_STREAM_STATE_PLAYING);
    EXPECT_EQ(telemetry->audio_disabled, 1u);
    EXPECT_EQ(telemetry->error_count, 0u);
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.videos, 2u);
    EXPECT_EQ(fake.disables, 1u);

    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
}

TEST(IptvStreamTest, WaitsForRandomAccessFrameBeforeOpeningVideoBackend)
{
    FakeBackend fake;
    iptv_stream_backend_t backend{};
    backend.context = &fake;
    backend.open = FakeOpen;
    backend.submit_video = FakeVideo;
    backend.submit_audio = FakeAudio;
    backend.disable_audio = FakeDisableAudio;
    backend.drain = FakeDrain;
    backend.close = FakeClose;

    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);

    std::vector<std::uint8_t> startup;
    AppendPacket(&startup, PsiPacket(0, PatSection()));
    AppendPacket(&startup, PsiPacket(0x100, PmtSection(0x0f)));
    AppendPacket(&startup, H264Packet(0, true, 0x41));
    ASSERT_EQ(iptv_stream_push(&session, startup.data(), startup.size()), IPTV_STREAM_OK);
    EXPECT_EQ(fake.opens, 0u);
    EXPECT_EQ(fake.videos, 0u);

    const auto random_access = H264Packet(1, false, 0x65);
    ASSERT_EQ(iptv_stream_push(&session, random_access.data(), random_access.size()),
              IPTV_STREAM_OK);
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.videos, 1u);
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
}

TEST(IptvStreamTest, WaitsForNewKeyframeAfterVideoPacketGap)
{
    FakeBackend fake;
    iptv_stream_backend_t backend{};
    backend.context = &fake;
    backend.open = FakeOpen;
    backend.submit_video = FakeVideo;
    backend.submit_audio = FakeAudio;
    backend.disable_audio = FakeDisableAudio;
    backend.discontinuity = FakeDiscontinuity;
    backend.drain = FakeDrain;
    backend.close = FakeClose;

    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
    const auto start = StreamBytes(0x0f, H264Packet(0, true));
    ASSERT_EQ(iptv_stream_push(&session, start.data(), start.size()), IPTV_STREAM_OK);
    EXPECT_EQ(fake.videos, 1u);

    const auto gap = H264Packet(2, false, 0x41);
    ASSERT_EQ(iptv_stream_push(&session, gap.data(), gap.size()), IPTV_STREAM_OK);
    const auto delta = H264Packet(3, false, 0x41);
    ASSERT_EQ(iptv_stream_push(&session, delta.data(), delta.size()), IPTV_STREAM_OK);
    EXPECT_EQ(fake.videos, 1u);
    EXPECT_EQ(session.telemetry.continuity_errors, 1u);

    const auto keyframe = H264Packet(4, false, 0x65);
    ASSERT_EQ(iptv_stream_push(&session, keyframe.data(), keyframe.size()), IPTV_STREAM_OK);
    EXPECT_EQ(fake.videos, 2u);
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
}

TEST(IptvStreamTest, EmitsAllCompleteFramesInOnePesWithoutMoreNetworkInput)
{
    FakeBackend fake;
    iptv_stream_backend_t backend{};
    backend.context = &fake;
    backend.open = FakeOpen;
    backend.submit_video = FakeVideo;
    backend.submit_audio = FakeAudio;
    backend.disable_audio = FakeDisableAudio;
    backend.discontinuity = FakeDiscontinuity;
    backend.drain = FakeDrain;
    backend.close = FakeClose;

    const Packet first = H264Packet(0, true);
    const std::size_t payload_bytes = (static_cast<std::size_t>(first[8]) << 8 | first[9]) - 3u;
    std::vector<std::uint8_t> payload(first.begin() + 13u, first.begin() + 13u + payload_bytes);
    const std::uint8_t more_frames[] = {
        0, 0,    0,    1, 0x41, 0x80, 0, 0,    0,    1, 0x09, 0xf0, 0, 0,    0,
        1, 0x41, 0x80, 0, 0,    0,    1, 0x09, 0xf0, 0, 0,    0,    1, 0x09, 0xf0,
    };
    payload.insert(payload.end(), std::begin(more_frames), std::end(more_frames));

    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
    const auto bytes = StreamBytes(0x0f, PesPacket(0x110, 0, 0xe0, payload));
    ASSERT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
    EXPECT_EQ(fake.videos, 3u);
    EXPECT_EQ(session.telemetry.video_access_units, 3u);
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
}

TEST(IptvStreamTest, PreservesNativeDrainFailureCode)
{
    FakeBackend fake;
    fake.drain_result = -1004;
    iptv_stream_backend_t backend{};
    backend.context = &fake;
    backend.open = FakeOpen;
    backend.submit_video = FakeVideo;
    backend.submit_audio = FakeAudio;
    backend.disable_audio = FakeDisableAudio;
    backend.drain = FakeDrain;
    backend.close = FakeClose;

    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
    const auto bytes = StreamBytes(0x0f, H264Packet(0, true));
    ASSERT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);

    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_NATIVE_ERROR);
    EXPECT_NE(std::strstr(session.telemetry.last_error, "(-1004)"), nullptr);
    EXPECT_EQ(fake.drains, 1u);
    EXPECT_EQ(fake.closes, 1u);
}

TEST(IptvStreamTest, IgnoresUnknownAudioCodecWhileKeepingSupportedVideo)
{
    Packet null_packet{};
    null_packet.fill(0xff);
    null_packet[0] = 0x47;
    null_packet[1] = 0x1f;
    null_packet[2] = 0xff;
    null_packet[3] = 0x10;

    iptv_stream_session_t session{};
    iptv_stream_init(&session);
    ASSERT_EQ(iptv_stream_open(&session, nullptr, nullptr), IPTV_STREAM_OK);
    ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
    const auto bytes = StreamBytes(0x90, null_packet);
    EXPECT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
    const iptv_stream_telemetry_t *telemetry = iptv_stream_telemetry(&session);
    ASSERT_NE(telemetry, nullptr);
    EXPECT_EQ(telemetry->format.video_codec, IPTV_STREAM_VIDEO_H264);
    EXPECT_EQ(telemetry->format.audio_pid, 0u);
    EXPECT_EQ(telemetry->first_other_stream_type, 0x90u);
    EXPECT_EQ(telemetry->error_count, 0u);
    EXPECT_EQ(iptv_stream_cleanup(&session), IPTV_STREAM_OK);
}

TEST(IptvStreamTest, AcceptsHevcMain10AndRejectsInvalidBitDepths)
{
    struct Case
    {
        unsigned profile, luma, chroma;
        bool supported;
    };
    for (const auto test : {Case{1, 0, 0, true}, Case{2, 2, 2, true}, Case{1, 2, 2, false},
                            Case{2, 4, 4, false}, Case{2, 2, 0, false}, Case{3, 2, 2, false}})
    {
        SCOPED_TRACE(test.profile * 100 + test.luma * 10 + test.chroma);
        std::vector<bool> bits;
        auto put = [&](std::uint32_t value, unsigned count)
        {
            while (count)
                bits.push_back((value >> --count) & 1u);
        };
        auto ue = [&](unsigned value)
        {
            unsigned count = 0;
            for (unsigned code = value + 1; code > 1; code >>= 1)
                ++count;
            put(0, count);
            put(value + 1, count + 1);
        };
        put(0, 4);
        put(0, 3);
        put(1, 1); // VPS ID, sublayers, nesting.
        put(0, 3);
        put(test.profile, 5);
        put(0, 32);
        put(0, 24);
        put(0, 24);
        put(153, 8);
        ue(0);
        ue(1);
        ue(3840);
        ue(2160);
        put(0, 1);
        ue(test.luma);
        ue(test.chroma);
        put(1, 1);
        while (bits.size() % 8)
            bits.push_back(false);
        std::vector<std::uint8_t> payload{0, 0, 0, 1, 0x40, 1, 0x80, 0, 0, 0, 1, 0x42, 1};
        unsigned zeros = 0;
        for (std::size_t i = 0; i < bits.size(); i += 8)
        {
            std::uint8_t byte = 0;
            for (unsigned j = 0; j < 8; ++j)
                byte = (byte << 1) | bits[i + j];
            if (zeros >= 2 && byte <= 3)
            {
                payload.push_back(3);
                zeros = 0;
            }
            payload.push_back(byte);
            zeros = byte == 0 ? zeros + 1 : 0;
        }
        const std::uint8_t frame[] = {0, 0, 0, 1, 0x44, 1, 0x80, 0, 0, 0, 1, 0x26, 1, 0x80,
                                      0, 0, 0, 1, 0x46, 1, 0x50, 0, 0, 0, 1, 0x46, 1, 0x50};
        payload.insert(payload.end(), std::begin(frame), std::end(frame));
        iptv_stream_format_t opened{};
        iptv_stream_backend_t backend{};
        backend.context = &opened;
        backend.open = [](void *context, const iptv_stream_format_t *format)
        {
            *static_cast<iptv_stream_format_t *>(context) = *format;
            return 0;
        };
        backend.submit_video = [](void *, const std::uint8_t *, std::size_t, std::uint64_t)
        { return 0; };
        backend.submit_audio = backend.submit_video;
        backend.disable_audio = [](void *) { return 0; };
        backend.drain = [](void *) { return 0; };
        backend.close = [](void *) {};
        iptv_stream_session_t session{};
        iptv_stream_init(&session);
        ASSERT_EQ(iptv_stream_open(&session, nullptr, &backend), IPTV_STREAM_OK);
        ASSERT_EQ(iptv_stream_start(&session), IPTV_STREAM_OK);
        std::vector<std::uint8_t> bytes;
        AppendPacket(&bytes, PsiPacket(0, PatSection()));
        AppendPacket(&bytes, PsiPacket(0x100, PmtSection(0x0f, 0x111, 0x24)));
        AppendPacket(&bytes, PesPacket(0x110, 0, 0xe0, payload));
        const int result = iptv_stream_push(&session, bytes.data(), bytes.size());
        if (test.supported)
        {
            EXPECT_EQ(result, IPTV_STREAM_OK);
            EXPECT_EQ(opened.video_codec, IPTV_STREAM_VIDEO_HEVC);
            EXPECT_EQ(opened.video_profile, test.profile);
            EXPECT_EQ(opened.video_bit_depth, 8u + test.luma);
            EXPECT_EQ(opened.visible_width, 3840u);
            EXPECT_EQ(opened.visible_height, 2160u);
            // A recurring PMT must preserve the SPS format, not force Main10 to 8-bit.
            bytes.clear();
            AppendPacket(&bytes, PsiPacket(0x100, PmtSection(0x0f, 0x111, 0x24), 1));
            ASSERT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
            EXPECT_EQ(iptv_stream_telemetry(&session)->format.video_bit_depth, 8u + test.luma);
            bytes.clear();
            AppendPacket(&bytes, PesPacket(0x110, 1, 0xe0, payload));
            EXPECT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
            EXPECT_EQ(iptv_stream_telemetry(&session)->error_count, 0u);
            // Live joins/reconnects must discard unavailable-reference RASL, not
            // decodable RADL or the RASL belonging to a later continuous CRA.
            for (const unsigned initial : {21u, 16u, 19u})
            {
                ASSERT_EQ(iptv_stream_discontinuity(&session), IPTV_STREAM_OK);
                const auto before = iptv_stream_telemetry(&session)->video_access_units;
                bytes.clear();
                AppendPacket(&bytes, PsiPacket(0, PatSection()));
                AppendPacket(&bytes, PsiPacket(0x100, PmtSection(0x0f, 0x111, 0x24)));
                unsigned counter = 0;
                for (const unsigned type : {initial, 8u, 6u, 9u, 1u, 8u, 21u, 9u, 1u})
                {
                    payload[payload.size() - sizeof(frame) + 11u] = type << 1;
                    AppendPacket(&bytes, PesPacket(0x110, counter++, 0xe0, payload));
                }
                ASSERT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
                EXPECT_EQ(iptv_stream_telemetry(&session)->video_access_units - before, 6u);
            }
        }
        else
            EXPECT_EQ(result, IPTV_STREAM_UNSUPPORTED_FORMAT);
        (void)iptv_stream_cleanup(&session);
    }
}

} // namespace
