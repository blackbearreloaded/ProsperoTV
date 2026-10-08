// ProsperoTV - Guide downloads share the catalog's network worker time.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/model.hpp"
#include "tv/platform.hpp"
#include "tv/local_tv.hpp"

namespace ptv
{
std::string Model::guide_path() const
{
    return cache_path(active_source_) + ".guide.sqlite3";
}

void Model::stop_guide()
{
    if (!guide_thread_)
        return;
    const bool completed = guide_done_.load(std::memory_order_acquire);
    guide_stop_.store(true, std::memory_order_release);
    while (!guide_done_.load(std::memory_order_acquire))
    {
        platform::network_cancel();
        platform::sleep_ms(10);
    }
    if (platform::thread_join(guide_thread_) != 0)
        platform::thread_detach(guide_thread_);
    guide_thread_ = nullptr;
    if (completed && guide_ok_)
    {
        guide_ = std::move(pending_guide_);
        rebuild_visible();
    }
    if (!completed)
        next_guide_check_ = 0;
    pending_guide_ = {};
}

void Model::poll_guide()
{
    const auto now = platform::unix_time();
    if (guide_thread_ && guide_done_.load(std::memory_order_acquire))
    {
        if (platform::thread_join(guide_thread_) != 0)
            platform::thread_detach(guide_thread_);
        guide_thread_ = nullptr;
        if (guide_ok_)
        {
            guide_ = std::move(pending_guide_);
            guide_status_ = guide_.count() == 0 ? "No programmes matched this source's channels."
                            : guide_.truncated
                                ? "Guide loaded; this provider exceeded the programme limit."
                            : guide_saved_ ? "Guide up to date"
                                           : "Guide loaded, but could not be saved.";
            rebuild_visible();
        }
        else
            guide_status_ = guide_.count() ? "Guide update failed. Showing the saved programmes."
                                           : "The programme guide could not be downloaded.";
        pending_guide_ = {};
    }
    if (now / 60 != guide_minute_)
    {
        guide_minute_ = now / 60;
        if (!query_.empty() && guide_.count())
            rebuild_visible();
    }
    if (!refreshing() && !guide_thread_ && has_catalog() && now >= next_guide_check_)
    {
        next_guide_check_ = now + 3600;
        if (refresh_due(schedule_, guide_.saved_unix, now))
            refresh_guide();
    }
}

void Model::refresh_guide()
{
    if (refreshing() || vod_.busy() || vod_.requested() || guide_thread_ || !has_catalog())
        return;
    guide_urls_ = catalog_.guide_urls;
    if (active_source_ == iptv::SourceKind::Xtream)
    {
        std::string url;
        if (iptv::BuildXtreamGuideUrl(xtream_, &url))
            guide_urls_ = {std::move(url)};
    }
    next_guide_check_ = platform::unix_time() + 3600;
    if (guide_urls_.empty())
    {
        guide_status_ = "This source has not supplied a programme guide.";
        return;
    }
    guide_file_ = guide_path();
    guide_authorization_ =
        active_source_ == iptv::SourceKind::Tvheadend ? local_tv_authorization(local_source_) : "";
    guide_origin_ = active_source_ == iptv::SourceKind::Tvheadend ? local_source_.url : "";
    guide_ok_ = guide_saved_ = false;
    guide_stop_.store(false, std::memory_order_relaxed);
    guide_done_.store(false, std::memory_order_relaxed);
    guide_thread_ =
        platform::thread_start(&Model::guide_entry, this, 2u * 1024u * 1024u, "iptv-guide");
    guide_status_ =
        guide_thread_ ? "Downloading the programme guide" : "The guide download could not start.";
}
void *Model::guide_entry(void *self)
{
    static_cast<Model *>(self)->run_guide();
    return nullptr;
}
void Model::run_guide()
{
    pending_guide_ = {};
    pending_guide_.source_id = catalog_.source_id;
    pending_guide_.saved_unix = platform::unix_time();
    const iptv::http::RequestControl control{
        [](void *self)
        { return static_cast<Model *>(self)->guide_stop_.load(std::memory_order_acquire); }, this};
    if (platform::network_init() == iptv::http::Status::ok)
    {
        bool all = true;
        for (const auto &url : guide_urls_)
        {
            std::string canonical;
            if (!iptv::CanonicalizeStreamUrl(url, &canonical))
            {
                all = false;
                break;
            }
            XmltvReader reader(catalog_, static_cast<std::int64_t>(platform::unix_time()),
                               pending_guide_);
            const iptv::http::ListSink sink{
                [](void *reader, const char *data, std::size_t bytes)
                { return static_cast<XmltvReader *>(reader)->feed({data, bytes}); }, &reader};
            iptv::http::RequestHeaders headers{};
            headers.authorization = guide_authorization_.c_str();
            headers.credential_origin = guide_origin_.empty() ? nullptr : guide_origin_.c_str();
            const auto scoped =
                iptv::http::HeadersForUrl(canonical.c_str(), canonical.c_str(), headers);
            const auto response = platform::fetch_list(canonical.c_str(), sink,
                                                       512u * 1024u * 1024u, &control, &scoped);
            if (response.status != iptv::http::Status::ok || !reader.finish() ||
                guide_stop_.load(std::memory_order_acquire))
            {
                all = false;
                break;
            }
        }
        guide_ok_ = all;
        if (guide_ok_)
            guide_saved_ = pending_guide_.save(guide_file_);
        platform::network_shutdown();
    }
    guide_done_.store(true, std::memory_order_release);
}

bool Model::play_programme(unsigned index, const Programme &programme)
{
    if (index >= channel_count())
        return false;
    const auto now = static_cast<std::int64_t>(platform::unix_time());
    if (programme.start <= now && programme.end > now)
        return play(index);
    const auto channel = catalog_[index];
    const auto url = catchup_url(channel, programme, now);
    if (url.empty())
    {
        notify(Level::warning, programme.start > now
                                   ? "This programme has not started"
                                   : "Catch-up is not available for this programme");
        return false;
    }
    play_request_ = {};
    play_request_.channel_id = channel.id;
    play_request_.channel_name = std::string(channel.name) + " - " + programme.title;
    play_request_.source_id = channel.source_id;
    play_request_.urls = {url};
    play_request_.user_agent = channel.http_user_agent;
    play_request_.referrer = channel.http_referrer;
    play_request_.record_channel_result = false;
    play_requested_ = true;
    return true;
}
} // namespace ptv
