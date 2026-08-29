/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_app.h"

#include "iptv_store.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>

#include <SDL2/SDL.h>

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

namespace
{

constexpr char kCatalogUrl[] = "https://iptv-org.github.io/iptv/index.m3u";
constexpr char kCatalogCachePath[] = "/download0/psiptv-catalog.sqlite3";
constexpr char kCustomCatalogCachePath[] = "/download0/psiptv-custom-catalog.sqlite3";
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
    return false;
}

bool MatchesGroup(const iptv::Channel &channel, unsigned group, const iptv::UserState &user_state)
{
    if (group == 0)
        return true;
    if (group == 1)
        return iptv::IsFavorite(user_state, channel.id);
    const char *term = group == 2 ? "news" : group == 3 ? "sport" : "kid";
    if (ContainsCi(channel.group_title, term))
        return true;
    for (const std::string &alternate : channel.alternate_group_titles)
        if (ContainsCi(alternate, term))
            return true;
    return false;
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

    document_ = document;
    screen_ = Screen::LiveTv;
    focus_target_ = FocusTarget::Channel;
    focus_slot_ = selected_slot_ = page_offset_ = 0;
    selected_group_ = 0;
    filtered_count_ = 0;
    search_query_[0] = '\0';
    active_source_ = SourceSelection::BuiltIn;
    source_health_.fill(SourceHealth::Empty);
    custom_source_url_.clear();
    refresh_url_.clear();
    refresh_cache_path_.clear();
    refresh_source_id_ = 0;
    catalog_ = {};
    user_state_ = {};
    catalog_loaded_ = false;
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
    source_health_[static_cast<unsigned>(active_source_)] = catalog_loaded_ ? SourceHealth::Cached
                                                            : custom_active ? SourceHealth::Saved
                                                                            : SourceHealth::Empty;
    RebuildFilteredChannels();
    ime_ready_ = iptv_ime_init();

    document_->Show();
    RefreshCatalogUi();
    SetScreen(Screen::LiveTv);
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
        SDL_Delay(10);
    refresh_thread_ = nullptr;
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

void IptvApp::SetScreen(Screen screen)
{
    screen_ = screen;
    focus_slot_ = 0;
    switch (screen_)
    {
    case Screen::Home:
        focus_target_ = FocusTarget::Home;
        focus_slot_ = 0;
        while (focus_slot_ < 7 && !HomeChannelAt(focus_slot_))
            ++focus_slot_;
        if (focus_slot_ == 7)
            focus_slot_ = 0;
        break;
    case Screen::LiveTv:
        focus_target_ = catalog_.channels.empty() ? FocusTarget::Error
                        : filtered_count_         ? FocusTarget::Channel
                                                  : FocusTarget::Group;
        break;
    case Screen::Sources:
        focus_target_ = FocusTarget::Source;
        break;
    case Screen::Settings:
        focus_target_ = FocusTarget::Setting;
        break;
    }

    static constexpr const char *screens[] = {"screen-home", "screen-live-tv", "screen-sources",
                                              "screen-settings"};
    for (unsigned index = 0; index < MenuCount; ++index)
        SetVisible(document_, screens[index], index == static_cast<unsigned>(screen_));
    RefreshFocus();
}

bool IptvApp::HandleInput(const IptvInputEvent &event)
{
    if (!event.pressed)
        return true;
    if (event.key == IptvInputKey::Circle)
    {
        if (search_query_[0])
        {
            iptv_ime_cancel();
            ApplySearch("");
        }
        else if (screen_ != Screen::LiveTv)
        {
            SetScreen(Screen::LiveTv);
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

    if (screen_ == Screen::LiveTv)
    {
        const unsigned total = filtered_count_;
        const unsigned available =
            page_offset_ < total ? (total - page_offset_ < kChannelCardCount ? total - page_offset_
                                                                             : kChannelCardCount)
                                 : 0;
        if (event.key == IptvInputKey::Triangle)
        {
            if (ime_ready_)
            {
                iptv_ime_request(search_query_, &IptvApp::SearchResult, this);
                SetText(document_, "preview-status-title", "Channel search");
                SetText(document_, "preview-status-message",
                        "Enter a channel name, TV ID, or group with the native keyboard.");
            }
            else
            {
                SetStatusState("Search unavailable", true, false);
                SetText(document_, "preview-status-title", "Native keyboard unavailable");
                SetText(document_, "preview-status-message",
                        "The IME module could not be initialized for this launcher session.");
            }
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
                RequestRefresh();
            }
            else if (focus_target_ == FocusTarget::Group)
            {
                selected_group_ = focus_slot_;
                page_offset_ = selected_slot_ = 0;
                RebuildFilteredChannels();
                focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Group;
                focus_slot_ = filtered_count_ ? 0 : selected_group_;
            }
            else if (focus_target_ == FocusTarget::LiveSource)
            {
                if (refresh_thread_)
                {
                    SetStatusState("Refresh in progress", true, false);
                }
                else if (focus_slot_ == 0)
                {
                    SelectSource(SourceSelection::BuiltIn);
                }
                else if (custom_source_url_.empty())
                {
                    OpenCustomSourceEditor();
                }
                else
                {
                    SelectSource(SourceSelection::Custom);
                }
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
            else if (event.key == IptvInputKey::Left && slot == 0 &&
                     page_offset_ >= kChannelCardCount)
            {
                page_offset_ -= kChannelCardCount;
                focus_slot_ = kChannelCardCount - 1;
            }
            else if (event.key == IptvInputKey::Right)
            {
                if (slot % 4 < 3 && slot + 1 < available && slot + 1 < kChannelCardCount)
                    focus_slot_ = slot + 1;
                else if (slot + 1 >= available && page_offset_ + available < total)
                {
                    page_offset_ += kChannelCardCount;
                    focus_slot_ = 0;
                }
                else
                    focus_target_ = FocusTarget::Play;
            }
            else if (event.key == IptvInputKey::Up && slot >= 4)
            {
                focus_slot_ = slot - 4;
            }
            else if (event.key == IptvInputKey::Up && slot < 4)
            {
                focus_target_ = FocusTarget::Group;
                focus_slot_ = selected_group_;
            }
            else if (event.key == IptvInputKey::Down)
            {
                if (slot < 4 && slot + 4 < available && slot + 4 < kChannelCardCount)
                    focus_slot_ = slot + 4;
                else
                    focus_target_ = FocusTarget::Play;
            }
        }
        else if (focus_target_ == FocusTarget::Play &&
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
            else if (event.key == IptvInputKey::Left)
            {
                focus_target_ = FocusTarget::LiveSource;
                focus_slot_ = static_cast<unsigned>(active_source_);
            }
            else if (event.key == IptvInputKey::Right && focus_slot_ < 4)
            {
                ++focus_slot_;
            }
            else if (event.key == IptvInputKey::Down)
            {
                focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Group;
                focus_slot_ = 0;
            }
        }
        else if (focus_target_ == FocusTarget::LiveSource)
        {
            if (event.key == IptvInputKey::Left && focus_slot_)
            {
                --focus_slot_;
            }
            else if (event.key == IptvInputKey::Right && focus_slot_ < 1)
            {
                ++focus_slot_;
            }
            else if (event.key == IptvInputKey::Right || event.key == IptvInputKey::Down)
            {
                focus_target_ = FocusTarget::Group;
                focus_slot_ = selected_group_;
            }
        }
        else if (focus_target_ == FocusTarget::Error &&
                 (event.key == IptvInputKey::Left || event.key == IptvInputKey::Up ||
                  event.key == IptvInputKey::Right || event.key == IptvInputKey::Down))
        {
            RequestRefresh();
        }
        if (focus_target_ == FocusTarget::Channel && focus_slot_ < available)
            selected_slot_ = CatalogIndexAt(page_offset_ + focus_slot_);
        RefreshCatalogUi();
        RefreshFocus();
        return true;
    }

    if (screen_ == Screen::Home)
    {
        if (event.key == IptvInputKey::Cross)
        {
            if (const iptv::Channel *channel = HomeChannelAt(focus_slot_))
                QueuePlay(*channel);
        }
        else
        {
            unsigned next = focus_slot_;
            if (event.key == IptvInputKey::Right)
            {
                next = focus_slot_ == 0
                           ? 1
                           : (focus_slot_ == 3 || focus_slot_ == 6 ? focus_slot_ : focus_slot_ + 1);
            }
            else if (event.key == IptvInputKey::Left)
            {
                next = focus_slot_ == 0
                           ? 0
                           : (focus_slot_ == 1 || focus_slot_ == 4 ? 0 : focus_slot_ - 1);
            }
            else if (event.key == IptvInputKey::Down && focus_slot_ >= 1 && focus_slot_ <= 3)
            {
                next = focus_slot_ + 3;
            }
            else if (event.key == IptvInputKey::Up && focus_slot_ >= 4)
            {
                next = focus_slot_ - 3;
            }
            if (HomeChannelAt(next))
                focus_slot_ = next;
        }
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

    if (screen_ == Screen::Settings)
    {
        if (event.key == IptvInputKey::Up && focus_slot_)
            --focus_slot_;
        else if (event.key == IptvInputKey::Down && focus_slot_ < 3)
            ++focus_slot_;
        else if (event.key == IptvInputKey::Cross)
        {
            SetStatusState("Settings are read-only", true, false);
            SetText(document_, "settings-status-detail",
                    "The displayed controller defaults are fixed for this native build.");
        }
        RefreshFocus();
    }
    return true;
}

void IptvApp::RefreshFocus()
{
    static constexpr const char *nav_ids[] = {"nav-home", "nav-live-tv", "nav-sources",
                                              "nav-settings"};
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
                 screen_ == Screen::LiveTv && focus_target_ == FocusTarget::Channel &&
                     index == focus_slot_ && visible);
    }
    SetClass(document_, "channel-play-button", "focused",
             screen_ == Screen::LiveTv && focus_target_ == FocusTarget::Play);
    SetClass(document_, "error-action", "focused",
             screen_ == Screen::LiveTv && focus_target_ == FocusTarget::Error);

    for (unsigned index = 0; index < 2; ++index)
    {
        char id[32];
        std::snprintf(id, sizeof(id), "source-slot-%u", index);
        SetClass(document_, id, "focused",
                 screen_ == Screen::LiveTv && focus_target_ == FocusTarget::LiveSource &&
                     index == focus_slot_);
    }
    for (unsigned index = 0; index < 5; ++index)
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

    static constexpr const char *setting_ids[] = {"setting-safe-area", "setting-start-screen",
                                                  "setting-repeat", "setting-cache"};
    for (unsigned index = 0; index < 4; ++index)
        SetClass(document_, setting_ids[index], "focused",
                 screen_ == Screen::Settings && focus_target_ == FocusTarget::Setting &&
                     index == focus_slot_);
    SetClass(document_, "home-continue-card", "focused",
             screen_ == Screen::Home && focus_target_ == FocusTarget::Home && focus_slot_ == 0);
    for (unsigned list = 0; list < 2; ++list)
    {
        for (unsigned slot = 0; slot < 3; ++slot)
        {
            char id[40];
            std::snprintf(id, sizeof(id), "home-%s-slot-%u", list == 0 ? "recent" : "favorite",
                          slot);
            SetClass(document_, id, "focused",
                     screen_ == Screen::Home && focus_target_ == FocusTarget::Home &&
                         focus_slot_ == 1 + list * 3 + slot);
        }
    }
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
    source_health_[static_cast<unsigned>(active_source_)] = catalog_loaded_ ? SourceHealth::Cached
                                                            : custom        ? SourceHealth::Saved
                                                                            : SourceHealth::Empty;
    page_offset_ = focus_slot_ = selected_slot_ = 0;
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
    scePthreadDetach(thread);
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

    refresh_thread_ = nullptr;
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
        page_offset_ = focus_slot_ = 0;
        RebuildFilteredChannels();
        RefreshCatalogUi();
        ShowCatalogError(false, "", "");
        source_health_[source_index] =
            pending_cache_saved_ ? SourceHealth::Ready : SourceHealth::Stale;
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
        SetText(document_, "preview-status-title",
                pending_cache_saved_ ? "Catalog ready" : "Catalog loaded");
        SetText(document_, "preview-status-message", detail);
        if (screen_ == Screen::LiveTv && focus_target_ == FocusTarget::Error)
        {
            focus_target_ = filtered_count_ ? FocusTarget::Channel : FocusTarget::Group;
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

void IptvApp::RebuildFilteredChannels()
{
    filtered_count_ = 0;
    const unsigned count = static_cast<unsigned>(catalog_.channels.size());
    for (unsigned index = 0; index < count && filtered_count_ < filtered_indices_.size(); ++index)
    {
        if (MatchesGroup(catalog_.channels[index], selected_group_, user_state_) &&
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

const iptv::Channel *IptvApp::HomeChannelAt(unsigned slot) const
{
    if (slot == 0)
        return user_state_.recent_channel_ids.empty()
                   ? nullptr
                   : FindChannelById(user_state_.recent_channel_ids.front());
    const std::vector<std::string> &ids =
        slot <= 3 ? user_state_.recent_channel_ids : user_state_.favorite_ids;
    const unsigned index = slot <= 3 ? slot - 1 : slot - 4;
    return index < ids.size() ? FindChannelById(ids[index]) : nullptr;
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
    unsigned counts[5] = {};
    for (const iptv::Channel &channel : catalog_.channels)
    {
        ++counts[0];
        for (unsigned group = 1; group < 5; ++group)
            if (MatchesGroup(channel, group, user_state_))
                ++counts[group];
    }
    for (unsigned group = 0; group < 5; ++group)
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
    if (screen_ == Screen::LiveTv)
    {
        focus_target_ = catalog_.channels.empty() ? FocusTarget::Error
                        : filtered_count_         ? FocusTarget::Channel
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
    static constexpr const char *group_names[] = {"All channels", "Favorites", "News", "Sports",
                                                  "Kids"};
    if (search_query_[0])
    {
        std::snprintf(text, sizeof(text), "1 source  /  %u of %u channels  /  %.72s",
                      filtered_count_, channel_count, search_query_);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%s  /  %u of %u channels", group_names[selected_group_],
                      filtered_count_, channel_count);
    }
    SetText(document_, "live-status-detail", text);
    SetText(document_, "search-query", search_query_[0] ? search_query_ : "Search channels");
    SetVisible(document_, "search-clear", search_query_[0] != '\0');
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
        SetText(document_, id,
                channel.group_title.empty() ? "Public catalog" : channel.group_title);
    }

    if (!filtered_count_)
    {
        SetClass(document_, "selected-channel-favorite", "active", false);
        SetClass(document_, "selected-channel-recent", "active", false);
        SetText(document_, "selected-channel-number", "--");
        SetText(document_, "selected-channel-name",
                search_query_[0]  ? "No matching channels"
                : selected_group_ ? "No channels in this group"
                                  : "No channels available");
        SetText(document_, "selected-channel-meta",
                search_query_[0]  ? "Circle clears the current search."
                : selected_group_ ? "Choose another group above."
                                  : "Refresh the public catalog or use a local cache.");
    }
    else
    {
        const iptv::Channel &channel = catalog_.channels[selected_slot_];
        SetClass(document_, "selected-channel-favorite", "active",
                 iptv::IsFavorite(user_state_, channel.id));
        SetClass(document_, "selected-channel-recent", "active",
                 iptv::IsRecentChannel(user_state_, channel.id));
        char number[16];
        std::snprintf(number, sizeof(number), "%02u", selected_slot_ + 1u);
        SetText(document_, "selected-channel-number", number);
        SetText(document_, "selected-channel-name",
                channel.name.empty() ? "Unnamed channel" : channel.name);
        SetText(document_, "selected-channel-meta",
                channel.group_title.empty() ? "Public catalog" : channel.group_title);
    }

    const iptv::Channel *continue_channel =
        user_state_.recent_channel_ids.empty()
            ? nullptr
            : FindChannelById(user_state_.recent_channel_ids.front());
    SetVisible(document_, "home-continue-card", continue_channel != nullptr);
    SetVisible(document_, "home-continue-empty", continue_channel == nullptr);
    SetClass(document_, "home-continue-marker", "active", continue_channel != nullptr);
    if (continue_channel)
    {
        unsigned number = 0;
        for (; number < channel_count; ++number)
            if (&catalog_.channels[number] == continue_channel)
                break;
        std::snprintf(text, sizeof(text), "%u", number + 1u);
        SetText(document_, "home-continue-number", text);
        SetText(document_, "home-continue-name", continue_channel->name);
        SetText(document_, "home-continue-meta",
                continue_channel->group_title.empty() ? "Public catalog"
                                                      : continue_channel->group_title);
    }

    const std::vector<std::string> *home_lists[] = {&user_state_.recent_channel_ids,
                                                    &user_state_.favorite_ids};
    const char *prefixes[] = {"home-recent", "home-favorite"};
    for (unsigned list = 0; list < 2; ++list)
    {
        for (unsigned slot = 0; slot < 3; ++slot)
        {
            const iptv::Channel *channel = slot < home_lists[list]->size()
                                               ? FindChannelById((*home_lists[list])[slot])
                                               : nullptr;
            char slot_id[48];
            std::snprintf(slot_id, sizeof(slot_id), "%s-slot-%u", prefixes[list], slot);
            SetVisible(document_, slot_id, channel != nullptr);
            std::snprintf(slot_id, sizeof(slot_id), "%s-name-%u", prefixes[list], slot);
            SetText(document_, slot_id, channel ? channel->name : "");
            std::snprintf(slot_id, sizeof(slot_id), "%s-meta-%u", prefixes[list], slot);
            SetText(document_, slot_id,
                    channel
                        ? (channel->group_title.empty() ? "Public catalog" : channel->group_title)
                        : "");
        }
    }
}
