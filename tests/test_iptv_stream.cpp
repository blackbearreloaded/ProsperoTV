/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_stream.h"

#include <gtest/gtest.h>

#include <array>
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
                                     std::uint8_t video_type = 0x1b)
{
    std::vector<std::uint8_t> section{0x02, 0xb0,       0x17, 0x00, 0x01, 0xc1, 0x00, 0x00,
                                      0xe1, 0x10,       0xf0, 0x00, 0x1b, 0xe1, 0x10, 0xf0,
                                      0x00, audio_type, 0x00, 0x00, 0xf0, 0x00};
    section[18] = static_cast<std::uint8_t>(0xe0u | (audio_pid >> 8));
    section[19] = static_cast<std::uint8_t>(audio_pid);
    section[12] = video_type;
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
        // AAC Main, 48 kHz, stereo, seven-byte ADTS frame. Only LC is supported.
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

void AppendPacket(std::vector<std::uint8_t> *bytes, const Packet &packet)
{
    bytes->insert(bytes->end(), packet.begin(), packet.end());
}

struct FakeBackend
{
    unsigned opens = 0;
    unsigned videos = 0;
    unsigned audios = 0;
    unsigned disables = 0;
    unsigned discontinuities = 0;
    unsigned drains = 0;
    unsigned closes = 0;
    int drain_result = 0;
};

int FakeOpen(void *context, const iptv_stream_format_t *format)
{
    auto *fake = static_cast<FakeBackend *>(context);
    ++fake->opens;
    EXPECT_EQ(format->video_codec, IPTV_STREAM_VIDEO_H264);
    EXPECT_EQ(format->visible_width, 1920u);
    EXPECT_EQ(format->visible_height, 1080u);
    return 0;
}

int FakeVideo(void *context, const std::uint8_t *, std::size_t, std::uint64_t)
{
    ++static_cast<FakeBackend *>(context)->videos;
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

TEST(IptvStreamTest, DisablesUnsupportedAacWithoutFailingVideoSession)
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
    EXPECT_EQ(telemetry->audio_disabled, 1u);
    EXPECT_NE(std::strstr(telemetry->audio_warning, "silent video"), nullptr);
    EXPECT_EQ(telemetry->error_count, 0u);
    EXPECT_EQ(telemetry->video_access_units, 2u);
    EXPECT_EQ(fake.opens, 1u);
    EXPECT_EQ(fake.videos, 2u);
    EXPECT_EQ(fake.audios, 0u);
    EXPECT_EQ(fake.disables, 1u);

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
    const auto bytes = StreamBytes(0x81, null_packet);
    EXPECT_EQ(iptv_stream_push(&session, bytes.data(), bytes.size()), IPTV_STREAM_OK);
    const iptv_stream_telemetry_t *telemetry = iptv_stream_telemetry(&session);
    ASSERT_NE(telemetry, nullptr);
    EXPECT_EQ(telemetry->format.video_codec, IPTV_STREAM_VIDEO_H264);
    EXPECT_EQ(telemetry->format.audio_pid, 0u);
    EXPECT_EQ(telemetry->first_other_stream_type, 0x81u);
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
