/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_app.h"

#include "iptv_player.h"
#include "iptv_store.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" int scePthreadCreate(void **thread, const void *attributes, void *(*entry)(void *),
                                void *argument, const char *name);
extern "C" int scePthreadDetach(void *thread);
extern "C" int scePthreadJoin(void *thread, void **result);

namespace
{

constexpr char kCatalogUrl[] = "https://iptv-org.github.io/iptv/index.m3u";
constexpr char kCatalogCachePath[] = "/download0/prosperotv-catalog.sqlite3";
constexpr char kCustomCatalogCachePath[] = "/download0/prosperotv-custom-catalog.sqlite3";
constexpr std::uint64_t kCatalogSourceId = UINT64_C(0x495054562d4f5247);
constexpr std::uint64_t kCatalogRefreshSeconds = UINT64_C(12) * 60u * 60u;
constexpr std::size_t kCatalogThreadStackBytes = 4u * 1024u * 1024u;
constexpr unsigned kChannelCardCount = 8;

bool ContainsCi(const std::string &text, const char *needle)
{
    if (!needle || !*needle)
        return true;
    for (const char *start = text.c_str(); *start; ++start)
    {
        const char *left = start;
        const char *right = needle;
        while (*left && *right &&
               std::tolower(static_cast<unsigned char>(*left)) ==
                   std::tolower(static_cast<unsigned char>(*right)))
        {
            ++left;
            ++right;
        }
        if (!*right)
            return true;
    }
    return false;
}

void FirstValue(const std::string &source, char *output, std::size_t output_bytes)
{
    if (!output || !output_bytes)
        return;
    std::size_t start = 0;
    while (start < source.size() && std::isspace(static_cast<unsigned char>(source[start])))
        ++start;
    std::size_t end = start;
    while (end < source.size() && source[end] != ',' && source[end] != ';' && source[end] != '|')
        ++end;
    while (end > start && std::isspace(static_cast<unsigned char>(source[end - 1])))
        --end;
    const std::size_t bytes = std::min(end - start, output_bytes - 1u);
    std::memcpy(output, source.data() + start, bytes);
    output[bytes] = '\0';
}

bool EqualsCi(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index])))
            return false;
    return true;
}

bool FieldHasValue(const std::string &field, const char *value)
{
    if (!value || !*value)
        return true;
    std::size_t start = 0;
    while (start < field.size())
    {
        while (start < field.size() &&
               (field[start] == ',' || field[start] == ';' || field[start] == '|' ||
                std::isspace(static_cast<unsigned char>(field[start]))))
            ++start;
        std::size_t end = start;
        while (end < field.size() && field[end] != ',' && field[end] != ';' && field[end] != '|')
            ++end;
        std::size_t trimmed = end;
        while (trimmed > start && std::isspace(static_cast<unsigned char>(field[trimmed - 1])))
            --trimmed;
        if (EqualsCi(std::string_view(field).substr(start, trimmed - start), value))
            return true;
        start = end + 1u;
    }
    return false;
}

void BuildChannelMonogram(const iptv::Channel &channel, char output[3])
{
    const std::string &source = !channel.tvg_name.empty() ? channel.tvg_name
                                : !channel.tvg_id.empty() ? channel.tvg_id
                                                          : channel.name;
    unsigned count = 0;
    bool word_start = true;
    for (unsigned char value : source)
    {
        const bool ascii_alnum = value < 0x80 && std::isalnum(value);
        if (ascii_alnum && word_start && count < 2)
            output[count++] = static_cast<char>(std::toupper(value));
        word_start = !ascii_alnum;
    }
    if (count < 2)
    {
        bool skipped_first = false;
        for (unsigned char value : source)
        {
            if (value >= 0x80 || !std::isalnum(value))
                continue;
            if (!skipped_first)
            {
                skipped_first = true;
                continue;
            }
            output[count++] = static_cast<char>(std::toupper(value));
            if (count == 2)
                break;
        }
    }
    if (!count)
    {
        output[0] = 'T';
        output[1] = 'V';
        count = 2;
    }
    output[count] = '\0';
}

bool ChannelContainsCi(const iptv::Channel &channel, const char *needle)
{
    return ContainsCi(channel.name, needle) || ContainsCi(channel.tvg_name, needle) ||
           ContainsCi(channel.url, needle);
}

unsigned ChannelQuality(const iptv::Channel &channel)
{
    if (ChannelContainsCi(channel, "2160p") || ChannelContainsCi(channel, "3840x2160") ||
        ChannelContainsCi(channel, " 4k") || ChannelContainsCi(channel, "uhd"))
        return 4;
    if (ChannelContainsCi(channel, "1080p") || ChannelContainsCi(channel, "1920x1080") ||
        ChannelContainsCi(channel, "fhd"))
        return 3;
    if (ChannelContainsCi(channel, "720p") || ChannelContainsCi(channel, "1280x720") ||
        ChannelContainsCi(channel, " hd"))
        return 2;
    if (ChannelContainsCi(channel, "576p") || ChannelContainsCi(channel, "480p") ||
        ChannelContainsCi(channel, "360p") || ChannelContainsCi(channel, "240p") ||
        ChannelContainsCi(channel, " sd"))
        return 1;
    return 0;
}

const char *QualityName(unsigned quality)
{
    static constexpr const char *names[] = {"Any quality", "SD", "HD / 720p", "Full HD / 1080p",
                                            "4K / UHD"};
    return quality < 5 ? names[quality] : names[0];
}

void BuildChannelTechnicalMeta(const iptv::Channel &channel, char *output, std::size_t output_bytes)
{
    const char *codec = "AUTO";
    const char *resolution = "AUTO";
    const char *frame_rate = "AUTO";

    if (ChannelContainsCi(channel, ".webm") || ChannelContainsCi(channel, "vp9"))
        codec = "VP9";
    else if (ChannelContainsCi(channel, "hevc") || ChannelContainsCi(channel, "h265") ||
             ChannelContainsCi(channel, "h.265"))
        codec = "HEVC";
    else if (ChannelContainsCi(channel, "h264") || ChannelContainsCi(channel, "h.264") ||
             ChannelContainsCi(channel, "avc"))
        codec = "H264";

    static constexpr struct
    {
        const char *needle;
        const char *label;
    } resolutions[] = {{"3840x2160", "2160P"}, {"2160p", "2160P"},     {"2560x1440", "1440P"},
                       {"1440p", "1440P"},     {"1920x1080", "1080P"}, {"1080p", "1080P"},
                       {"1280x720", "720P"},   {"720p", "720P"},       {"720x576", "576P"},
                       {"576p", "576P"},       {"720x480", "480P"},    {"480p", "480P"}};
    for (const auto &candidate : resolutions)
        if (ChannelContainsCi(channel, candidate.needle))
        {
            resolution = candidate.label;
            break;
        }

    static constexpr struct
    {
        const char *needle;
        const char *label;
    } frame_rates[] = {{"120fps", "120"}, {"60fps", "60"}, {"59.94fps", "59.94"},
                       {"50fps", "50"},   {"30fps", "30"}, {"29.97fps", "29.97"},
                       {"25fps", "25"},   {"24fps", "24"}, {"23.976fps", "23.98"}};
    for (const auto &candidate : frame_rates)
        if (ChannelContainsCi(channel, candidate.needle))
        {
            frame_rate = candidate.label;
            break;
        }

    std::snprintf(output, output_bytes, "%s | %s | FPS %s", codec, resolution, frame_rate);
}

void BuildChannelContext(const iptv::Channel &channel, char *output, std::size_t output_bytes)
{
    const char *country = channel.tvg_country.empty() ? "World" : channel.tvg_country.c_str();
    const char *language =
        channel.tvg_language.empty() ? "Unknown language" : channel.tvg_language.c_str();
    const char *category =
        channel.group_title.empty() ? "Uncategorized" : channel.group_title.c_str();
    std::snprintf(output, output_bytes, "%.28s  |  %.40s  |  %.52s", country, language, category);
}

bool MatchesQuery(const iptv::Channel &channel, const char *query)
{
    if (!query || !*query)
        return true;
    if (ContainsCi(channel.name, query) || ContainsCi(channel.tvg_name, query) ||
        ContainsCi(channel.tvg_id, query) || ContainsCi(channel.group_title, query) ||
        ContainsCi(channel.tvg_country, query) || ContainsCi(channel.tvg_language, query))
        return true;
    for (const std::string &group : channel.alternate_group_titles)
        if (ContainsCi(group, query))
            return true;
    const unsigned quality = ChannelQuality(channel);
    return (quality == 1 && (ContainsCi("SD 480P 576P", query))) ||
           (quality == 2 && ContainsCi("HD 720P", query)) ||
           (quality == 3 && ContainsCi("FULL HD FHD 1080P", query)) ||
           (quality == 4 && ContainsCi("4K UHD 2160P", query));
}

bool MatchesGroup(const iptv::Channel &channel, unsigned group, const iptv::UserState &user_state)
{
    if (group == 0)
        return true;
    if (group == 1)
        return iptv::IsFavorite(user_state, channel.id);
    if (group == 2)
        return iptv::IsRecentChannel(user_state, channel.id);
    const char *term = group == 3 ? "news" : group == 4 ? "sport" : "kid";
    if (ContainsCi(channel.group_title, term))
        return true;
    for (const std::string &alternate : channel.alternate_group_titles)
        if (ContainsCi(alternate, term))
            return true;
    return false;
}

bool MatchesFilters(const iptv::Channel &channel, const char *country, const char *category,
                    const char *language, unsigned quality)
{
    if (country && *country && !FieldHasValue(channel.tvg_country, country))
        return false;
    if (language && *language && !FieldHasValue(channel.tvg_language, language))
        return false;
    if (category && *category)
    {
        bool found = FieldHasValue(channel.group_title, category);
        for (const std::string &alternate : channel.alternate_group_titles)
            found = found || FieldHasValue(alternate, category);
        if (!found)
            return false;
    }
    return quality == 0 || ChannelQuality(channel) == quality;
}

Rml::Element *Find(Rml::ElementDocument *document, const char *id)
{
    return document ? document->GetElementById(id) : nullptr;
}

void SetText(Rml::ElementDocument *document, const char *id, const std::string &text)
{
    if (Rml::Element *element = Find(document, id))
        element->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
}

void SetText(Rml::ElementDocument *document, const char *id, const char *text)
{
    SetText(document, id, std::string(text ? text : ""));
}

void SetClass(Rml::ElementDocument *document, const char *id, const char *class_name, bool enabled)
{
    if (Rml::Element *element = Find(document, id))
        element->SetClass(class_name, enabled);
}

void SetVisible(Rml::ElementDocument *document, const char *id, bool visible)
{
    SetClass(document, id, "hidden", !visible);
}

void SetProperty(Rml::ElementDocument *document, const char *id, const char *name,
                 const char *value)
{
    if (Rml::Element *element = Find(document, id))
        element->SetProperty(name, value);
}

void SetPlaybackBadge(Rml::ElementDocument *document, const char *id, iptv::PlaybackStatus status)
{
    const bool visited = status != iptv::PlaybackStatus::unknown;
    const bool playable = status == iptv::PlaybackStatus::playable;
    SetText(document, id, playable ? "PLAYABLE" : "FAILED");
    SetClass(document, id, "active", visited);
    SetClass(document, id, "playable", playable);
    SetClass(document, id, "failed", visited && !playable);
}

const char *FetchStatusName(iptv::http::Status status)
{
    switch (status)
    {
    case iptv::http::Status::invalid_argument:
        return "invalid request";
    case iptv::http::Status::unsupported_url:
        return "unsupported URL";
    case iptv::http::Status::not_initialized:
        return "HTTP not initialized";
    case iptv::http::Status::platform_unavailable:
        return "HTTP unavailable";
    case iptv::http::Status::network_init_failed:
        return "HTTP setup failed";
    case iptv::http::Status::request_failed:
        return "request failed";
    case iptv::http::Status::http_status_error:
        return "HTTP status error";
    case iptv::http::Status::response_too_large:
        return "playlist too large";
    case iptv::http::Status::read_failed:
        return "playlist read failed";
    case iptv::http::Status::deadline_exceeded:
        return "playlist request timed out";
    case iptv::http::Status::redirect_error:
        return "redirect failed";
    case iptv::http::Status::cancelled:
        return "request cancelled";
    case iptv::http::Status::ok:
        return "ok";
    }
    return "unknown HTTP error";
}

} // namespace

bool IptvApp::Initialize(Rml::ElementDocument *document)
{
    if (!document)
        return false;

    const bool restore_navigation = navigation_ready_;
    document_ = document;
    if (!restore_navigation)
    {
        screen_ = Screen::LiveTv;
        focus_target_ = FocusTarget::Channel;
        focus_slot_ = selected_slot_ = page_offset_ = 0;
        selected_group_ = last_live_group_ = 0;
        search_query_[0] = '\0';
        filter_country_[0] = filter_category_[0] = filter_language_[0] = '\0';
        filter_quality_ = 0;
    }
    search_open_ = false;
    search_focus_ = 0;
    filtered_count_ = 0;
    active_source_ = SourceSelection::BuiltIn;
    source_health_.fill(SourceHealth::Empty);
    custom_source_url_.clear();
    refresh_url_.clear();
    refresh_cache_path_.clear();
    refresh_source_id_ = 0;
    catalog_ = {};
    user_state_ = {};
    catalog_loaded_ = false;
    error_retries_playback_ = false;
    playback_retry_channel_id_.clear();
    play_requested_ = false;
    play_request_ = {};
    refresh_queued_ = false;
    refresh_complete_.store(false, std::memory_order_relaxed);
    shutdown_requested_.store(false, std::memory_order_relaxed);

    std::string saved_custom_url;
    if (iptv::LoadCustomSourceUrl(&saved_custom_url) == iptv::SourceStateStatus::ok)
    {
        custom_source_url_ = std::move(saved_custom_url);
        source_health_[1] = SourceHealth::Saved;
    }

    bool saved_custom_active = false;
    if (iptv::LoadActiveSource(&saved_custom_active) == iptv::SourceStateStatus::ok &&
        saved_custom_active && !custom_source_url_.empty())
    {
        active_source_ = SourceSelection::Custom;
    }

    const bool custom_active = active_source_ == SourceSelection::Custom;
    const char *active_cache_path = custom_active ? kCustomCatalogCachePath : kCatalogCachePath;
    const std::uint64_t active_source_id =
        custom_active ? iptv::CustomSourceId(custom_source_url_) : kCatalogSourceId;
    iptv::StoreReport cache_report;
    const iptv::StoreStatus cache_status =
        iptv::LoadCatalog(active_cache_path, &catalog_, {}, &cache_report);
    (void)iptv::LoadUserState(&user_state_);
    catalog_loaded_ = cache_status == iptv::StoreStatus::ok &&
                      catalog_.source_id == active_source_id && !catalog_.channels.empty();
    if (!catalog_loaded_)
        catalog_ = {};
    else
        (void)iptv::LoadPlaybackResults(iptv::kDefaultPlaybackHistoryPath, active_source_id,
                                        &catalog_);
    source_health_[static_cast<unsigned>(active_source_)] = catalog_loaded_ ? SourceHealth::Cached
                                                            : custom_active ? SourceHealth::Saved
                                                                            : SourceHealth::Empty;
    RebuildFacets();
    RebuildFilteredChannels();
    ime_ready_ = iptv_ime_init();

    document_->Show();
    RefreshCatalogUi();
    SetScreen(screen_, !restore_navigation);
    RefreshSourceUi();

    if (catalog_loaded_)
    {
        SetStatusState("Cached catalog", true, false);
        SetText(document_, "source-status-label",
                custom_active ? "Cached custom playlist ready" : "Cached catalog ready");
        SetText(document_, "source-status-detail",
                "Using the last good catalog while the public source refreshes in the background.");
        SetText(document_, "source-status-0", "CACHED");
    }
    else
    {
        SetStatusState("Loading catalog", true, false);
        SetText(document_, "source-status-label", "Starting catalog refresh");
        SetText(document_, "source-status-detail",
                cache_status == iptv::StoreStatus::not_found
                    ? "No local catalog is available yet. The first refresh runs off the UI thread."
                    : "The local catalog is unavailable or corrupt. A fresh copy will be requested "
                      "off the UI thread.");
        SetText(document_, "source-status-0", "LOADING");
    }
    RefreshSourceUi();
    ShowCatalogError(false, "", "");
    const std::time_t now = std::time(nullptr);
    const bool cache_fresh =
        catalog_loaded_ && now > 0 && cache_report.saved_unix != 0 &&
        static_cast<std::uint64_t>(now) >= cache_report.saved_unix &&
        static_cast<std::uint64_t>(now) - cache_report.saved_unix < kCatalogRefreshSeconds;
    if (!cache_fresh)
    {
        RequestRefresh();
    }
    else
    {
        SetText(document_, "source-status-detail",
                "The cached catalog is current. Use Options to refresh it now.");
    }
    navigation_ready_ = true;
    return true;
}

void IptvApp::Shutdown()
{
    shutdown_requested_.store(true, std::memory_order_release);
    if (ime_ready_)
    {
        iptv_ime_shutdown();
        ime_ready_ = false;
    }
    while (refresh_thread_ && !refresh_complete_.load(std::memory_order_acquire))
    {
        iptv::http::CancelActivePlaylistRequest();
        SDL_Delay(10);
    }
    while (refresh_thread_ && !JoinRefreshThread())
        SDL_Delay(10);
    refresh_queued_ = false;
    refresh_complete_.store(false, std::memory_order_relaxed);
    shutdown_requested_.store(false, std::memory_order_relaxed);
    pending_catalog_ = {};
    document_ = nullptr;
    catalog_loaded_ = false;
}

void IptvApp::Poll()
{
    if (ime_ready_)
        iptv_ime_poll();
    ConsumeRefresh();
}

bool IptvApp::TakePlayRequest(IptvPlayRequest *request)
{
    if (!request || !play_requested_)
        return false;
    *request = std::move(play_request_);
    play_request_ = {};
    play_requested_ = false;
    return true;
}

void IptvApp::ReportPlaybackFailure(const char *channel_id, const char *channel_name, int result,
                                    unsigned attempts)
{
    if (!document_ || result >= 0)
        return;
    char message[384]{};
    const char *detail = iptv_player_last_error();
    std::snprintf(message, sizeof(message), "Unable to play %.120s after %u stream attempt%s. %s",
                  channel_name && *channel_name ? channel_name : "this channel", attempts,
                  attempts == 1u ? "" : "s",
                  detail && *detail ? detail : "Try the channel again or select another source.");
    std::fprintf(stderr, "[ProsperoTV][player] channel=%s result=%d attempts=%u reason=%s\n",
                 channel_id ? channel_id : "", result, attempts,
                 detail && *detail ? detail : "unspecified");
    playback_retry_channel_id_ = channel_id ? channel_id : "";
    error_retries_playback_ = FindChannelById(playback_retry_channel_id_) != nullptr;
    SetStatusState("Playback failed", false, true);
    ShowCatalogError(true, "Channel playback failed", message);
    SetText(document_, "error-action-label",
            error_retries_playback_ ? "Retry channel" : "Reload catalog");
    focus_target_ = FocusTarget::Error;
    RefreshFocus();
}

bool IptvApp::JoinRefreshThread()
{
    if (!refresh_thread_)
        return true;
    void *result = nullptr;
    const int join_result = scePthreadJoin(refresh_thread_, &result);
    if (join_result != 0)
    {
        if (!refresh_complete_.load(std::memory_order_acquire))
            return false;
        const int detach_result = scePthreadDetach(refresh_thread_);
        std::fprintf(stderr, "[ProsperoTV] refresh join failed: %d; detach fallback: %d\n",
                     join_result, detach_result);
    }
    refresh_thread_ = nullptr;
    return true;
}

void IptvApp::DismissPlaybackError()
{
    error_retries_playback_ = false;
    playback_retry_channel_id_.clear();
    ShowCatalogError(false, "", "");
}

void IptvApp::SetScreen(Screen screen, bool reset_focus)
{
    if (error_retries_playback_)
        DismissPlaybackError();
    if (screen_ == Screen::LiveTv && screen != Screen::LiveTv)
        last_live_group_ = selected_group_;
    screen_ = screen;
    if (screen_ == Screen::Favorites)
        selected_group_ = 1;
    else if (screen_ == Screen::LiveTv)
        selected_group_ = last_live_group_;
    if (screen_ != Screen::Sources)
        RebuildFilteredChannels();
    if (reset_focus)
    {
        focus_slot_ = 0;
        switch (screen_)
        {
        case Screen::LiveTv:
            focus_target_ = catalog_.channels.empty() ? FocusTarget::Error
                            : filtered_count_         ? FocusTarget::Channel
                                                      : FocusTarget::Group;
            break;
        case Screen::Favorites:
            focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Play;
            break;
        case Screen::Sources:
            focus_target_ = FocusTarget::Source;
            break;
        }
    }

    SetVisible(document_, "screen-live-tv", screen_ != Screen::Sources);
    SetVisible(document_, "screen-sources", screen_ == Screen::Sources);
    SetClass(document_, "screen-live-tv", "favorites-view", screen_ == Screen::Favorites);
    SetText(document_, "live-tv-heading", screen_ == Screen::Favorites ? "Favorites" : "Live TV");
    SetText(document_, "live-tv-subtitle",
            screen_ == Screen::Favorites
                ? "Your saved channels. Square removes a favorite; Triangle searches this list."
                : "Browse the cached catalog instantly. Circle returns to Quick Views from any "
                  "channel page.");
    RefreshCatalogUi();
    RefreshFocus();
}

bool IptvApp::HandleInput(const IptvInputEvent &event)
{
    if (!event.pressed)
        return true;
    if (search_open_)
    {
        if (event.key == IptvInputKey::Circle || event.key == IptvInputKey::Triangle)
        {
            CloseSearch();
            return true;
        }
        if (event.key == IptvInputKey::Up)
            search_focus_ = search_focus_ ? search_focus_ - 1u : SearchControlCount - 1u;
        else if (event.key == IptvInputKey::Down)
            search_focus_ = (search_focus_ + 1u) % SearchControlCount;
        else if ((event.key == IptvInputKey::Left || event.key == IptvInputKey::Right) &&
                 search_focus_ >= 1 && search_focus_ <= 4)
            CycleSearchFilter(search_focus_ - 1u, event.key == IptvInputKey::Left ? -1 : 1);
        else if ((event.key == IptvInputKey::Left || event.key == IptvInputKey::Right) &&
                 search_focus_ >= 5)
            search_focus_ = search_focus_ == 5 ? 6 : 5;
        else if (event.key == IptvInputKey::Cross)
        {
            if (search_focus_ == 0)
            {
                if (ime_ready_)
                    iptv_ime_request(search_query_, &IptvApp::SearchResult, this);
                else
                    SetText(document_, "search-help", "Native keyboard unavailable.");
            }
            else if (search_focus_ <= 4)
            {
                CycleSearchFilter(search_focus_ - 1u, 1);
            }
            else if (search_focus_ == 5)
            {
                ResetSearch();
            }
            else
            {
                CloseSearch();
                return true;
            }
        }
        RefreshSearchUi();
        RefreshFocus();
        return true;
    }
    if (event.key == IptvInputKey::Circle)
    {
        if (error_retries_playback_)
        {
            DismissPlaybackError();
            focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Group;
            focus_slot_ = filtered_count_ ? 0 : selected_group_;
            RefreshCatalogUi();
            RefreshFocus();
        }
        else if (search_query_[0] || filter_country_[0] || filter_category_[0] ||
                 filter_language_[0] || filter_quality_)
        {
            iptv_ime_cancel();
            ResetSearch();
        }
        else if (screen_ != Screen::LiveTv)
        {
            SetScreen(Screen::LiveTv);
        }
        else if (focus_target_ == FocusTarget::Channel || focus_target_ == FocusTarget::Play)
        {
            page_offset_ = 0;
            if (filtered_count_)
                selected_slot_ = CatalogIndexAt(0);
            focus_target_ = FocusTarget::Group;
            focus_slot_ = selected_group_;
            RefreshCatalogUi();
            RefreshFocus();
        }
        return true;
    }

    if (event.key == IptvInputKey::L1 || event.key == IptvInputKey::R1)
    {
        int next = static_cast<int>(screen_) + (event.key == IptvInputKey::R1 ? 1 : -1);
        if (next < 0)
            next = MenuCount - 1;
        if (next >= static_cast<int>(MenuCount))
            next = 0;
        SetScreen(static_cast<Screen>(next));
        return true;
    }

    if (event.key == IptvInputKey::Options)
    {
        RequestRefresh();
        return true;
    }

    if (screen_ != Screen::Sources)
    {
        const unsigned total = filtered_count_;
        const unsigned available =
            page_offset_ < total ? (total - page_offset_ < kChannelCardCount ? total - page_offset_
                                                                             : kChannelCardCount)
                                 : 0;
        if (event.key == IptvInputKey::Triangle)
        {
            OpenSearch();
        }
        else if (event.key == IptvInputKey::Square)
        {
            unsigned channel_index = selected_slot_;
            if (focus_target_ == FocusTarget::Channel && focus_slot_ < available)
                channel_index = CatalogIndexAt(page_offset_ + focus_slot_);
            if (available && channel_index < catalog_.channels.size())
            {
                selected_slot_ = channel_index;
                const iptv::Channel &channel = catalog_.channels[channel_index];
                const std::vector<std::string> previous = user_state_.favorite_ids;
                const bool favorite = iptv::ToggleFavorite(&user_state_, channel.id);
                if (iptv::SaveUserState(user_state_) != iptv::UserStateStatus::ok)
                {
                    user_state_.favorite_ids = previous;
                    SetStatusState("Favorite not saved", false, true);
                    SetText(document_, "preview-status-title", "Storage unavailable");
                    SetText(document_, "preview-status-message",
                            "The /download0 favorite record could not be updated.");
                }
                else
                {
                    RebuildFilteredChannels();
                    if (screen_ == Screen::Favorites && !filtered_count_)
                        focus_target_ = FocusTarget::Play;
                    SetStatusState(favorite ? "Added to favorites" : "Removed from favorites",
                                   false, false);
                    SetText(document_, "preview-status-title",
                            favorite ? "Favorite saved" : "Favorite removed");
                    SetText(document_, "preview-status-message", channel.name);
                }
            }
        }
        else if (event.key == IptvInputKey::Cross)
        {
            if (focus_target_ == FocusTarget::Error)
            {
                if (error_retries_playback_)
                {
                    const iptv::Channel *channel = FindChannelById(playback_retry_channel_id_);
                    if (channel)
                    {
                        DismissPlaybackError();
                        QueuePlay(*channel);
                    }
                }
                else
                {
                    RequestRefresh();
                }
            }
            else if (focus_target_ == FocusTarget::Group)
            {
                selected_group_ = focus_slot_;
                last_live_group_ = selected_group_;
                page_offset_ = selected_slot_ = 0;
                RebuildFilteredChannels();
                focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Group;
                focus_slot_ = filtered_count_ ? 0 : selected_group_;
            }
            else if (focus_target_ == FocusTarget::Channel && focus_slot_ < available &&
                     focus_slot_ < kChannelCardCount)
            {
                selected_slot_ = CatalogIndexAt(page_offset_ + focus_slot_);
                RefreshCatalogUi();
                const iptv::Channel &channel = catalog_.channels[selected_slot_];
                QueuePlay(channel);
            }
            else if (focus_target_ == FocusTarget::Play &&
                     selected_slot_ < catalog_.channels.size())
            {
                const iptv::Channel &channel = catalog_.channels[selected_slot_];
                QueuePlay(channel);
            }
        }
        else if (focus_target_ == FocusTarget::Channel && available &&
                 focus_slot_ < kChannelCardCount)
        {
            const unsigned slot = focus_slot_;
            if (event.key == IptvInputKey::Left && slot % 4)
            {
                focus_slot_ = slot - 1;
            }
            else if (event.key == IptvInputKey::Right)
            {
                if (slot % 4 < 3 && slot + 1 < available && slot + 1 < kChannelCardCount)
                    focus_slot_ = slot + 1;
                else
                    focus_target_ = FocusTarget::Play;
            }
            else if (event.key == IptvInputKey::Up && slot >= 4)
            {
                focus_slot_ = slot - 4;
            }
            else if (event.key == IptvInputKey::Up && slot < 4)
            {
                if (page_offset_ >= kChannelCardCount)
                {
                    page_offset_ -= kChannelCardCount;
                    const unsigned previous_available = total - page_offset_ < kChannelCardCount
                                                            ? total - page_offset_
                                                            : kChannelCardCount;
                    const unsigned previous_slot = slot + 4;
                    focus_slot_ = previous_slot < previous_available ? previous_slot
                                                                     : previous_available - 1u;
                }
                else
                {
                    if (screen_ == Screen::LiveTv)
                    {
                        focus_target_ = FocusTarget::Group;
                        focus_slot_ = selected_group_;
                    }
                }
            }
            else if (event.key == IptvInputKey::Down)
            {
                if (slot < 4 && slot + 4 < available && slot + 4 < kChannelCardCount)
                {
                    focus_slot_ = slot + 4;
                }
                else if (page_offset_ + available < total)
                {
                    page_offset_ += kChannelCardCount;
                    const unsigned next_available = total - page_offset_ < kChannelCardCount
                                                        ? total - page_offset_
                                                        : kChannelCardCount;
                    const unsigned next_slot = slot % 4;
                    focus_slot_ = next_slot < next_available ? next_slot : next_available - 1u;
                }
                else
                {
                    focus_target_ = FocusTarget::Play;
                }
            }
        }
        else if (focus_target_ == FocusTarget::Play && filtered_count_ &&
                 (event.key == IptvInputKey::Left || event.key == IptvInputKey::Up))
        {
            focus_target_ = FocusTarget::Channel;
        }
        else if (focus_target_ == FocusTarget::Group)
        {
            if (event.key == IptvInputKey::Left && focus_slot_)
            {
                --focus_slot_;
            }
            else if (event.key == IptvInputKey::Right && focus_slot_ + 1u < GroupCount)
            {
                ++focus_slot_;
            }
            else if (event.key == IptvInputKey::Down)
            {
                focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Group;
                focus_slot_ = 0;
            }
        }
        else if (focus_target_ == FocusTarget::Error &&
                 (event.key == IptvInputKey::Left || event.key == IptvInputKey::Up ||
                  event.key == IptvInputKey::Right || event.key == IptvInputKey::Down))
        {
            if (error_retries_playback_)
            {
                DismissPlaybackError();
                focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Group;
                focus_slot_ = filtered_count_ ? 0 : selected_group_;
            }
            else
            {
                RequestRefresh();
            }
        }
        if (focus_target_ == FocusTarget::Channel)
        {
            const unsigned current_available =
                page_offset_ < total
                    ? (total - page_offset_ < kChannelCardCount ? total - page_offset_
                                                                : kChannelCardCount)
                    : 0;
            if (focus_slot_ < current_available)
                selected_slot_ = CatalogIndexAt(page_offset_ + focus_slot_);
        }
        RefreshCatalogUi();
        RefreshFocus();
        return true;
    }

    if (screen_ == Screen::Sources)
    {
        if (focus_target_ == FocusTarget::SourceRefresh)
        {
            if (event.key == IptvInputKey::Left || event.key == IptvInputKey::Up)
                focus_target_ = FocusTarget::Source;
            else if (event.key == IptvInputKey::Cross)
                RequestRefresh();
        }
        else if (focus_target_ == FocusTarget::Source)
        {
            if (event.key == IptvInputKey::Up && focus_slot_)
                --focus_slot_;
            else if (event.key == IptvInputKey::Down && focus_slot_ < 1)
                ++focus_slot_;
            else if (event.key == IptvInputKey::Right)
                focus_target_ = FocusTarget::SourceRefresh;
            else if (event.key == IptvInputKey::Triangle && focus_slot_ == 1)
            {
                OpenCustomSourceEditor();
            }
            else if (event.key == IptvInputKey::Cross && focus_slot_ < 2)
            {
                if (refresh_thread_)
                {
                    SetStatusState("Refresh in progress", true, false);
                    SetText(document_, "source-status-label", "Source change disabled");
                    SetText(
                        document_, "source-status-detail",
                        "Wait for the active playlist refresh to finish before changing sources.");
                }
                else if (focus_slot_ == 0)
                {
                    SelectSource(SourceSelection::BuiltIn);
                }
                else if (!custom_source_url_.empty())
                {
                    SelectSource(SourceSelection::Custom);
                }
                else
                {
                    OpenCustomSourceEditor();
                }
            }
        }
        RefreshFocus();
        return true;
    }

    return true;
}

void IptvApp::RefreshFocus()
{
    static constexpr const char *nav_ids[] = {"nav-live-tv", "nav-favorites", "nav-sources"};
    for (unsigned index = 0; index < MenuCount; ++index)
    {
        const bool selected = index == static_cast<unsigned>(screen_);
        SetClass(document_, nav_ids[index], "active", selected);
        SetClass(document_, nav_ids[index], "selected", selected);
        SetClass(document_, nav_ids[index], "focused", false);
    }

    for (unsigned index = 0; index < kChannelCardCount; ++index)
    {
        char id[32];
        std::snprintf(id, sizeof(id), "channel-slot-%u", index);
        const unsigned filtered_index = page_offset_ + index;
        const bool visible = filtered_index < filtered_count_;
        SetClass(document_, id, "selected",
                 visible && CatalogIndexAt(filtered_index) == selected_slot_);
        SetClass(document_, id, "focused",
                 screen_ != Screen::Sources && focus_target_ == FocusTarget::Channel &&
                     index == focus_slot_ && visible);
    }
    SetClass(document_, "channel-play-button", "focused",
             screen_ != Screen::Sources && focus_target_ == FocusTarget::Play);
    SetVisible(document_, "channel-play-button", filtered_count_ != 0);
    SetClass(document_, "error-action", "focused",
             screen_ != Screen::Sources && focus_target_ == FocusTarget::Error);

    for (unsigned index = 0; index < GroupCount; ++index)
    {
        char id[32];
        std::snprintf(id, sizeof(id), "group-slot-%u", index);
        SetClass(document_, id, "focused",
                 screen_ == Screen::LiveTv && focus_target_ == FocusTarget::Group &&
                     index == focus_slot_);
        SetClass(document_, id, "selected", index == selected_group_);
    }

    for (unsigned index = 0; index < 2; ++index)
    {
        char id[40];
        std::snprintf(id, sizeof(id), "source-management-slot-%u", index);
        SetClass(document_, id, "focused",
                 screen_ == Screen::Sources && focus_target_ == FocusTarget::Source &&
                     index == focus_slot_);
    }
    SetClass(document_, "source-refresh-button", "focused",
             screen_ == Screen::Sources && focus_target_ == FocusTarget::SourceRefresh);
    static constexpr const char *search_ids[SearchControlCount] = {
        "search-query-button", "filter-country", "filter-category", "filter-language",
        "filter-quality",      "search-reset",   "search-apply"};
    for (unsigned index = 0; index < SearchControlCount; ++index)
        SetClass(document_, search_ids[index], "focused", search_open_ && search_focus_ == index);
}

void IptvApp::SetStatusColour(const char *colour)
{
    SetProperty(document_, "header-status-dot", "background-color", colour);
    SetProperty(document_, "footer-status-dot", "background-color", colour);
    SetProperty(document_, "live-status-dot", "background-color", colour);
    SetProperty(document_, "preview-status-dot", "background-color", colour);
}

void IptvApp::SetStatusState(const char *label, bool warning, bool error)
{
    static constexpr const char *labels[] = {"header-status-label", "footer-status-label",
                                             "live-status-label"};
    static constexpr const char *dots[] = {"header-status-dot", "footer-status-dot",
                                           "live-status-dot", "preview-status-dot"};
    for (const char *id : labels)
    {
        SetText(document_, id, label);
        SetClass(document_, id, "warning", warning);
        SetClass(document_, id, "error", error);
    }
    for (const char *id : dots)
    {
        SetClass(document_, id, "ready", !warning && !error);
        SetClass(document_, id, "warning", warning);
        SetClass(document_, id, "error", error);
    }
    SetStatusColour(error ? "#f16f72" : warning ? "#f0bd61" : "#64e6a6");
}

void IptvApp::ShowCatalogError(bool visible, const char *title, const char *message)
{
    if (!error_retries_playback_)
        SetText(document_, "error-action-label", "Retry");
    SetVisible(document_, "error-region", visible);
    SetVisible(document_, "live-status-region", !visible);
    if (visible)
    {
        SetText(document_, "error-title", title);
        SetText(document_, "error-message", message);
        SetText(document_, "preview-status-title", title);
        SetText(document_, "preview-status-message", message);
    }
}

void IptvApp::RefreshSourceUi()
{
    static constexpr const char *health_names[] = {"EMPTY", "SAVED", "CACHED", "REFRESHING",
                                                   "READY", "STALE", "ERROR"};
    SetText(document_, "source-name-0", "iptv-org public catalog");
    SetText(document_, "source-meta-0", "Public M3U");
    SetText(document_, "source-management-name-0", "iptv-org public catalog");
    SetText(document_, "source-management-url-0", kCatalogUrl);

    SetText(document_, "source-name-1", "Custom playlist");
    SetText(document_, "source-meta-1", custom_source_url_.empty() ? "Add URL" : "Custom M3U");
    SetText(document_, "source-management-name-1",
            custom_source_url_.empty() ? "Add a custom playlist" : "Custom playlist");
    SetText(document_, "source-management-url-1",
            custom_source_url_.empty() ? "Cross to enter an HTTP(S) M3U/M3U8 URL"
                                       : custom_source_url_);

    for (unsigned index = 0; index < 2; ++index)
    {
        char id[48];
        const char *health = health_names[static_cast<unsigned>(source_health_[index])];
        std::snprintf(id, sizeof(id), "source-status-%u", index);
        SetText(document_, id, health);
        std::snprintf(id, sizeof(id), "source-management-health-%u", index);
        SetText(document_, id, health);
        std::snprintf(id, sizeof(id), "source-slot-%u", index);
        SetVisible(document_, id, index == static_cast<unsigned>(active_source_));
        SetClass(document_, id, "selected", index == static_cast<unsigned>(active_source_));
        std::snprintf(id, sizeof(id), "source-management-slot-%u", index);
        SetClass(document_, id, "selected", index == static_cast<unsigned>(active_source_));
    }
    SetVisible(document_, "source-slot-2", false);
    SetVisible(document_, "source-management-slot-2", false);
}

void IptvApp::LoadActiveSourceCache()
{
    const bool custom = active_source_ == SourceSelection::Custom;
    const char *path = custom ? kCustomCatalogCachePath : kCatalogCachePath;
    const std::uint64_t source_id =
        custom ? iptv::CustomSourceId(custom_source_url_) : kCatalogSourceId;
    iptv::CatalogState cached;
    const iptv::StoreStatus status = iptv::LoadCatalog(path, &cached);
    catalog_loaded_ = status == iptv::StoreStatus::ok && cached.source_id == source_id &&
                      !cached.channels.empty();
    catalog_ = catalog_loaded_ ? std::move(cached) : iptv::CatalogState{};
    if (catalog_loaded_)
        (void)iptv::LoadPlaybackResults(iptv::kDefaultPlaybackHistoryPath, source_id, &catalog_);
    source_health_[static_cast<unsigned>(active_source_)] = catalog_loaded_ ? SourceHealth::Cached
                                                            : custom        ? SourceHealth::Saved
                                                                            : SourceHealth::Empty;
    page_offset_ = focus_slot_ = selected_slot_ = 0;
    RebuildFacets();
    RebuildFilteredChannels();
    RefreshCatalogUi();
    RefreshSourceUi();
}

void IptvApp::SelectSource(SourceSelection source)
{
    if (refresh_thread_)
        return;
    if (source == SourceSelection::Custom && custom_source_url_.empty())
    {
        OpenCustomSourceEditor();
        return;
    }
    if (source != active_source_)
    {
        active_source_ = source;
        LoadActiveSourceCache();
    }
    if (iptv::SaveActiveSource(source == SourceSelection::Custom) != iptv::SourceStateStatus::ok)
    {
        SetStatusState("Source selected; preference not saved", true, false);
    }
    SetText(document_, "source-status-label",
            source == SourceSelection::Custom ? "Custom playlist selected" : "iptv-org selected");
    SetText(document_, "source-status-detail",
            catalog_loaded_ ? "Showing the saved cache while the selected playlist refreshes."
                            : "No cache is available; downloading the selected playlist now.");
    RefreshSourceUi();
    RequestRefresh();
}

void IptvApp::OpenCustomSourceEditor()
{
    if (refresh_thread_)
        return;
    if (!ime_ready_)
    {
        SetStatusState("URL entry unavailable", true, false);
        SetText(document_, "source-status-label", "Native keyboard unavailable");
        SetText(document_, "source-status-detail",
                "The IME module could not be initialized for this launcher session.");
        return;
    }
    iptv_ime_request_prompt(custom_source_url_.c_str(), "Custom playlist URL",
                            "http(s)://host/playlist.m3u", IPTV_IME_BUFFER_CHARACTERS,
                            &IptvApp::CustomSourceResult, this);
    SetText(document_, "source-status-label", "Enter custom playlist URL");
    SetText(document_, "source-status-detail",
            "The URL must use HTTP or HTTPS and fit the bounded local source record.");
}

void IptvApp::ApplyCustomSourceUrl(const char *url)
{
    if (refresh_thread_)
    {
        SetStatusState("Refresh in progress", true, false);
        SetText(document_, "source-status-label", "Custom source not changed");
        SetText(document_, "source-status-detail",
                "Wait for the active playlist refresh to finish and try again.");
        return;
    }
    if (!iptv::http::IsSupportedPlaylistUrl(url))
    {
        SetStatusState("Invalid playlist URL", false, true);
        SetText(document_, "source-status-label", "Custom URL rejected");
        SetText(document_, "source-status-detail",
                "Enter an HTTP(S) URL without spaces, credentials, or control characters.");
        return;
    }
    const bool changed = custom_source_url_ != url;
    const iptv::SourceStateStatus saved = iptv::SaveCustomSourceUrl(url);
    if (saved != iptv::SourceStateStatus::ok)
    {
        SetStatusState("Unable to save source", false, true);
        SetText(document_, "source-status-label", "Custom URL was not saved");
        SetText(document_, "source-status-detail",
                saved == iptv::SourceStateStatus::too_large
                    ? "The URL exceeds the 1020-byte custom source limit."
                    : "The atomic /download0 source record could not be written.");
        return;
    }
    custom_source_url_ = url;
    source_health_[1] = SourceHealth::Saved;
    if (changed && active_source_ == SourceSelection::Custom)
        LoadActiveSourceCache();
    RefreshSourceUi();
    SelectSource(SourceSelection::Custom);
}

void IptvApp::RequestRefresh()
{
    if (!document_)
        return;
    if (refresh_thread_)
    {
        refresh_queued_ = true;
        SetText(document_, "source-status-detail",
                "One more refresh is queued after the active request finishes.");
        return;
    }
    refresh_queued_ = false;

    const bool custom = active_source_ == SourceSelection::Custom;
    if (custom && custom_source_url_.empty())
        return;
    refresh_source_ = active_source_;
    refresh_url_ = custom ? custom_source_url_ : kCatalogUrl;
    refresh_cache_path_ = custom ? kCustomCatalogCachePath : kCatalogCachePath;
    refresh_source_id_ = custom ? iptv::CustomSourceId(custom_source_url_) : kCatalogSourceId;
    source_health_[static_cast<unsigned>(active_source_)] = SourceHealth::Refreshing;

    refresh_complete_.store(false, std::memory_order_relaxed);
    shutdown_requested_.store(false, std::memory_order_relaxed);
    pending_cache_saved_ = false;
    SetStatusState("Refreshing", true, false);
    SetText(document_, "source-status-label",
            custom ? "Refreshing custom playlist" : "Refreshing iptv-org catalog");
    SetText(document_, "source-status-detail",
            catalog_loaded_
                ? "Cached channels remain available while the bounded M3U request runs off the UI "
                  "thread."
                : "Downloading and parsing the bounded M3U response off the UI thread.");
    RefreshSourceUi();
    if (!error_retries_playback_)
        ShowCatalogError(false, "", "");

    pthread_attr_t attributes;
    const int attr_result = pthread_attr_init(&attributes);
    const int stack_result = attr_result == 0
                                 ? pthread_attr_setstacksize(&attributes, kCatalogThreadStackBytes)
                                 : attr_result;
    void *thread = nullptr;
    const int create_result =
        stack_result == 0 ? scePthreadCreate(&thread, &attributes, &IptvApp::RefreshThreadEntry,
                                             this, "iptv-catalog")
                          : stack_result;
    if (attr_result == 0)
        pthread_attr_destroy(&attributes);
    if (create_result != 0)
    {
        source_health_[static_cast<unsigned>(active_source_)] = SourceHealth::Error;
        SetStatusState("Refresh unavailable", false, true);
        SetText(document_, "source-status-label", "Unable to start refresh");
        SetText(document_, "source-status-detail",
                "The refresh worker could not be created. Try again from Options.");
        RefreshSourceUi();
        if (!catalog_loaded_)
        {
            focus_target_ = FocusTarget::Error;
            ShowCatalogError(true, "Unable to load catalog",
                             "The refresh worker could not be created. Press Cross to retry.");
        }
        RefreshFocus();
        return;
    }
    refresh_thread_ = thread;
}

void *IptvApp::RefreshThreadEntry(void *argument)
{
    auto *app = static_cast<IptvApp *>(argument);
    app->pending_fetch_ = {};
    app->pending_catalog_ = {};
    app->pending_report_ = {};
    app->pending_cache_saved_ = false;
    app->pending_network_status_ = iptv::http::NetworkInit();

    if (app->pending_network_status_ == iptv::http::Status::ok &&
        !app->shutdown_requested_.load(std::memory_order_acquire))
    {
        std::vector<char> playlist(iptv::http::kDefaultMaxPlaylistBytes + 1u);
        const iptv::http::RequestControl control{
            [](void *context)
            {
                const auto *current = static_cast<const IptvApp *>(context);
                return current->shutdown_requested_.load(std::memory_order_acquire);
            },
            app};
        app->pending_fetch_ =
            iptv::http::GetM3u(app->refresh_url_.c_str(), playlist.data(), playlist.size(),
                               iptv::http::kDefaultMaxPlaylistBytes, nullptr, &control);
        if (app->pending_fetch_.status == iptv::http::Status::ok &&
            !app->shutdown_requested_.load(std::memory_order_acquire))
        {
            const std::string_view input(playlist.data(), app->pending_fetch_.bytes);
            app->pending_catalog_ =
                iptv::ParseExtendedM3u(input, app->refresh_source_id_, {}, &app->pending_report_);
            if (!app->pending_catalog_.channels.empty() &&
                !app->shutdown_requested_.load(std::memory_order_acquire))
            {
                app->pending_cache_saved_ =
                    iptv::SaveCatalog(app->refresh_cache_path_, app->pending_catalog_) ==
                    iptv::StoreStatus::ok;
            }
        }
    }
    if (app->pending_network_status_ == iptv::http::Status::ok)
        iptv::http::NetworkShutdown();

    app->refresh_complete_.store(true, std::memory_order_release);
    return nullptr;
}

void IptvApp::ConsumeRefresh()
{
    if (!refresh_thread_ || !refresh_complete_.load(std::memory_order_acquire))
        return;

    if (!JoinRefreshThread())
        return;
    refresh_complete_.store(false, std::memory_order_relaxed);

    const bool success = pending_network_status_ == iptv::http::Status::ok &&
                         pending_fetch_.status == iptv::http::Status::ok &&
                         !pending_catalog_.channels.empty();
    const bool custom = refresh_source_ == SourceSelection::Custom;
    const unsigned source_index = static_cast<unsigned>(refresh_source_);
    if (success)
    {
        catalog_ = std::move(pending_catalog_);
        catalog_loaded_ = true;
        (void)iptv::LoadPlaybackResults(iptv::kDefaultPlaybackHistoryPath, catalog_.source_id,
                                        &catalog_);
        RebuildFacets();
        RebuildFilteredChannels();
        if (error_retries_playback_ && !FindChannelById(playback_retry_channel_id_))
        {
            DismissPlaybackError();
            focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Group;
            focus_slot_ = filtered_count_ ? 0 : selected_group_;
        }
        RefreshCatalogUi();
        if (!error_retries_playback_)
            ShowCatalogError(false, "", "");
        source_health_[source_index] =
            pending_cache_saved_ ? SourceHealth::Ready : SourceHealth::Stale;
        if (!error_retries_playback_)
            SetStatusState(pending_cache_saved_ ? "Catalog ready" : "Catalog not cached",
                           !pending_cache_saved_, false);
        SetText(document_, "source-status-label",
                custom ? (pending_cache_saved_ ? "Custom playlist ready"
                                               : "Custom playlist loaded in memory")
                       : (pending_cache_saved_ ? "Public catalog ready"
                                               : "Public catalog loaded in memory"));
        char detail[160];
        std::snprintf(
            detail, sizeof(detail), "%u channels accepted; %u malformed records skipped.%s",
            static_cast<unsigned>(catalog_.channels.size()),
            static_cast<unsigned>(pending_report_.skipped),
            pending_cache_saved_ ? "" : " Cache write failed; this catalog lasts until exit.");
        SetText(document_, "source-status-detail", detail);
        if (!error_retries_playback_)
        {
            SetText(document_, "preview-status-title",
                    pending_cache_saved_ ? "Catalog ready" : "Catalog loaded");
            SetText(document_, "preview-status-message", detail);
        }
        if (!error_retries_playback_ && screen_ != Screen::Sources &&
            focus_target_ == FocusTarget::Error)
        {
            focus_target_ = filtered_count_                ? FocusTarget::Channel
                            : screen_ == Screen::Favorites ? FocusTarget::Play
                                                           : FocusTarget::Group;
            focus_slot_ = filtered_count_ ? 0 : selected_group_;
        }
    }
    else if (catalog_loaded_)
    {
        char detail[240];
        if (pending_network_status_ != iptv::http::Status::ok)
        {
            std::snprintf(detail, sizeof(detail), "%s. Showing %u cached channels.",
                          FetchStatusName(pending_network_status_),
                          static_cast<unsigned>(catalog_.channels.size()));
        }
        else if (pending_fetch_.status == iptv::http::Status::http_status_error)
        {
            std::snprintf(detail, sizeof(detail), "HTTP returned %d. Showing %u cached channels.",
                          pending_fetch_.http_status,
                          static_cast<unsigned>(catalog_.channels.size()));
        }
        else if (pending_fetch_.status == iptv::http::Status::ok)
        {
            std::snprintf(detail, sizeof(detail),
                          "Playlist contained no playable channels; %u records skipped. Showing %u "
                          "cached channels.",
                          static_cast<unsigned>(pending_report_.skipped),
                          static_cast<unsigned>(catalog_.channels.size()));
        }
        else
        {
            std::snprintf(detail, sizeof(detail), "%s (native %d). Showing %u cached channels.",
                          FetchStatusName(pending_fetch_.status), pending_fetch_.native_error,
                          static_cast<unsigned>(catalog_.channels.size()));
        }
        RefreshCatalogUi();
        source_health_[source_index] = SourceHealth::Stale;
        SetStatusState("Offline cache", true, false);
        SetText(document_, "source-status-label", "Refresh failed; offline cache active");
        SetText(document_, "source-status-detail", detail);
        SetText(document_, "preview-status-title", "Cached catalog");
        SetText(document_, "preview-status-message", detail);
    }
    else
    {
        char detail[240];
        if (pending_network_status_ != iptv::http::Status::ok)
        {
            std::snprintf(detail, sizeof(detail),
                          "%s. Connect the console and press Cross to retry.",
                          FetchStatusName(pending_network_status_));
        }
        else if (pending_fetch_.status == iptv::http::Status::http_status_error)
        {
            std::snprintf(detail, sizeof(detail), "HTTP returned %d. Press Cross to retry.",
                          pending_fetch_.http_status);
        }
        else if (pending_fetch_.status == iptv::http::Status::ok)
        {
            std::snprintf(detail, sizeof(detail),
                          "Playlist contained no playable channels; %u records skipped. Press "
                          "Cross to retry.",
                          static_cast<unsigned>(pending_report_.skipped));
        }
        else
        {
            std::snprintf(detail, sizeof(detail), "%s (native %d). Press Cross to retry.",
                          FetchStatusName(pending_fetch_.status), pending_fetch_.native_error);
        }
        source_health_[source_index] = SourceHealth::Error;
        SetStatusState("Catalog unavailable", false, true);
        SetText(document_, "source-status-label",
                custom ? "Unable to load custom playlist" : "Unable to load public catalog");
        SetText(document_, "source-status-detail", detail);
        ShowCatalogError(true, "Unable to load catalog", detail);
        focus_target_ = FocusTarget::Error;
    }
    pending_catalog_ = {};
    RefreshSourceUi();
    RefreshFocus();
    if (refresh_queued_)
    {
        refresh_queued_ = false;
        RequestRefresh();
    }
}

void IptvApp::RebuildFacets()
{
    using Entry = std::pair<std::string, unsigned>;
    std::vector<Entry> countries;
    std::vector<Entry> categories;
    std::vector<Entry> languages;
    auto add = [](std::vector<Entry> *entries, const std::string &field)
    {
        char value[48];
        FirstValue(field, value, sizeof(value));
        if (!*value)
            return;
        for (Entry &entry : *entries)
            if (EqualsCi(entry.first, value))
            {
                ++entry.second;
                return;
            }
        entries->emplace_back(value, 1u);
    };
    for (const iptv::Channel &channel : catalog_.channels)
    {
        add(&countries, channel.tvg_country);
        add(&categories, channel.group_title);
        add(&languages, channel.tvg_language);
        for (const std::string &category : channel.alternate_group_titles)
            add(&categories, category);
    }
    auto store = [](std::vector<Entry> *entries, auto *facets, unsigned *count)
    {
        std::sort(entries->begin(), entries->end(),
                  [](const Entry &left, const Entry &right)
                  {
                      if (left.second != right.second)
                          return left.second > right.second;
                      return left.first < right.first;
                  });
        *count = static_cast<unsigned>(std::min<std::size_t>(entries->size(), FacetMax));
        for (unsigned index = 0; index < *count; ++index)
        {
            facets[index].value = (*entries)[index].first;
            facets[index].count = (*entries)[index].second;
        }
    };
    store(&countries, country_facets_.data(), &country_facet_count_);
    store(&categories, category_facets_.data(), &category_facet_count_);
    store(&languages, language_facets_.data(), &language_facet_count_);

    auto retained = [](const auto &facets, unsigned count, const char *selected)
    {
        if (!*selected)
            return true;
        for (unsigned index = 0; index < count; ++index)
            if (EqualsCi(facets[index].value, selected))
                return true;
        return false;
    };
    if (!retained(country_facets_, country_facet_count_, filter_country_))
        filter_country_[0] = '\0';
    if (!retained(category_facets_, category_facet_count_, filter_category_))
        filter_category_[0] = '\0';
    if (!retained(language_facets_, language_facet_count_, filter_language_))
        filter_language_[0] = '\0';
}

void IptvApp::RebuildFilteredChannels()
{
    filtered_count_ = 0;
    const unsigned count = static_cast<unsigned>(catalog_.channels.size());
    for (unsigned index = 0; index < count && filtered_count_ < filtered_indices_.size(); ++index)
    {
        if (MatchesGroup(catalog_.channels[index], selected_group_, user_state_) &&
            MatchesFilters(catalog_.channels[index], filter_country_, filter_category_,
                           filter_language_, filter_quality_) &&
            MatchesQuery(catalog_.channels[index], search_query_))
            filtered_indices_[filtered_count_++] = index;
    }
    RefreshGroupUi();

    if (!filtered_count_)
    {
        page_offset_ = focus_slot_ = selected_slot_ = 0;
        return;
    }
    if (page_offset_ >= filtered_count_)
        page_offset_ = ((filtered_count_ - 1U) / kChannelCardCount) * kChannelCardCount;
    if (focus_slot_ >= filtered_count_ - page_offset_)
        focus_slot_ = 0;

    bool selected_visible = false;
    for (unsigned index = 0; index < filtered_count_; ++index)
    {
        if (filtered_indices_[index] == selected_slot_)
        {
            selected_visible = true;
            break;
        }
    }
    if (!selected_visible)
        selected_slot_ = filtered_indices_[page_offset_];
}

const iptv::Channel *IptvApp::FindChannelById(const std::string &channel_id) const
{
    for (const iptv::Channel &channel : catalog_.channels)
        if (channel.id == channel_id)
            return &channel;
    return nullptr;
}

void IptvApp::QueuePlay(const iptv::Channel &channel)
{
    play_request_.channel_id = channel.id;
    play_request_.channel_name = channel.name;
    play_request_.urls.clear();
    if (!channel.url.empty())
        play_request_.urls.push_back(channel.url);
    for (const std::string &alternate : channel.alternate_urls)
        if (!alternate.empty())
            play_request_.urls.push_back(alternate);
    play_request_.user_agent = channel.http_user_agent;
    play_request_.referrer = channel.http_referrer;
    play_request_.source_id = channel.source_id;
    play_requested_ = !play_request_.urls.empty();
    if (!play_requested_)
        return;
    const std::vector<std::string> previous = user_state_.recent_channel_ids;
    (void)iptv::AddRecentChannel(&user_state_, channel.id);
    if (iptv::SaveUserState(user_state_) != iptv::UserStateStatus::ok)
        user_state_.recent_channel_ids = previous;
}

void IptvApp::RefreshGroupUi()
{
    unsigned counts[GroupCount] = {};
    for (const iptv::Channel &channel : catalog_.channels)
    {
        ++counts[0];
        for (unsigned group = 1; group < GroupCount; ++group)
            if (MatchesGroup(channel, group, user_state_))
                ++counts[group];
    }
    for (unsigned group = 0; group < GroupCount; ++group)
    {
        char id[32];
        char count[24];
        std::snprintf(id, sizeof(id), "group-count-%u", group);
        std::snprintf(count, sizeof(count), "%u", counts[group]);
        SetText(document_, id, count);
        std::snprintf(id, sizeof(id), "group-slot-%u", group);
        SetClass(document_, id, "selected", group == selected_group_);
    }
}

void IptvApp::ApplySearch(const char *query)
{
    SDL_strlcpy(search_query_, query ? query : "", sizeof(search_query_));
    page_offset_ = focus_slot_ = 0;
    RebuildFilteredChannels();
    if (screen_ != Screen::Sources)
    {
        focus_target_ = catalog_.channels.empty()      ? FocusTarget::Error
                        : filtered_count_              ? FocusTarget::Channel
                        : screen_ == Screen::Favorites ? FocusTarget::Play
                                                       : FocusTarget::Group;
        if (focus_target_ == FocusTarget::Group)
            focus_slot_ = selected_group_;
    }
    RefreshCatalogUi();
    if (search_query_[0])
    {
        char message[192];
        std::snprintf(message, sizeof(message), "%u matching channels. Circle clears the search.",
                      filtered_count_);
        SetStatusState("Search active", false, false);
        SetText(document_, "preview-status-title", search_query_);
        SetText(document_, "preview-status-message", message);
    }
    else
    {
        SetStatusState("Search cleared", false, false);
        SetText(document_, "preview-status-title", "All channels");
        SetText(document_, "preview-status-message",
                "The full local catalog is visible. Triangle opens search.");
    }
    RefreshFocus();
}

void IptvApp::OpenSearch()
{
    search_open_ = true;
    search_focus_ = 0;
    SetVisible(document_, "search-overlay", true);
    RefreshSearchUi();
    RefreshFocus();
}

void IptvApp::CloseSearch()
{
    search_open_ = false;
    SetVisible(document_, "search-overlay", false);
    RefreshCatalogUi();
    RefreshFocus();
}

void IptvApp::ResetSearch()
{
    search_query_[0] = filter_country_[0] = filter_category_[0] = filter_language_[0] = '\0';
    filter_quality_ = 0;
    selected_group_ = screen_ == Screen::Favorites ? 1 : 0;
    if (screen_ == Screen::LiveTv)
        last_live_group_ = 0;
    page_offset_ = focus_slot_ = selected_slot_ = 0;
    RebuildFilteredChannels();
    focus_target_ = filtered_count_                ? FocusTarget::Channel
                    : screen_ == Screen::Favorites ? FocusTarget::Play
                                                   : FocusTarget::Group;
    RefreshCatalogUi();
    SetStatusState("Filters cleared", false, false);
}

void IptvApp::CycleSearchFilter(unsigned filter, int direction)
{
    auto cycle =
        [direction](char *selected, std::size_t selected_bytes, const auto &facets, unsigned count)
    {
        int current = -1;
        for (unsigned index = 0; index < count; ++index)
            if (EqualsCi(facets[index].value, selected))
                current = static_cast<int>(index);
        int next = current + direction;
        if (next < -1)
            next = static_cast<int>(count) - 1;
        if (next >= static_cast<int>(count))
            next = -1;
        SDL_strlcpy(selected, next >= 0 ? facets[static_cast<unsigned>(next)].value.c_str() : "",
                    selected_bytes);
    };
    switch (filter)
    {
    case 0:
        cycle(filter_country_, sizeof(filter_country_), country_facets_, country_facet_count_);
        break;
    case 1:
        cycle(filter_category_, sizeof(filter_category_), category_facets_, category_facet_count_);
        break;
    case 2:
        cycle(filter_language_, sizeof(filter_language_), language_facets_, language_facet_count_);
        break;
    case 3:
        filter_quality_ =
            static_cast<unsigned>((static_cast<int>(filter_quality_) + direction + 5) % 5);
        break;
    default:
        return;
    }
    page_offset_ = focus_slot_ = selected_slot_ = 0;
    RebuildFilteredChannels();
    focus_target_ = filtered_count_                ? FocusTarget::Channel
                    : screen_ == Screen::Favorites ? FocusTarget::Play
                                                   : FocusTarget::Group;
    RefreshCatalogUi();
}

void IptvApp::RefreshSearchUi()
{
    SetText(document_, "search-query-value",
            search_query_[0] ? search_query_
                             : "Type a name, country, language, category or quality");
    SetText(document_, "filter-country-value",
            filter_country_[0] ? filter_country_ : "All countries");
    SetText(document_, "filter-category-value",
            filter_category_[0] ? filter_category_ : "All categories");
    SetText(document_, "filter-language-value",
            filter_language_[0] ? filter_language_ : "All languages");
    SetText(document_, "filter-quality-value", QualityName(filter_quality_));
    char result[96];
    std::snprintf(result, sizeof(result), "%u matching channel%s in the local cache",
                  filtered_count_, filtered_count_ == 1 ? "" : "s");
    SetText(document_, "search-result-count", result);
    SetText(document_, "search-help",
            "Cross changes a filter. Results update instantly; Circle keeps the current view.");
}

unsigned IptvApp::CatalogIndexAt(unsigned filtered_index) const
{
    return filtered_index < filtered_count_ ? filtered_indices_[filtered_index]
                                            : static_cast<unsigned>(catalog_.channels.size());
}

void IptvApp::SearchResult(const char *text, void *user_data)
{
    IptvApp *app = static_cast<IptvApp *>(user_data);
    if (app && text)
        app->ApplySearch(text);
}

void IptvApp::CustomSourceResult(const char *text, void *user_data)
{
    IptvApp *app = static_cast<IptvApp *>(user_data);
    if (app && text)
        app->ApplyCustomSourceUrl(text);
}

void IptvApp::RefreshCatalogUi()
{
    char text[160];
    const unsigned channel_count = static_cast<unsigned>(catalog_.channels.size());
    const unsigned page_count =
        filtered_count_ ? (filtered_count_ + kChannelCardCount - 1u) / kChannelCardCount : 0;
    const unsigned page = filtered_count_ ? page_offset_ / kChannelCardCount + 1u : 0;
    static constexpr const char *group_names[] = {"All channels", "Favorites", "Recent",
                                                  "News",         "Sports",    "Kids"};
    const bool filters_active =
        filter_country_[0] || filter_category_[0] || filter_language_[0] || filter_quality_;
    if (search_query_[0] || filters_active)
    {
        std::snprintf(text, sizeof(text),
                      "%s  /  %u of %u channels  /  Page %u of %u  /  Search filters active",
                      group_names[selected_group_], filtered_count_, channel_count, page,
                      page_count);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%s  /  %u of %u channels  /  Page %u of %u",
                      group_names[selected_group_], filtered_count_, channel_count, page,
                      page_count);
    }
    SetText(document_, "live-status-detail", text);
    SetText(document_, "search-query",
            search_query_[0] ? search_query_
            : filters_active ? "Filters active"
                             : "Search and filter channels");
    SetVisible(document_, "search-clear", search_query_[0] || filters_active);
    SetVisible(document_, "channel-empty", filtered_count_ == 0);

    if (filtered_count_ && selected_slot_ >= channel_count)
        selected_slot_ = filtered_indices_[0];
    if (!filtered_count_)
        selected_slot_ = page_offset_ = 0;
    if (page_offset_ >= filtered_count_ && filtered_count_)
        page_offset_ = ((filtered_count_ - 1u) / kChannelCardCount) * kChannelCardCount;

    for (unsigned index = 0; index < kChannelCardCount; ++index)
    {
        char id[48];
        std::snprintf(id, sizeof(id), "channel-slot-%u", index);
        const unsigned filtered_index = page_offset_ + index;
        const bool visible = filtered_index < filtered_count_;
        const unsigned channel_index = CatalogIndexAt(filtered_index);
        SetVisible(document_, id, visible);
        SetClass(document_, id, "selected", visible && channel_index == selected_slot_);
        if (!visible)
            continue;

        const iptv::Channel &channel = catalog_.channels[channel_index];
        const bool favorite = iptv::IsFavorite(user_state_, channel.id);
        const bool recent = iptv::IsRecentChannel(user_state_, channel.id);
        SetClass(document_, id, "is-favorite", favorite);
        SetClass(document_, id, "is-recent", recent);
        std::snprintf(id, sizeof(id), "channel-favorite-%u", index);
        SetClass(document_, id, "active", favorite);
        std::snprintf(id, sizeof(id), "channel-recent-%u", index);
        SetClass(document_, id, "active", recent);
        std::snprintf(id, sizeof(id), "channel-playback-%u", index);
        SetPlaybackBadge(document_, id, channel.playback_status);
        std::snprintf(id, sizeof(id), "channel-logo-%u", index);
        char monogram[3];
        BuildChannelMonogram(channel, monogram);
        SetText(document_, id, monogram);
        std::snprintf(id, sizeof(id), "channel-number-%u", index);
        std::snprintf(text, sizeof(text), "%u", channel_index + 1u);
        SetText(document_, id, text);
        std::snprintf(id, sizeof(id), "channel-name-%u", index);
        SetText(document_, id, channel.name.empty() ? "Unnamed channel" : channel.name);
        std::snprintf(id, sizeof(id), "channel-meta-%u", index);
        char context[160];
        BuildChannelContext(channel, context, sizeof(context));
        SetText(document_, id, context);
        char technical[64];
        BuildChannelTechnicalMeta(channel, technical, sizeof(technical));
        std::snprintf(id, sizeof(id), "channel-tech-%u", index);
        SetText(document_, id, technical);
    }

    if (!filtered_count_)
    {
        SetClass(document_, "selected-channel-favorite", "active", false);
        SetClass(document_, "selected-channel-recent", "active", false);
        SetPlaybackBadge(document_, "selected-channel-playback", iptv::PlaybackStatus::unknown);
        SetText(document_, "selected-channel-number", "--");
        SetText(document_, "selected-channel-name",
                (search_query_[0] || filters_active) ? "No matching channels"
                : selected_group_                    ? "No channels in this group"
                                                     : "No channels available");
        SetText(document_, "selected-channel-meta",
                (search_query_[0] || filters_active) ? "Circle clears the current search."
                : selected_group_                    ? "Choose another group above."
                                  : "Refresh the public catalog or use a local cache.");
        SetText(document_, "selected-channel-tech", "Codec, resolution and FPS unavailable");
    }
    else
    {
        const iptv::Channel &channel = catalog_.channels[selected_slot_];
        SetClass(document_, "selected-channel-favorite", "active",
                 iptv::IsFavorite(user_state_, channel.id));
        SetClass(document_, "selected-channel-recent", "active",
                 iptv::IsRecentChannel(user_state_, channel.id));
        SetPlaybackBadge(document_, "selected-channel-playback", channel.playback_status);
        char number[16];
        std::snprintf(number, sizeof(number), "%02u", selected_slot_ + 1u);
        SetText(document_, "selected-channel-number", number);
        SetText(document_, "selected-channel-name",
                channel.name.empty() ? "Unnamed channel" : channel.name);
        char context[160];
        BuildChannelContext(channel, context, sizeof(context));
        SetText(document_, "selected-channel-meta", context);
        char technical[64];
        BuildChannelTechnicalMeta(channel, technical, sizeof(technical));
        SetText(document_, "selected-channel-tech", technical);
    }

    RefreshSearchUi();
}
