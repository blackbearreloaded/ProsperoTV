/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "iptv_catalog.h"
#include "iptv_http.h"
#include "iptv_ime.h"
#include "iptv_source_state.h"
#include "iptv_user_state.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

enum class IptvInputKey {
    Cross,
    Circle,
    Square,
    Triangle,
    Options,
    L1,
    R1,
    Up,
    Down,
    Left,
    Right
};

struct IptvInputEvent {
    IptvInputKey key;
    bool pressed;
};

struct IptvPlayRequest {
    std::string channel_id;
    std::string channel_name;
    std::vector<std::string> urls;
    std::string user_agent;
    std::string referrer;
};

class IptvApp {
public:
    bool Initialize(Rml::ElementDocument* document);
    void Poll();
    bool HandleInput(const IptvInputEvent& event);
    bool TakePlayRequest(IptvPlayRequest* request);
    void ReportPlaybackFailure(const char* channel_id, const char* channel_name, int result,
                               unsigned attempts);
    void Shutdown();

private:
    static constexpr unsigned MenuCount = 4;

    enum class Screen {
        Home,
        LiveTv,
        Sources,
        Settings
    };

    enum class FocusTarget {
        Channel,
        Play,
        Error,
        LiveSource,
        Group,
        Source,
        SourceRefresh,
        Setting,
        Home
    };

    enum class SourceSelection {
        BuiltIn,
        Custom
    };

    enum class SourceHealth {
        Empty,
        Saved,
        Cached,
        Refreshing,
        Ready,
        Stale,
        Error
    };

    Rml::ElementDocument* document_ = nullptr;
    Screen screen_ = Screen::LiveTv;
    FocusTarget focus_target_ = FocusTarget::Channel;
    unsigned focus_slot_ = 0;
    unsigned selected_slot_ = 0;
    unsigned page_offset_ = 0;
    unsigned selected_group_ = 0;
    std::array<unsigned, iptv::kDefaultMaxChannels> filtered_indices_{};
    unsigned filtered_count_ = 0;
    char search_query_[IPTV_IME_MAX_TEXT_BYTES]{};
    bool ime_ready_ = false;
    SourceSelection active_source_ = SourceSelection::BuiltIn;
    std::array<SourceHealth, 2> source_health_{};
    std::string custom_source_url_;
    SourceSelection refresh_source_ = SourceSelection::BuiltIn;
    std::string refresh_url_;
    std::string refresh_cache_path_;
    std::uint64_t refresh_source_id_ = 0;
    void* refresh_thread_ = nullptr;
    bool refresh_queued_ = false;
    std::atomic<bool> refresh_complete_{false};
    std::atomic<bool> shutdown_requested_{false};
    iptv::http::Status pending_network_status_ = iptv::http::Status::not_initialized;
    iptv::http::FetchResult pending_fetch_{};
    iptv::ParseReport pending_report_{};
    iptv::CatalogState pending_catalog_{};
    bool pending_cache_saved_ = false;
    iptv::CatalogState catalog_{};
    iptv::UserState user_state_{};
    bool catalog_loaded_ = false;
    bool error_retries_playback_ = false;
    std::string playback_retry_channel_id_;
    bool play_requested_ = false;
    IptvPlayRequest play_request_{};

    void RefreshFocus();
    void SetStatusColour(const char* colour);
    void SetStatusState(const char* label, bool warning, bool error);
    void ShowCatalogError(bool visible, const char* title, const char* message);
    void SetScreen(Screen screen);
    void RefreshSourceUi();
    void LoadActiveSourceCache();
    void SelectSource(SourceSelection source);
    void OpenCustomSourceEditor();
    void ApplyCustomSourceUrl(const char* url);
    void RequestRefresh();
    void ConsumeRefresh();
    bool JoinRefreshThread();
    void DismissPlaybackError();
    void RebuildFilteredChannels();
    void ApplySearch(const char* query);
    const iptv::Channel* FindChannelById(const std::string& channel_id) const;
    const iptv::Channel* HomeChannelAt(unsigned slot) const;
    void QueuePlay(const iptv::Channel& channel);
    void RefreshGroupUi();
    unsigned CatalogIndexAt(unsigned filtered_index) const;
    void RefreshCatalogUi();
    static void* RefreshThreadEntry(void* argument);
    static void SearchResult(const char* text, void* user_data);
    static void CustomSourceResult(const char* text, void* user_data);
};
