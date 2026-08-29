/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_vp9_packet.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace
{
TEST(Vp9PacketTest, LeavesPlainPacketIntact)
{
    constexpr std::array<std::uint8_t, 2> bytes{0x42, 0xaa};
    iptv_vp9_packet_t packet{};
    ASSERT_EQ(iptv_vp9_split_packet(bytes.data(), bytes.size(), &packet), 0);
    ASSERT_EQ(packet.count, 1u);
    EXPECT_EQ(packet.frames[0].data, bytes.data());
    EXPECT_EQ(packet.frames[0].bytes, bytes.size());
}

TEST(Vp9PacketTest, SplitsAStandardCompoundSuperframe)
{
    constexpr std::array<std::uint8_t, 9> bytes{0x42, 0xaa, 0x02, 0xbb, 0xcc,
                                                0xc1, 0x02, 0x03, 0xc1};
    iptv_vp9_packet_t packet{};
    ASSERT_EQ(iptv_vp9_split_packet(bytes.data(), bytes.size(), &packet), 0);
    ASSERT_EQ(packet.count, 2u);
    EXPECT_EQ(packet.frames[0].data, bytes.data());
    EXPECT_EQ(packet.frames[0].bytes, 2u);
    EXPECT_EQ(packet.frames[1].data, bytes.data() + 2);
    EXPECT_EQ(packet.frames[1].bytes, 3u);
}

TEST(Vp9PacketTest, RejectsMismatchedOrIncompleteIndexes)
{
    constexpr std::array<std::uint8_t, 9> mismatched{0x42, 0xaa, 0x02, 0xbb, 0xcc,
                                                     0xc0, 0x02, 0x03, 0xc1};
    constexpr std::array<std::uint8_t, 9> incomplete{0x42, 0xaa, 0x02, 0xbb, 0xcc,
                                                     0xc1, 0x02, 0x02, 0xc1};
    iptv_vp9_packet_t packet{};
    EXPECT_NE(iptv_vp9_split_packet(mismatched.data(), mismatched.size(), &packet), 0);
    EXPECT_NE(iptv_vp9_split_packet(incomplete.data(), incomplete.size(), &packet), 0);
}

TEST(Vp9PacketTest, ReadsVisibleHiddenAndShowExistingFlags)
{
    // Prefix from the first keyframe of a libvpx-produced Profile 0 fixture.
    constexpr std::array<std::uint8_t, 8> visible{0x82, 0x49, 0x83, 0x42, 0x00, 0x77, 0xf0, 0x43};
    constexpr std::array<std::uint8_t, 1> hidden{0x84};
    constexpr std::array<std::uint8_t, 1> show_existing{0x88};
    iptv_vp9_frame_flags_t flags{};

    ASSERT_EQ(iptv_vp9_read_frame_flags(visible.data(), visible.size(), 0, &flags), 0);
    EXPECT_EQ(flags.show_frame, 1u);
    EXPECT_EQ(flags.show_existing_frame, 0u);
    EXPECT_EQ(flags.displayable, 1u);

    ASSERT_EQ(iptv_vp9_read_frame_flags(hidden.data(), hidden.size(), 0, &flags), 0);
    EXPECT_EQ(flags.show_frame, 0u);
    EXPECT_EQ(flags.show_existing_frame, 0u);
    EXPECT_EQ(flags.displayable, 0u);

    ASSERT_EQ(iptv_vp9_read_frame_flags(show_existing.data(), show_existing.size(), 0, &flags), 0);
    EXPECT_EQ(flags.show_existing_frame, 1u);
    EXPECT_EQ(flags.displayable, 1u);
}

TEST(Vp9PacketTest, RejectsProfileMismatchAndBadFrameMarker)
{
    constexpr std::array<std::uint8_t, 1> profile_two{0x92};
    constexpr std::array<std::uint8_t, 1> bad_marker{0x42};
    iptv_vp9_frame_flags_t flags{};
    EXPECT_NE(iptv_vp9_read_frame_flags(profile_two.data(), profile_two.size(), 0, &flags), 0);
    EXPECT_NE(iptv_vp9_read_frame_flags(bad_marker.data(), bad_marker.size(), 0, &flags), 0);
}
} // namespace
