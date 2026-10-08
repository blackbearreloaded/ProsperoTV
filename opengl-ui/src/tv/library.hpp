// ProsperoTV - Saved sources, category visibility and favorite folders.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <utility>

struct sqlite3;

namespace ptv
{

enum class RefreshSchedule : int
{
    daily,
    weekly,
    manual
};
bool refresh_due(RefreshSchedule schedule, std::uint64_t saved, std::uint64_t now);
const char *schedule_name(RefreshSchedule schedule);

// IDs 1..3 are the original built-in, playlist and Xtream source slots.
// Subsequent records allow any number of playlists and accounts.
struct SavedSource
{
    std::int64_t id = 0;
    int kind = 0; // 0: built-in, 1: playlist, 2: Xtream, 3: portal
    std::string name;
    std::string url;
    std::string username;
    std::string password;
    std::string mac;
    RefreshSchedule schedule = RefreshSchedule::daily;
};

// All mutations are committed before callers update their in-memory state.
// The original catalog and favorites files remain readable by older releases.
class Library
{
  public:
    Library() = default;
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    bool open(const std::string &path);
    bool ready() const
    {
        return db_ != nullptr;
    }
    std::vector<SavedSource> sources() const;
    bool save_source(SavedSource *source);
    bool remove_source(std::int64_t id);
    std::int64_t selected_source() const;
    bool select_source(std::int64_t id);
    bool remember_channel(std::int64_t source, std::string_view channel);
    std::pair<std::int64_t, std::string> last_channel() const;
    std::unordered_set<std::string> hidden_categories(std::uint64_t source) const;
    bool hide_category(std::uint64_t source, std::string_view category, bool hidden);
    std::vector<std::string> folders() const;
    bool add_folder(std::string_view name);
    bool rename_folder(std::string_view name, std::string_view replacement);
    bool remove_folder(std::string_view name);
    std::unordered_set<std::string> folder_channels(std::string_view folder) const;
    bool put_in_folder(std::string_view folder, std::string_view channel, bool included);
    bool remove_from_folders(std::string_view channel);

  private:
    sqlite3 *db_ = nullptr;
};

} // namespace ptv
