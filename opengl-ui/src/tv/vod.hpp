// ProsperoTV - Provider movies, series and episodes, using compact catalog storage.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iptv_catalog.h"
#include "iptv_http.h"
#include "iptv_xtream.h"
#include "tv/library.hpp"
#include <atomic>
#include <string>
#include <map>

namespace ptv
{
enum class VodKind
{
    movies,
    series,
    episodes
};
struct VodDetails
{
    std::string plot, cast, genre, year, rating, poster;
    bool complete = false;
};
using VodMetadata = std::map<std::string, VodDetails>;
bool save_vod_metadata(const std::string &path, const VodMetadata &metadata);
VodMetadata load_vod_metadata(const std::string &path, const iptv::Catalog &catalog);
std::vector<std::string> vod_genres(std::string_view genres);
// Past the limit (the catalog's default, or `limit` if smaller) the first titles are kept.
bool load_vod(const iptv::XtreamCredentials &account, VodKind kind, std::string_view series,
              iptv::Catalog *catalog, const iptv::http::RequestControl *control, std::string *error,
              VodMetadata *metadata = nullptr, const iptv::http::RequestHeaders *headers = nullptr,
              bool *truncated = nullptr, std::size_t limit = 0);

// Shares the app's network time with live-list and programme-guide downloads.
// The Model grants that time through poll(can_download).
class VodLibrary
{
  public:
    ~VodLibrary();
    void configure(iptv::XtreamCredentials account, std::string cache, RefreshSchedule schedule,
                   std::string user_agent = {}, std::string referrer = {},
                   std::string extra_headers = {});
    void configure_playlist(const iptv::Catalog *playlist);
    void configure_like(const VodLibrary &source, bool reload = false);
    std::size_t find_series(std::string_view series, std::uint64_t source) const;
    void playlist_updated();
    void select(VodKind kind, std::string series = {}, bool force = false);
    void poll(bool can_download);
    void stop();
    const VodDetails &details(std::string_view id) const;
    // When the provider added a movie or show (Unix seconds), kept in the row's otherwise
    // unused source_line so a large list costs nothing more; 0 when the provider did not say.
    std::uint32_t added(unsigned index) const;
    void request_details(unsigned index);
    bool available() const
    {
        return playlist_ != nullptr || iptv::ValidateXtreamCredentials(account_);
    }
    bool busy() const
    {
        return thread_ != nullptr;
    }
    bool requested() const
    {
        return requested_ || !wanted_id_.empty();
    }
    // The details of this title are on their way (asked for, or being downloaded).
    bool details_loading(std::string_view id) const;
    bool loaded() const
    {
        return loaded_;
    }
    const iptv::Catalog &catalog() const
    {
        return catalog_;
    }
    const std::string &status() const
    {
        return status_;
    }
    unsigned revision() const
    {
        return revision_;
    }
    VodKind kind() const
    {
        return kind_;
    }
    const std::string &series() const
    {
        return series_;
    }
    bool east_asian() const
    {
        return east_asian_;
    }
    bool korean() const
    {
        return korean_;
    }

  private:
    static void *work(void *);
    std::string file() const;
    void note_scripts();
    iptv::XtreamCredentials account_;
    const iptv::Catalog *playlist_ = nullptr;
    std::string cache_, series_, key_, status_, user_agent_, referrer_, extra_headers_;
    RefreshSchedule schedule_ = RefreshSchedule::daily;
    VodKind kind_ = VodKind::movies;
    iptv::Catalog catalog_, pending_;
    VodMetadata metadata_, pending_metadata_;
    std::string detail_id_, detail_provider_id_;
    // The details asked for last, fetched as soon as nothing else is downloading.
    std::string wanted_id_, wanted_provider_id_;
    iptv::XtreamCredentials detail_account_, wanted_account_;
    void start();
    std::uint64_t saved_ = 0;
    unsigned revision_ = 0;
    bool requested_ = false, loaded_ = false, ok_ = false, saved_ok_ = false;
    bool truncated_ = false, pending_truncated_ = false;
    bool east_asian_ = false, korean_ = false;
    std::string error_;
    void *thread_ = nullptr;
    std::atomic<bool> done_{false}, stop_{false};
};
} // namespace ptv
