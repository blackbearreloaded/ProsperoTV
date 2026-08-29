/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_USER_STATE_H
#define IPTV_USER_STATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iptv {

inline constexpr char kDefaultFavoritesPath[] =
    "/download0/iptv-favorites-v1.bin";
inline constexpr char kDefaultRecentChannelIdsPath[] =
    "/download0/iptv-history-v1.bin";
inline constexpr char kDefaultHistoryPath[] =
    "/download0/iptv-history-v1.bin";

inline constexpr std::size_t kDefaultMaxFavoriteIds = 256u;
inline constexpr std::size_t kDefaultMaxRecentChannelIds = 128u;
inline constexpr std::size_t kDefaultMaxUserStateIdBytes = 256u;
inline constexpr std::size_t kDefaultMaxUserStateFileBytes = 128u * 1024u;

struct UserState {
    std::vector<std::string> favorite_ids;
    std::vector<std::string> recent_channel_ids;
};

struct UserStateLimits {
    std::size_t max_favorite_ids = kDefaultMaxFavoriteIds;
    std::size_t max_recent_channel_ids = kDefaultMaxRecentChannelIds;
    std::size_t max_id_bytes = kDefaultMaxUserStateIdBytes;
    std::size_t max_file_bytes = kDefaultMaxUserStateFileBytes;
};

enum class UserStateStatus : std::uint8_t {
    ok,
    invalid_argument,
    not_found,
    too_large,
    io_error,
    corrupt,
    unsupported_version,
};

struct UserStateReport {
    UserStateStatus status = UserStateStatus::ok;
    std::size_t favorite_ids = 0;
    std::size_t recent_channel_ids = 0;
    std::size_t bytes = 0;
};

UserStateStatus SaveFavorites(
    const std::string& path,
    const std::vector<std::string>& favorite_ids,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

UserStateStatus LoadFavorites(
    const std::string& path,
    std::vector<std::string>* favorite_ids,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

UserStateStatus SaveRecentChannelIds(
    const std::string& path,
    const std::vector<std::string>& recent_channel_ids,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

UserStateStatus LoadRecentChannelIds(
    const std::string& path,
    std::vector<std::string>* recent_channel_ids,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

// Saves each list to its own atomic file. A failed second save leaves the first
// file intact, but the pair is not a cross-file transaction.
UserStateStatus SaveUserState(
    const std::string& favorites_path,
    const std::string& recent_channel_ids_path,
    const UserState& state,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

UserStateStatus LoadUserState(
    const std::string& favorites_path,
    const std::string& recent_channel_ids_path,
    UserState* state,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

// One-file form for callers that need both lists in one atomic snapshot.
UserStateStatus SaveUserState(
    const std::string& path,
    const UserState& state,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

UserStateStatus LoadUserState(
    const std::string& path,
    UserState* state,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

UserStateStatus SaveUserState(
    const UserState& state,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

UserStateStatus LoadUserState(
    UserState* state,
    const UserStateLimits& limits = UserStateLimits{},
    UserStateReport* report = nullptr);

bool IsFavorite(const UserState& state, std::string_view channel_id);
bool IsRecentChannel(const UserState& state, std::string_view channel_id);

bool AddFavorite(UserState* state,
                 std::string_view channel_id,
                 const UserStateLimits& limits = UserStateLimits{});
bool RemoveFavorite(UserState* state, std::string_view channel_id);
// Returns true when the channel is favorite after the operation.
bool ToggleFavorite(UserState* state,
                    std::string_view channel_id,
                    const UserStateLimits& limits = UserStateLimits{});

// Recent IDs are ordered newest first. Adding an existing ID moves it to the
// front and drops the oldest ID when the configured bound is reached.
bool AddRecentChannel(UserState* state,
                      std::string_view channel_id,
                      const UserStateLimits& limits = UserStateLimits{});
bool RemoveRecentChannel(UserState* state, std::string_view channel_id);

inline bool AddRecent(UserState* state,
                      std::string_view channel_id,
                      const UserStateLimits& limits = UserStateLimits{}) {
    return AddRecentChannel(state, channel_id, limits);
}

inline bool RemoveRecent(UserState* state, std::string_view channel_id) {
    return RemoveRecentChannel(state, channel_id);
}

}  // namespace iptv

#endif
