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

namespace ptv
{
enum class VodKind
{
    movies,
    series,
    episodes
};
bool load_vod(const iptv::XtreamCredentials &account, VodKind kind, std::string_view series,
              iptv::Catalog *catalog, const iptv::http::RequestControl *control,
              std::string *error);

// Shares the app's network time with live-list and programme-guide downloads.
// The Model grants that time through poll(can_download).
class VodLibrary
{
  public:
    ~VodLibrary();
    void configure(iptv::XtreamCredentials account, std::string cache, RefreshSchedule schedule);
    void select(VodKind kind, std::string series = {}, bool force = false);
    void poll(bool can_download);
    void stop();
    bool available() const
    {
        return iptv::ValidateXtreamCredentials(account_);
    }
    bool busy() const
    {
        return thread_ != nullptr;
    }
    bool requested() const
    {
        return requested_;
    }
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
    std::string cache_, series_, key_, status_;
    RefreshSchedule schedule_ = RefreshSchedule::daily;
    VodKind kind_ = VodKind::movies;
    iptv::Catalog catalog_, pending_;
    std::uint64_t saved_ = 0;
    unsigned revision_ = 0;
    bool requested_ = false, loaded_ = false, ok_ = false, saved_ok_ = false;
    bool east_asian_ = false, korean_ = false;
    std::string error_;
    void *thread_ = nullptr;
    std::atomic<bool> done_{false}, stop_{false};
};
} // namespace ptv
