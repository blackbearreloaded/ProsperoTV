// ProsperoTV - Stalker/Ministra accounts supplied by the viewer.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iptv_catalog.h"
#include "iptv_http.h"
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ptv
{
struct PortalCredentials
{
    std::string url;
    std::string mac;
};
bool portal_endpoint(std::string_view address, std::string *endpoint);
bool portal_mac(std::string_view input, std::string *mac);
std::uint64_t portal_source_id(const PortalCredentials &credentials);
std::string portal_channel_id(std::uint64_t source, std::string_view id);

// Call from a worker while platform::network_init() is active. No credentials
// or provider replies are logged. Commands remain data; no shell is involved.
class PortalClient
{
  public:
    using Fetch = std::function<iptv::http::FetchResult(
        const char *, char *, std::size_t, std::size_t, const iptv::http::RequestControl *,
        const iptv::http::RequestHeaders *)>;
    PortalClient(PortalCredentials credentials, const iptv::http::RequestControl *control,
                 Fetch fetch = {});
    bool sign_in();
    bool load(iptv::Catalog *catalog, iptv::ParseReport *report,
              std::atomic<unsigned> *progress = nullptr);
    bool resolve(std::string_view command, std::string *url);
    const std::string &error() const
    {
        return error_;
    }

  private:
    using Parameters = std::vector<std::pair<std::string, std::string>>;
    std::string address(std::string_view type, std::string_view action,
                        const Parameters &parameters = {}) const;
    iptv::http::RequestHeaders headers() const;
    bool request(std::string_view type, std::string_view action, std::string *reply,
                 const Parameters &parameters = {});
    PortalCredentials credentials_;
    Fetch fetch_;
    const iptv::http::RequestControl *control_;
    std::string cookie_, authorization_, referrer_, error_;
};

// The menu closes its catalog/guide workers before starting this job. Its
// tuning screen and controller keep running while create_link answers.
class PortalResolveJob
{
  public:
    ~PortalResolveJob();
    bool start(PortalCredentials credentials, std::string command);
    void cancel();
    bool done() const
    {
        return done_.load();
    }
    bool succeeded() const
    {
        return done() && !url_.empty();
    }
    const std::string &url() const
    {
        return url_;
    }
    const std::string &error() const
    {
        return error_;
    }

  private:
    static void *work(void *self);
    void *thread_ = nullptr;
    std::atomic<bool> stop_{false}, done_{false};
    PortalCredentials credentials_;
    std::string command_, url_, error_;
};
} // namespace ptv
