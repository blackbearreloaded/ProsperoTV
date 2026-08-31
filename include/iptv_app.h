/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "iptv_catalog.h"
#include "iptv_http.h"
#include "iptv_ime.h"
#include "iptv_source_state.h"
#include "iptv_user_state.h"
#include "iptv_xtream.h"

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
    std::uint64_t source_id = 0;
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
    static constexpr unsigned MenuCount = 3;
    static constexpr unsigned GroupCount = 6;
    static constexpr unsigned FacetMax = 24;
    static constexpr unsigned SearchControlCount = 7;

    enum class Screen {
        LiveTv,
        Favorites,
        Sources
    };

    enum class FocusTarget {
        Channel,
        Play,
        Error,
        Group,
        Source,
        SourceRefresh
    };

    struct Facet {
        std::string value;
        unsigned count = 0;
    };

    using SourceSelection = iptv::SourceKind;
    static constexpr unsigned SourceCount = 3;

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
    unsigned last_live_group_ = 0;
    std::array<unsigned, iptv::kDefaultMaxChannels> filtered_indices_{};
    unsigned filtered_count_ = 0;
    char search_query_[IPTV_IME_MAX_TEXT_BYTES]{};
    char filter_country_[48]{};
    char filter_category_[48]{};
    char filter_language_[48]{};
    unsigned filter_quality_ = 0;
    std::array<Facet, FacetMax> country_facets_{};
    std::array<Facet, FacetMax> category_facets_{};
    std::array<Facet, FacetMax> language_facets_{};
    unsigned country_facet_count_ = 0;
    unsigned category_facet_count_ = 0;
    unsigned language_facet_count_ = 0;
    bool search_open_ = false;
    unsigned search_focus_ = 0;
    bool ime_ready_ = false;
    SourceSelection active_source_ = SourceSelection::BuiltIn;
    std::array<SourceHealth, SourceCount> source_health_{};
    std::string custom_source_url_;
    iptv::XtreamCredentials xtream_credentials_{};
    iptv::XtreamCredentials xtream_editor_{};
    enum class XtreamEditorStage { None, Server, Username, Password };
    XtreamEditorStage xtream_editor_stage_ = XtreamEditorStage::None;
    bool xtream_editor_prompt_pending_ = false;
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
    iptv::XtreamStatus pending_xtream_status_ = iptv::XtreamStatus::ok;
    std::string pending_xtream_message_;
    bool pending_cache_saved_ = false;
    iptv::CatalogState catalog_{};
    iptv::UserState user_state_{};
    bool catalog_loaded_ = false;
    bool error_retries_playback_ = false;
    std::string playback_retry_channel_id_;
    bool play_requested_ = false;
    IptvPlayRequest play_request_{};
    bool navigation_ready_ = false;

    void RefreshFocus();
    void SetStatusColour(const char* colour);
    void SetStatusState(const char* label, bool warning, bool error);
    void ShowCatalogError(bool visible, const char* title, const char* message);
    void SetScreen(Screen screen, bool reset_focus = true);
    void RefreshSourceUi();
    void LoadActiveSourceCache();
    void SelectSource(SourceSelection source);
    void OpenCustomSourceEditor();
    void ApplyCustomSourceUrl(const char* url);
    void OpenXtreamEditor();
    void ContinueXtreamEditor();
    void ApplyXtreamServer(const char* server);
    void ApplyXtreamUsername(const char* username);
    void ApplyXtreamPassword(const char* password);
    void RequestRefresh();
    void ConsumeRefresh();
    bool JoinRefreshThread();
    void DismissPlaybackError();
    void RebuildFacets();
    void RebuildFilteredChannels();
    void ApplySearch(const char* query);
    void OpenSearch();
    void CloseSearch();
    void ResetSearch();
    void CycleSearchFilter(unsigned filter, int direction = 1);
    void RefreshSearchUi();
    const iptv::Channel* FindChannelById(const std::string& channel_id) const;
    void QueuePlay(const iptv::Channel& channel);
    void RefreshGroupUi();
    unsigned CatalogIndexAt(unsigned filtered_index) const;
    void RefreshCatalogUi();
    static void* RefreshThreadEntry(void* argument);
    static void SearchResult(const char* text, void* user_data);
    static void CustomSourceResult(const char* text, void* user_data);
    static void XtreamServerResult(const char* text, void* user_data);
    static void XtreamUsernameResult(const char* text, void* user_data);
    static void XtreamPasswordResult(const char* text, void* user_data);
};
