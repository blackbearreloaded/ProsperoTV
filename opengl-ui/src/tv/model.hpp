// ProsperoTV - The app without its screens: sources, catalog, filters, favorites.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything the interface shows comes from here and everything it does goes
// through here. Nothing in this file draws, and nothing in it knows which
// renderer is in use, so it runs unchanged in tests and on a PC.

#pragma once

#include "iptv_catalog.h"
#include "iptv_http.h"
#include "iptv_source_state.h"
#include "iptv_user_state.h"
#include "iptv_xtream.h"
#include "tv/catalog_index.hpp"
#include "tv/channel_text.hpp"
#include "tv/library.hpp"
#include "tv/guide.hpp"
#include "tv/portal.hpp"
#include "tv/vod.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ptv
{

// What the player is asked to open: the channel's addresses, best first.
struct PlayRequest
{
    std::string channel_id;
    std::string channel_name;
    std::vector<std::string> urls;
    std::string user_agent;
    std::string referrer;
    std::uint64_t source_id = 0;
    bool reconnect_live = false;
    bool record_channel_result = true;
    PortalCredentials portal;
    std::string portal_command;
};

// The lists a catalog is browsed by.
enum class Group : std::uint8_t
{
    all,
    favorites,
    recent,
    news,
    sports,
    kids,
    count,
};
inline constexpr unsigned kGroupCount = static_cast<unsigned>(Group::count);

enum class Level : std::uint8_t
{
    ready,
    busy,
    warning,
    error,
};

enum class SourceHealth : std::uint8_t
{
    empty,      // not set up
    saved,      // set up, nothing downloaded yet
    cached,     // showing the copy saved on the console
    refreshing, // downloading now
    ready,      // downloaded and saved
    stale,      // the last download failed or could not be saved
    error,      // nothing to show
};

// Something that happened and is worth a line on screen once.
struct Notice
{
    Level level = Level::ready;
    std::string title;
    std::string body;
    float seconds = 0.0f; // how long it stays; 0: as long as notices usually do
};

// The last channel that would not open.
struct PlaybackFailure
{
    std::string channel_id;
    std::string channel_name;
    std::string reason;
    unsigned attempts = 0;
    bool can_retry = false; // the channel is still in the catalog
};

// What the interface keeps across a channel: the menu is closed while video
// plays and reopened where it was.
struct ViewState
{
    int tab = 0;
    Group live_group = Group::all;
    std::string focused_channel; // its id
    int focused_source = 0;
    int vod_kind = -1, vod_focus = 0;
    bool vod_all = false;
    std::string vod_category, vod_series, vod_series_name, vod_series_cover, vod_query;
};

class Model
{
  public:
    static constexpr unsigned kFacetMax = CatalogIndex::kFacetMax;
    static constexpr unsigned kSourceCount = 4;

    // data_dir is where the app keeps its files: the title's own storage or
    // /data/prosperotv/config on the console, any folder on a PC. cache_dir
    // holds the downloaded channel lists; empty means beside the rest.
    explicit Model(std::string data_dir, std::string cache_dir = {});
    Model(const Model &) = delete;
    Model &operator=(const Model &) = delete;

    // ---- one menu session ----
    // open() reads the sources, the lists and the saved catalog, and starts a
    // download when the saved copy is old. close() ends the keyboard and the
    // download. Both may be called again and again; the filters and the view
    // survive.
    bool open();
    void close();
    // Once per frame: keyboard answers, the steps of the account form, and
    // the result of a download.
    void poll();
    void set_hide_failed(bool hide);
    // Attempts startup playback once per application lifetime, after a catalog arrives.
    void resume_last(bool enabled);

    std::span<const Facet> provider_categories() const
    {
        return index_.provider_categories;
    }
    const std::string &provider_category() const
    {
        return provider_category_;
    }
    void set_provider_category(std::string_view category);
    bool category_hidden(std::string_view category) const;
    bool hide_category(std::string_view category, bool hidden);
    std::vector<std::string> folders() const
    {
        return library_.folders();
    }
    const std::string &folder() const
    {
        return folder_;
    }
    void set_folder(std::string_view folder);
    bool create_folder(std::string_view name);
    bool rename_folder(std::string_view name, std::string_view replacement);
    bool remove_folder(std::string_view name);
    bool in_folder(std::string_view folder, std::string_view channel) const;
    bool put_in_folder(std::string_view folder, unsigned index, bool included);

    const std::vector<SavedSource> &saved_sources() const
    {
        return sources_;
    }
    std::int64_t selected_source_id() const
    {
        return selected_source_id_;
    }
    const SavedSource *saved_source(std::int64_t id) const;
    void use_saved_source(std::int64_t id);
    void add_source(iptv::SourceKind kind);
    void edit_saved_source(std::int64_t id);
    bool remove_source(std::int64_t id);
    bool set_schedule(std::int64_t id, RefreshSchedule schedule);
    VodLibrary &vod()
    {
        return vod_;
    }
    const VodLibrary &vod() const
    {
        return vod_;
    }
    bool play_vod(unsigned index);
    bool ask_vod_query();

    // ---- the catalog ----
    bool has_catalog() const
    {
        return catalog_loaded_;
    }
    unsigned channel_count() const
    {
        return static_cast<unsigned>(catalog_.size());
    }
    // A channel as the catalog holds it: read it and let it go (the texts are
    // the catalog's own, and a download replaces the catalog).
    iptv::ChannelView channel(unsigned index) const
    {
        return catalog_[index];
    }
    // The channels the group, the search and the filters leave, in the order
    // of the alphabet (see sort_key).
    unsigned visible_count() const
    {
        return visible_count_;
    }
    // The catalog index of the channel at a position of that list.
    unsigned visible(unsigned position) const
    {
        return visible_[position];
    }
    // Where a channel is in that list, or -1.
    int position_of(std::string_view channel_id) const;
    std::optional<iptv::ChannelView> find(std::string_view channel_id) const;
    // The letter (0 for '#', 1 to 26) the channel at a position is filed under.
    int letter_at(unsigned position) const;
    // The position of the first channel of that list under a letter, or -1.
    int letter_start(int letter) const
    {
        return visible_count_ > 0 && letter >= 0 && letter < kLetterCount
                   ? letter_starts_[static_cast<unsigned>(letter)]
                   : -1;
    }
    // A channel's place in the whole catalog read in the order of the
    // alphabet, from 1: the number it is known by whatever narrows the list.
    unsigned number_of(unsigned catalog_index) const
    {
        return catalog_index < index_.ranks.size() ? index_.ranks[catalog_index] + 1u : 0u;
    }
    // The scripts the list's names and groups are written in beyond the
    // European ones: the faces for them are large, and loaded only when asked for.
    bool uses_east_asian() const
    {
        return index_.east_asian || vod_.east_asian();
    }
    bool uses_korean() const
    {
        return index_.korean || vod_.korean();
    }
    // Changes whenever the visible list may have changed.
    unsigned revision() const
    {
        return revision_;
    }

    // ---- groups ----
    Group group() const
    {
        return group_;
    }
    void set_group(Group group);
    unsigned group_size(Group group) const
    {
        return group_sizes_[static_cast<unsigned>(group)];
    }
    static const char *group_name(Group group);

    // ---- search and filters ----
    const std::string &query() const
    {
        return query_;
    }
    void set_query(std::string_view query);
    std::span<const Facet> countries() const
    {
        return {index_.countries.data(), index_.country_count};
    }
    std::span<const Facet> categories() const
    {
        return {index_.categories.data(), index_.category_count};
    }
    std::span<const Facet> languages() const
    {
        return {index_.languages.data(), index_.language_count};
    }
    const std::string &country() const
    {
        return country_;
    }
    const std::string &category() const
    {
        return category_;
    }
    const std::string &language() const
    {
        return language_;
    }
    unsigned quality() const
    {
        return quality_;
    }
    void set_country(std::string_view value);
    void set_category(std::string_view value);
    void set_language(std::string_view value);
    void set_quality(unsigned quality);
    // A query or a filter narrows the list.
    bool filtering() const;
    void clear_filters();
    // Opens the console keyboard for the search text.
    bool ask_query();

    // ---- one channel ----
    bool is_favorite(const iptv::ChannelView &channel) const;
    bool is_recent(const iptv::ChannelView &channel) const;
    enum class Starred : std::uint8_t
    {
        added,
        removed,
        failed, // the list could not be saved; nothing changed
    };
    Starred toggle_favorite(unsigned catalog_index);

    // ---- playback ----
    // Queues the channel for the player; the frame loop takes the request,
    // closes the menu and plays it.
    bool play(unsigned catalog_index);
    bool play_programme(unsigned catalog_index, const Programme &programme);
    const Guide &guide() const
    {
        return guide_;
    }
    bool guide_refreshing() const
    {
        return guide_thread_ != nullptr;
    }
    const std::string &guide_status() const
    {
        return guide_status_;
    }
    void refresh_guide();
    bool take_play_request(PlayRequest *request);
    // The focused live channel without changing history or playback health.
    std::optional<PlayRequest> preview_request(std::string_view channel_id) const;
    // Called when the menu reopens after a channel that would not play.
    void report_playback_failure(const char *channel_id, const char *channel_name, int result,
                                 unsigned attempts, const char *detail);
    const PlaybackFailure *failure() const
    {
        return has_failure_ ? &failure_ : nullptr;
    }
    void dismiss_failure();
    bool retry_failure();

    // ---- sources ----
    iptv::SourceKind active_source() const
    {
        return active_source_;
    }
    SourceHealth health(iptv::SourceKind source) const
    {
        return health_[static_cast<unsigned>(source)];
    }
    const std::string &custom_url() const
    {
        return custom_url_;
    }
    bool xtream_ready() const;
    const std::string &xtream_server() const
    {
        return xtream_.server_url;
    }
    bool is_set_up(iptv::SourceKind source) const;
    static const char *builtin_url();
    bool refreshing() const
    {
        return refresh_thread_ != nullptr;
    }
    // How many channels the download in progress has read so far.
    unsigned refresh_progress() const
    {
        return refresh_count_.load(std::memory_order_relaxed);
    }
    bool keyboard_ready() const
    {
        return keyboard_ready_;
    }
    // Makes a source the one in use. A source that is not set up opens its form.
    void use_source(iptv::SourceKind source);
    // Opens the form of a source (the address, or the three account fields).
    void edit_source(iptv::SourceKind source);
    // Downloads the source in use again.
    void refresh();
    // When the catalog on screen was saved (seconds since 1970; 0: unknown).
    std::uint64_t saved_unix() const
    {
        return saved_unix_;
    }

    // ---- what to say ----
    Level level() const
    {
        return level_;
    }
    // A few words for the corner of the screen.
    const std::string &status() const
    {
        return status_;
    }
    // The source in use: a headline and a sentence.
    const std::string &source_title() const
    {
        return source_title_;
    }
    const std::string &source_detail() const
    {
        return source_detail_;
    }
    // Nothing to browse and the download failed: why.
    bool catalog_failed() const
    {
        return catalog_failed_;
    }
    const std::string &catalog_error() const
    {
        return catalog_error_;
    }
    std::vector<Notice> take_notices();
    // Something the app itself has to say (a newer version exists).
    void announce(Level level, std::string title, std::string body, float seconds);

    ViewState view;

  private:
    enum class AccountStep : std::uint8_t
    {
        none,
        server,
        username,
        password,
        portal_address,
        portal_mac,
    };

    std::string path(const char *name) const;
    std::string cache_path(iptv::SourceKind source) const;
    std::uint64_t source_id(iptv::SourceKind source) const;
    void load_cache();
    void adopt_catalog();
    void mark_lists();
    void recount_groups();
    void rebuild_visible();
    void set_status(std::string label, Level level);
    void set_source_text(std::string title, std::string detail);
    void notify(Level level, std::string title, std::string body = {});

    void apply_custom_url(const char *url);
    void apply_account_server(const char *server);
    void apply_account_username(const char *username);
    void apply_account_password(const char *password);
    void continue_account_form();
    void consume_refresh();
    bool join_refresh();
    static void *refresh_entry(void *self);
    void run_refresh();
    void save_account_receipt() const;
    void load_library();
    bool save_source_form(std::string_view url, const iptv::XtreamCredentials *account);
    bool commit_source(SavedSource source);
    static void on_portal_address(const char *text, void *self);
    static void on_portal_mac(const char *text, void *self);
    void select_source_record(const SavedSource &source);
    void mark_visibility();
    bool refresh_needed() const;
    std::string guide_path() const;
    void stop_guide();
    void poll_guide();
    static void *guide_entry(void *self);
    void run_guide();

    static void on_query(const char *text, void *self);
    static void on_vod_query(const char *text, void *self);
    static void on_custom_url(const char *text, void *self);
    static void on_account_server(const char *text, void *self);
    static void on_account_username(const char *text, void *self);
    static void on_account_password(const char *text, void *self);

    std::string data_dir_;
    std::string cache_dir_;
    bool opened_once_ = false;
    bool keyboard_ready_ = false;
    Library library_;
    std::vector<SavedSource> sources_;
    std::int64_t selected_source_id_ = 1;
    std::int64_t editing_source_id_ = 2;
    RefreshSchedule schedule_ = RefreshSchedule::daily;
    std::uint64_t next_schedule_check_ = 0;
    std::unordered_set<std::string> hidden_categories_;
    std::unordered_set<std::string> folder_channels_;
    std::string provider_category_;
    std::string folder_;
    bool hide_failed_ = false;
    bool resume_attempted_ = false;
    Guide guide_, pending_guide_;
    VodLibrary vod_;
    void *guide_thread_ = nullptr;
    std::atomic<bool> guide_done_{false}, guide_stop_{false};
    std::vector<std::string> guide_urls_;
    std::string guide_file_, guide_status_;
    bool guide_ok_ = false, guide_saved_ = false;
    std::uint64_t next_guide_check_ = 0, guide_minute_ = 0;

    // ---- catalog and lists ----
    iptv::Catalog catalog_;
    CatalogIndex index_; // of catalog_
    bool catalog_loaded_ = false;
    std::uint64_t saved_unix_ = 0;
    iptv::UserState user_;
    std::vector<std::uint8_t> marks_; // by catalog index: a favorite, a recent channel
    std::vector<std::uint32_t> visible_;
    unsigned visible_count_ = 0;
    std::array<int, kLetterCount> letter_starts_{};
    unsigned revision_ = 0;
    Group group_ = Group::all;
    std::array<unsigned, kGroupCount> group_sizes_{};

    // ---- filters ----
    std::string query_;
    std::string country_;
    std::string category_;
    std::string language_;
    unsigned quality_ = kQualityAny;

    // ---- sources ----
    iptv::SourceKind active_source_ = iptv::SourceKind::BuiltIn;
    std::array<SourceHealth, kSourceCount> health_{};
    std::string custom_url_;
    iptv::XtreamCredentials xtream_;
    iptv::XtreamCredentials account_form_;
    PortalCredentials portal_, portal_form_, refresh_portal_;
    AccountStep account_step_ = AccountStep::none;
    bool account_prompt_pending_ = false;

    // ---- the download in progress ----
    void *refresh_thread_ = nullptr;
    bool refresh_queued_ = false;
    std::atomic<bool> refresh_done_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<unsigned> refresh_count_{0};
    iptv::SourceKind refresh_source_ = iptv::SourceKind::BuiltIn;
    std::string refresh_url_;
    std::string refresh_cache_path_;
    std::uint64_t refresh_source_id_ = 0;
    iptv::XtreamCredentials refresh_account_;
    iptv::http::Status pending_network_ = iptv::http::Status::not_initialized;
    iptv::http::FetchResult pending_fetch_{};
    iptv::ParseReport pending_report_{};
    iptv::Catalog pending_catalog_{};
    CatalogIndex pending_index_; // of pending_catalog_
    iptv::XtreamStatus pending_account_ = iptv::XtreamStatus::ok;
    std::string pending_account_stage_;
    std::string pending_account_message_;
    bool pending_saved_ = false;

    // ---- playback ----
    bool play_requested_ = false;
    PlayRequest play_request_;
    bool has_failure_ = false;
    PlaybackFailure failure_;

    // ---- words ----
    Level level_ = Level::ready;
    std::string status_ = "Starting";
    std::string source_title_;
    std::string source_detail_;
    bool catalog_failed_ = false;
    std::string catalog_error_;
    std::vector<Notice> notices_;
};

} // namespace ptv
