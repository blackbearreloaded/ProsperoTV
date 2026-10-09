// ProsperoTV - Live history retention, seeking, wraparound and concurrent reads.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_timeshift.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

namespace
{
using Packet = std::array<std::uint8_t, 188>;
Packet packet(std::uint64_t ticks, bool random = true, unsigned pid = 256)
{
    Packet p{};
    p.fill(0xff);
    p[0] = 0x47;
    p[1] = 0x40 | (pid >> 8);
    p[2] = pid & 255;
    p[3] = 0x30;
    p[4] = 1;
    p[5] = random ? 0x40 : 0;
    p[6] = p[7] = p[10] = p[11] = 0;
    p[8] = 1;
    p[9] = 0xe0;
    p[12] = p[13] = 0x80;
    p[14] = 5;
    p[15] = 0x21 | ((ticks >> 29) & 14);
    p[16] = ticks >> 22;
    p[17] = 1 | ((ticks >> 14) & 254);
    p[18] = ticks >> 7;
    p[19] = 1 | ((ticks << 1) & 254);
    return p;
}
std::vector<std::uint8_t> broadcast(unsigned count)
{
    std::vector<std::uint8_t> bytes;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto p = packet((i + 2) * 90000, i % 2 == 0);
        bytes.insert(bytes.end(), p.begin(), p.end());
    }
    return bytes;
}
TEST(TimeshiftSeek, AccumulatesQueuedAndInFlightStepsUntilANewPictureArrives)
{
    constexpr std::uint64_t second = 1000000;
    iptv::TimeshiftSeek seek;
    EXPECT_FALSE(seek.take());
    ASSERT_TRUE(seek.relative(0, 300 * second, 240 * second, -30));
    seek.acknowledge(240 * second); // An old picture cannot cancel a queued request.
    ASSERT_TRUE(seek.relative(0, 300 * second, 240 * second, -30));
    EXPECT_EQ(seek.take(), 180 * second);
    EXPECT_FALSE(seek.take());
    seek.acknowledge(UINT64_MAX); // Decoder reset; no new picture yet.
    ASSERT_TRUE(seek.relative(0, 300 * second, 240 * second, -30));
    EXPECT_EQ(seek.take(), 150 * second);
    seek.acknowledge(UINT64_MAX);
    ASSERT_TRUE(seek.relative(0, 300 * second, 240 * second, 30));
    EXPECT_EQ(seek.take(), 180 * second);
    seek.acknowledge(178 * second); // Actual keyframe can precede the requested point.
    ASSERT_TRUE(seek.relative(0, 300 * second, 179 * second, -30));
    EXPECT_EQ(seek.take(), 149 * second);
    seek.live(305 * second);
    EXPECT_EQ(seek.take(), 305 * second);
    seek.acknowledge(UINT64_MAX);
    ASSERT_TRUE(seek.relative(0, 306 * second, 179 * second, -30));
    EXPECT_EQ(seek.take(), 275 * second);
    seek = {}; // A different channel must not inherit the previous target.
    ASSERT_TRUE(seek.relative(0, 60 * second, 40 * second, -30));
    EXPECT_EQ(seek.take(), 10 * second);
}

TEST(Timeshift, ExpiredDecoderConfigurationAlsoExpiresTransportAndSeekPositions)
{
    iptv::Timeshift history(188 * 32);
    const auto data = broadcast(20);
    ASSERT_TRUE(history.append(data.data(), data.size()));
    const auto old = history.seek(2000000);
    ASSERT_TRUE(old);
    history.discard_before(12500000, 0);
    EXPECT_GE(history.range().first_pts_us, 12500000u);
    const auto current = history.seek(2000000);
    ASSERT_TRUE(current);
    EXPECT_GE(current->pts_us, 12500000u);
    std::array<std::uint8_t, 188> output;
    EXPECT_EQ(history.read(old->offset, output.data(), output.size()).status,
              iptv::Timeshift::ReadStatus::expired);
    history.discard_before(100000000, 0);
    EXPECT_FALSE(history.seek(2000000));
    EXPECT_EQ(history.range().begin, history.range().end);
    history.discontinuity();
    ASSERT_TRUE(history.append(data.data(), data.size()));
    history.discard_before(100000000, 0); // A stale owner cannot trim a new timeline.
    EXPECT_TRUE(history.seek(2000000));
    const auto current_read = history.read(history.range().begin, output.data(), output.size());
    ASSERT_EQ(current_read.status, iptv::Timeshift::ReadStatus::data);
    EXPECT_EQ(current_read.generation, 1u);
}

TEST(TimeshiftSeek, ClampsToMovingHistoryAndAvoidsArithmeticOverflow)
{
    iptv::TimeshiftSeek seek;
    ASSERT_TRUE(seek.relative(1000000, 2000000, 1500000, std::numeric_limits<int>::min()));
    EXPECT_EQ(seek.take(), 1000000u);
    ASSERT_TRUE(seek.relative(1500000, 3000000, 1500000, -30));
    EXPECT_EQ(seek.take(), 1500000u); // The old target expired while decoding.
    ASSERT_TRUE(seek.relative(1500000, 3000000, 1500000, std::numeric_limits<int>::max()));
    EXPECT_EQ(seek.take(), 3000000u);
    seek = {};
    ASSERT_TRUE(seek.relative(UINT64_MAX - 1000000, UINT64_MAX - 1, UINT64_MAX - 500000,
                              std::numeric_limits<int>::max()));
    EXPECT_EQ(seek.take(), UINT64_MAX - 1);
    EXPECT_FALSE(seek.relative(20, 10, 15, 30));
    EXPECT_FALSE(seek.take());
    ASSERT_TRUE(seek.relative(0, 1000000, UINT64_MAX, 0));
    EXPECT_EQ(seek.take(), 1000000u);
}

TEST(Timeshift, PreservesFragmentedTransportAndSeeksAtRandomAccessPictures)
{
    const auto input = broadcast(20);
    for (const auto piece : {1u, 7u, 188u, 1000u, 5000u})
    {
        iptv::Timeshift buffer(188 * 64);
        ASSERT_TRUE(buffer.available());
        for (std::size_t at = 0; at < input.size(); at += piece)
            ASSERT_TRUE(
                buffer.append(input.data() + at, std::min<std::size_t>(piece, input.size() - at)));
        std::vector<std::uint8_t> output(input.size());
        const auto got = buffer.read(0, output.data(), output.size());
        ASSERT_EQ(got.status, iptv::Timeshift::ReadStatus::data);
        EXPECT_EQ(got.bytes, input.size());
        EXPECT_EQ(output, input);
        const auto range = buffer.range();
        EXPECT_TRUE(range.timed);
        EXPECT_EQ(range.first_pts_us, 2000000u);
        EXPECT_EQ(range.last_pts_us, 21000000u);
        ASSERT_TRUE(buffer.seek(11000000));
        EXPECT_EQ(buffer.seek(11000000)->pts_us, 10000000u);
        EXPECT_EQ(buffer.seek(11000000)->offset, 188u * 8);
        EXPECT_EQ(buffer.seek(0)->pts_us, 2000000u);
        EXPECT_EQ(buffer.seek(UINT64_MAX)->pts_us, 20000000u);
        EXPECT_EQ(buffer.read(range.end, output.data(), output.size()).status,
                  iptv::Timeshift::ReadStatus::empty);
    }
}
TEST(Timeshift, EnforcesBothByteAndTimeLimitsAndReportsOverwrittenPlayback)
{
    auto input = broadcast(50);
    for (const auto piece : {188u, 1000u, 20000u})
    {
        iptv::Timeshift buffer(188 * 16, 5000000);
        for (std::size_t at = 0; at < input.size(); at += piece)
            ASSERT_TRUE(
                buffer.append(input.data() + at, std::min<std::size_t>(piece, input.size() - at)));
        const auto range = buffer.range();
        EXPECT_LE(range.end - range.begin, 188u * 16);
        EXPECT_LE(range.last_pts_us - range.first_pts_us, 5000000u);
        EXPECT_EQ(range.last_pts_us, 51000000u);
        std::array<std::uint8_t, 512> bytes{};
        EXPECT_EQ(buffer.read(0, bytes.data(), bytes.size()).status,
                  iptv::Timeshift::ReadStatus::expired);
        const auto earliest = buffer.seek(0);
        ASSERT_TRUE(earliest);
        EXPECT_GE(earliest->offset, range.begin);
        const auto read = buffer.read(earliest->offset, bytes.data(), bytes.size());
        ASSERT_EQ(read.status, iptv::Timeshift::ReadStatus::data);
        EXPECT_EQ(bytes[0], 0x47);
        EXPECT_TRUE(std::equal(bytes.begin(), bytes.begin() + read.bytes,
                               input.begin() + earliest->offset));
    }
}
TEST(Timeshift, ExplicitProviderBoundaryExpiresOldBytesEvenWithTheSameClock)
{
    iptv::Timeshift buffer(188 * 64);
    const auto bytes = broadcast(20);
    ASSERT_TRUE(buffer.append(bytes.data(), bytes.size()));
    const auto old = buffer.range();
    buffer.discontinuity();
    EXPECT_FALSE(buffer.range().timed);
    EXPECT_EQ(buffer.range().generation, old.generation + 1);
    EXPECT_EQ(buffer.range().begin, old.end);
    ASSERT_TRUE(buffer.append(bytes.data(), bytes.size()));
    std::array<std::uint8_t, 188> copied{};
    EXPECT_EQ(buffer.read(0, copied.data(), copied.size()).status,
              iptv::Timeshift::ReadStatus::expired);
    const auto first = buffer.seek(0);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->offset, old.end);
    EXPECT_EQ(first->pts_us, old.first_pts_us);
}

TEST(Timeshift, HandlesClockWrapReorderingAndProviderTimelineChanges)
{
    iptv::Timeshift buffer(188 * 64);
    const std::uint64_t wrap = UINT64_C(1) << 33;
    for (auto ticks : {wrap - 180000, wrap - 90000, wrap - 135000, UINT64_C(0), UINT64_C(90000)})
    {
        const auto p = packet(ticks);
        ASSERT_TRUE(buffer.append(p.data(), p.size()));
    }
    auto range = buffer.range();
    EXPECT_EQ(range.generation, 0u);
    EXPECT_EQ(range.last_pts_us, (wrap + 90000) * 1000000 / 90000);
    EXPECT_LE(range.last_pts_us - range.first_pts_us, 3000001u);
    buffer.set_video_pid(256);
    EXPECT_EQ(buffer.range().last_pts_us, range.last_pts_us);
    EXPECT_EQ(buffer.range().generation, 0u);
    const auto reset = packet(30 * 90000);
    ASSERT_TRUE(buffer.append(reset.data(), reset.size()));
    const auto restart = packet(0);
    ASSERT_TRUE(buffer.append(restart.data(), restart.size()));
    range = buffer.range();
    EXPECT_EQ(range.generation, 1u);
    EXPECT_EQ(range.first_pts_us, 0u);
    EXPECT_EQ(range.last_pts_us, 0u);
    EXPECT_EQ(buffer.seek(1000000)->offset, 6u * 188);
}
TEST(Timeshift, UsesSelectedVideoPidAndRejectsMalformedTimestampHeaders)
{
    iptv::Timeshift buffer(188 * 64);
    buffer.set_video_pid(300);
    for (unsigned i = 0; i < 3; ++i)
    {
        const auto other = packet((100 + i) * 90000, true, 400);
        const auto video = packet((2 + i) * 90000, true, 300);
        EXPECT_TRUE(buffer.append(other.data(), other.size()));
        EXPECT_TRUE(buffer.append(video.data(), video.size()));
    }
    EXPECT_EQ(buffer.range().last_pts_us, 4000000u);
    auto malformed = packet(100 * 90000, true, 300);
    malformed[15] &= ~1;
    EXPECT_TRUE(buffer.append(malformed.data(), malformed.size()));
    EXPECT_EQ(buffer.range().last_pts_us, 4000000u);
    EXPECT_EQ(buffer.range().generation, 0u);
    iptv::Timeshift invalid(1);
    EXPECT_FALSE(invalid.available());
    EXPECT_FALSE(invalid.append(malformed.data(), malformed.size()));
    EXPECT_FALSE(buffer.append(nullptr, 1));
}
TEST(Timeshift, ConcurrentDownloadAndPlaybackNeverExposeReusedRingBytes)
{
    iptv::Timeshift buffer(188 * 32);
    std::atomic<bool> finished{false};
    std::thread download(
        [&]
        {
            for (unsigned i = 0; i < 2000; ++i)
            {
                const auto p = packet(static_cast<std::uint64_t>(i) * 9000);
                ASSERT_TRUE(buffer.append(p.data(), p.size()));
            }
            finished = true;
        });
    std::uint64_t cursor = 0;
    Packet out{};
    while (!finished || cursor < buffer.range().end)
    {
        const auto read = buffer.read(cursor, out.data(), out.size());
        if (read.status == iptv::Timeshift::ReadStatus::expired)
        {
            if (const auto next = buffer.seek(0))
                cursor = next->offset;
        }
        else if (read.status == iptv::Timeshift::ReadStatus::data)
        {
            const auto expected = packet(cursor / 188 * 9000);
            EXPECT_EQ(out, expected);
            cursor += read.bytes;
        }
        else
            std::this_thread::yield();
    }
    download.join();
}
TEST(Timeshift, ResynchronizesAndKeepsOneProgrammeClockWithoutRandomAccessFlags)
{
    iptv::Timeshift buffer(188 * 64);
    const std::array<std::uint8_t, 5> junk{1, 2, 0x47, 3, 4};
    ASSERT_TRUE(buffer.append(junk.data(), junk.size()));
    for (unsigned i = 0; i < 20; ++i)
    {
        const auto first = packet((2 + i) * 90000, false, 300);
        const auto other = packet((100 + i) * 90000, false, 400);
        ASSERT_TRUE(buffer.append(first.data(), first.size()));
        ASSERT_TRUE(buffer.append(other.data(), other.size()));
    }
    EXPECT_EQ(buffer.range().last_pts_us, 21000000u);
    EXPECT_EQ(buffer.range().generation, 0u);
    const auto position = buffer.seek(16000000);
    ASSERT_TRUE(position);
    EXPECT_EQ(position->pts_us, 11000000u); // Decoder has five seconds to find an IDR.
    EXPECT_EQ(position->offset, junk.size() + 18 * 188u);
    buffer.set_video_pid(400);
    EXPECT_EQ(buffer.range().first_pts_us, 100000000u);
    EXPECT_EQ(buffer.range().last_pts_us, 119000000u);
    EXPECT_EQ(buffer.range().generation, 1u);
}
} // namespace
