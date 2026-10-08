// ProsperoTV - The app without its screens: sources, catalog, filters, favorites.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/model.hpp"
#include "tv/category_path.hpp"

#include "iptv_ime.h"
#include "iptv_store.h"
#include "tv/diag.hpp"
#include "tv/platform.hpp"
#include "tv/stream_sniff.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace ptv
{

namespace
{

constexpr char kCatalogUrl[] = "https://iptv-org.github.io/iptv/index.m3u";
constexpr std::uint64_t kBuiltInSourceId = UINT64_C(0x495054562d4f5247);
constexpr std::size_t kCatalogThreadStackBytes = 4u * 1024u * 1024u;
// An address that is not a playlist can send anything for as long as it likes:
// this much with not one channel in it, and the download is given up.
constexpr std::size_t kPlaylistProbeBytes = 1024u * 1024u;
// What a channel is to its user, in one byte beside the index's.
constexpr std::uint8_t kFavoriteMark = 0x01;
constexpr std::uint8_t kRecentMark = 0x02;

constexpr unsigned kCustom = static_cast<unsigned>(iptv::SourceKind::Custom);
constexpr unsigned kXtream = static_cast<unsigned>(iptv::SourceKind::Xtream);

const char *source_name(iptv::SourceKind source)
{
    switch (source)
    {
    case iptv::SourceKind::Custom:
        return "your playlist";
    case iptv::SourceKind::Xtream:
        return "your Xtream account";
    case iptv::SourceKind::Portal:
        return "your MAC-code portal";
    case iptv::SourceKind::BuiltIn:
        break;
    }
    return "the iptv-org catalog";
}

bool valid_credential(const char *value)
{
    if (value == nullptr || *value == '\0')
        return false;
    const std::size_t bytes = std::strlen(value);
    if (bytes > iptv::kMaxXtreamCredentialBytes)
        return false;
    for (std::size_t index = 0; index < bytes; ++index)
    {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x20u || byte == 0x7fu)
            return false;
    }
    return true;
}

// `quality` is the channel's picture size, as its index has it.
bool matches_query(const iptv::ChannelView &channel, std::string_view query, unsigned quality)
{
    if (query.empty())
        return true;
    if (contains_nocase(channel.name, query) ||
        (channel.tvg_name != channel.name && contains_nocase(channel.tvg_name, query)) ||
        contains_nocase(channel.tvg_id, query) || contains_nocase(channel.group_title, query) ||
        contains_nocase(channel.tvg_country, query) || contains_nocase(channel.tvg_language, query))
        return true;
    for (const std::string_view group : channel.alternate_group_titles)
        if (contains_nocase(group, query))
            return true;
    // "hd", "1080p" and the like find the channels of that size.
    switch (quality)
    {
    case kQualitySd:
        return contains_nocase("SD 480P 576P", query);
    case kQualityHd:
        return contains_nocase("HD 720P", query);
    case kQualityFullHd:
        return contains_nocase("FULL HD FHD 1080P", query);
    case kQualityUhd:
        return contains_nocase("4K UHD 2160P", query);
    default:
        return false;
    }
}

bool matches_filters(const iptv::ChannelView &channel, const std::string &country,
                     const std::string &category, const std::string &language)
{
    if (!country.empty() && !field_has_value(channel.tvg_country, country))
        return false;
    if (!language.empty() && !field_has_value(channel.tvg_language, language))
        return false;
    if (!category.empty())
    {
        bool found = field_has_value(channel.group_title, category);
        for (const std::string_view alternate : channel.alternate_group_titles)
            found = found || field_has_value(alternate, category);
        if (!found)
            return false;
    }
    return true;
}

// Why a download failed, in words a person can act on.
std::string fetch_problem(iptv::http::Status network, const iptv::http::FetchResult &fetch,
                          bool account, iptv::XtreamStatus account_status,
                          const std::string &account_message, std::size_t skipped)
{
    using iptv::http::Status;
    if (network != Status::ok)
        return "The console could not start its network connection.";
    if (account && account_status != iptv::XtreamStatus::ok)
    {
        std::string text = iptv::XtreamStatusDescription(account_status);
        if (!account_message.empty())
            text += ": " + account_message.substr(0, 96);
        return text + ".";
    }
    char text[160];
    switch (fetch.status)
    {
    case Status::ok:
        std::snprintf(text, sizeof(text),
                      "The playlist has no channels that can be played (%u entries skipped).",
                      static_cast<unsigned>(skipped));
        return text;
    case Status::http_status_error:
        std::snprintf(text, sizeof(text), "The server answered with error %d.", fetch.http_status);
        return text;
    case Status::deadline_exceeded:
        return "The server took too long to answer.";
    case Status::response_too_large:
        return "The channel list is larger than this app can load.";
    case Status::unsupported_url:
    case Status::invalid_argument:
        return "The address is not one this app can open.";
    case Status::redirect_error:
        return "The server sent the app in a circle.";
    case Status::cancelled:
        return "The download was stopped.";
    default:
        std::snprintf(text, sizeof(text), "The server could not be reached (code %d).",
                      fetch.native_error);
        return text;
    }
}

} // namespace

Model::Model(std::string data_dir, std::string cache_dir)
    : data_dir_(std::move(data_dir)), cache_dir_(std::move(cache_dir))
{
    if (cache_dir_.empty())
        cache_dir_ = data_dir_;
    health_.fill(SourceHealth::empty);
}

std::string Model::path(const char *name) const
{
    return data_dir_ + "/" + name;
}

std::string Model::cache_path(iptv::SourceKind source) const
{
    if (selected_source_id_ > 3)
        return cache_dir_ + "/prosperotv-source-" + std::to_string(selected_source_id_) +
               ".sqlite3";
    switch (source)
    {
    case iptv::SourceKind::Custom:
        return cache_dir_ + "/prosperotv-custom-catalog.sqlite3";
    case iptv::SourceKind::Xtream:
        return cache_dir_ + "/prosperotv-xtream-catalog.sqlite3";
    case iptv::SourceKind::Portal:
        return cache_dir_ + "/prosperotv-source-4.sqlite3";
    case iptv::SourceKind::BuiltIn:
        break;
    }
    return cache_dir_ + "/prosperotv-catalog.sqlite3";
}

std::uint64_t Model::source_id(iptv::SourceKind source) const
{
    switch (source)
    {
    case iptv::SourceKind::Custom:
        return iptv::CustomSourceId(custom_url_);
    case iptv::SourceKind::Xtream:
        return iptv::XtreamSourceId(xtream_);
    case iptv::SourceKind::Portal:
        return portal_source_id(portal_);
    case iptv::SourceKind::BuiltIn:
        break;
    }
    return kBuiltInSourceId;
}

const char *Model::builtin_url()
{
    return kCatalogUrl;
}

const char *Model::group_name(Group group)
{
    static constexpr const char *names[kGroupCount] = {"All",  "Favorites", "Recent",
                                                       "News", "Sports",    "Kids"};
    return names[static_cast<unsigned>(group)];
}

bool Model::xtream_ready() const
{
    return iptv::ValidateXtreamCredentials(xtream_);
}

bool Model::is_set_up(iptv::SourceKind source) const
{
    switch (source)
    {
    case iptv::SourceKind::Custom:
        return !custom_url_.empty();
    case iptv::SourceKind::Xtream:
        return xtream_ready();
    case iptv::SourceKind::Portal:
        return !portal_.url.empty() && !portal_.mac.empty();
    case iptv::SourceKind::BuiltIn:
        break;
    }
    return true;
}

void Model::set_status(std::string label, Level level)
{
    status_ = std::move(label);
    level_ = level;
}

void Model::set_source_text(std::string title, std::string detail)
{
    source_title_ = std::move(title);
    source_detail_ = std::move(detail);
}

void Model::notify(Level level, std::string title, std::string body)
{
    // The interface shows a few at a time; a burst keeps its newest.
    if (notices_.size() >= 8)
        notices_.erase(notices_.begin());
    notices_.push_back({level, std::move(title), std::move(body), 0.0f});
}

void Model::announce(Level level, std::string title, std::string body, float seconds)
{
    notify(level, std::move(title), std::move(body));
    notices_.back().seconds = seconds;
}

std::vector<Notice> Model::take_notices()
{
    return std::exchange(notices_, {});
}

// ---- one menu session ------------------------------------------------------

bool Model::open()
{
    const bool first = !opened_once_;
    opened_once_ = true;

    account_form_ = {};
    account_step_ = AccountStep::none;
    account_prompt_pending_ = false;
    refresh_queued_ = false;
    refresh_done_.store(false, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    pending_account_ = iptv::XtreamStatus::ok;
    pending_account_message_.clear();
    play_requested_ = false;
    play_request_ = {};
    catalog_failed_ = false;
    catalog_error_.clear();

    if (first)
    {
        health_.fill(SourceHealth::empty);
        std::string saved_url;
        if (iptv::LoadCustomSourceUrl(path("iptv-custom-source-v1.txt"), &saved_url) ==
            iptv::SourceStateStatus::ok)
        {
            custom_url_ = std::move(saved_url);
            health_[kCustom] = SourceHealth::saved;
        }
        if (iptv::LoadXtreamCredentials(path("prosperotv-xtream-v1.txt"), &xtream_) ==
            iptv::XtreamStatus::ok)
            health_[kXtream] = SourceHealth::saved;
        else
            xtream_ = {};

        active_source_ = iptv::SourceKind::BuiltIn;
        iptv::SourceKind saved = iptv::SourceKind::BuiltIn;
        if (iptv::LoadActiveSource(path("iptv-active-source-v1.txt"), &saved) ==
                iptv::SourceStateStatus::ok &&
            is_set_up(saved))
            active_source_ = saved;

        (void)iptv::LoadUserState(path("iptv-favorites-v1.bin"), path("iptv-history-v1.bin"),
                                  &user_);
        load_library();
        load_cache();
    }
    else if (catalog_loaded_)
    {
        // The catalog is still in memory: only what the last channel changed
        // is read again.
        (void)iptv::LoadPlaybackResults(path("prosperotv-playback-history.sqlite3"),
                                        catalog_.source_id, &catalog_);
        mark_visibility();
        mark_lists();
        recount_groups();
        rebuild_visible();
    }
    else
    {
        load_cache();
    }

    keyboard_ready_ = iptv_ime_init();

    const bool fresh = !refresh_needed();
    if (catalog_loaded_)
    {
        set_status("Saved copy", Level::ready);
        set_source_text("Showing the copy saved on this console",
                        fresh ? "It is up to date. Press Options to download it again."
                              : "A newer copy is being downloaded in the background.");
    }
    else
    {
        set_status("Loading", Level::busy);
        set_source_text("Downloading the channel list",
                        "Nothing is saved on this console yet. This happens once.");
    }
    if (!fresh)
        refresh();
    return true;
}

void Model::close()
{
    vod_.stop();
    stop_guide();
    stop_requested_.store(true, std::memory_order_release);
    if (keyboard_ready_)
    {
        iptv_ime_shutdown();
        keyboard_ready_ = false;
    }
    while (refresh_thread_ != nullptr && !refresh_done_.load(std::memory_order_acquire))
    {
        platform::network_cancel();
        platform::sleep_ms(10);
    }
    while (refresh_thread_ != nullptr && !join_refresh())
        platform::sleep_ms(10);
    if (health_[static_cast<unsigned>(active_source_)] == SourceHealth::refreshing)
        health_[static_cast<unsigned>(active_source_)] =
            catalog_loaded_ ? SourceHealth::cached : SourceHealth::saved;
    refresh_queued_ = false;
    refresh_done_.store(false, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    refresh_count_.store(0, std::memory_order_relaxed);
    pending_catalog_ = {};
    pending_index_.clear();
    account_step_ = AccountStep::none;
    account_prompt_pending_ = false;
}

void Model::poll()
{
    if (keyboard_ready_)
        iptv_ime_poll();
    continue_account_form();
    consume_refresh();
    if (vod_.requested() && !refreshing())
        stop_guide();
    vod_.poll(!refreshing() && !guide_thread_);
    if (!vod_.busy() && !vod_.requested())
        poll_guide();
    const auto now = platform::unix_time();
    if (catalog_loaded_ && !refreshing() && !vod_.busy() && now >= next_schedule_check_)
    {
        // A failed scheduled refresh retries in an hour, not on every frame.
        next_schedule_check_ = now + 3600;
        if (refresh_needed())
            refresh();
    }
}

// ---- the catalog ------------------------------------------------------------

void Model::load_cache()
{
    stop_guide();
    guide_ = {};
    next_guide_check_ = 0;
    const std::uint64_t wanted = source_id(active_source_);
    iptv::Catalog cached;
    iptv::StoreReport report;
    const iptv::StoreStatus status =
        iptv::LoadCatalog(cache_path(active_source_), &cached, {}, &report);
    catalog_loaded_ =
        status == iptv::StoreStatus::ok && cached.source_id == wanted && !cached.empty();
    catalog_ = catalog_loaded_ ? std::move(cached) : iptv::Catalog{};
    saved_unix_ = catalog_loaded_ ? report.saved_unix : 0;
    if (catalog_loaded_)
        (void)iptv::LoadPlaybackResults(path("prosperotv-playback-history.sqlite3"), wanted,
                                        &catalog_);
    const bool own = active_source_ != iptv::SourceKind::BuiltIn;
    health_[static_cast<unsigned>(active_source_)] = catalog_loaded_ ? SourceHealth::cached
                                                     : own           ? SourceHealth::saved
                                                                     : SourceHealth::empty;
    index_.build(catalog_);
    if (catalog_loaded_)
        (void)guide_.load(guide_path(), catalog_.source_id);
    adopt_catalog();
}

// The catalog and its index are new: everything counted from them follows.
void Model::adopt_catalog()
{
    hidden_categories_ = library_.hidden_categories(source_id(active_source_));
    folder_channels_ = library_.folder_channels(folder_);
    if (!provider_category_.empty() &&
        std::none_of(index_.provider_categories.begin(), index_.provider_categories.end(),
                     [this](const Facet &facet)
                     { return category_belongs(facet.value, provider_category_); }))
        provider_category_.clear();
    // A filter the new catalog has no word for cannot stay on.
    const auto kept = [](std::span<const Facet> facets, const std::string &selected)
    {
        if (selected.empty())
            return true;
        for (const Facet &facet : facets)
            if (equals_nocase(facet.value, selected))
                return true;
        return false;
    };
    if (!kept(countries(), country_))
        country_.clear();
    if (!kept(categories(), category_))
        category_.clear();
    if (!kept(languages(), language_))
        language_.clear();
    mark_lists();
    recount_groups();
    rebuild_visible();
}

// Which channels are favorites and which were watched: a few hundred ids at
// most, each found in the catalog at once, whatever its size.
void Model::mark_lists()
{
    marks_.assign(catalog_.size(), 0);
    const auto mark = [this](const std::vector<std::string> &ids, std::uint8_t bit)
    {
        for (const std::string &id : ids)
        {
            const std::size_t index = catalog_.Find(id);
            if (index != iptv::Catalog::npos)
                marks_[index] |= bit;
        }
    };
    mark(user_.favorite_ids, kFavoriteMark);
    mark(user_.recent_channel_ids, kRecentMark);
    mark_visibility();
}

void Model::recount_groups()
{
    group_sizes_.fill(0);
    const std::size_t count = std::min(index_.traits.size(), marks_.size());
    unsigned favorites = 0;
    unsigned recent = 0;
    unsigned news = 0;
    unsigned sports = 0;
    unsigned kids = 0;
    unsigned shown = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::uint8_t trait = index_.traits[index];
        const std::uint8_t mark = marks_[index];
        if ((mark & 4u) != 0)
            continue;
        ++shown;
        favorites += (mark & kFavoriteMark) != 0 ? 1u : 0u;
        recent += (mark & kRecentMark) != 0 ? 1u : 0u;
        news += (trait & CatalogIndex::kNews) != 0 ? 1u : 0u;
        sports += (trait & CatalogIndex::kSports) != 0 ? 1u : 0u;
        kids += (trait & CatalogIndex::kKids) != 0 ? 1u : 0u;
    }
    group_sizes_[static_cast<unsigned>(Group::all)] = shown;
    group_sizes_[static_cast<unsigned>(Group::favorites)] = favorites;
    group_sizes_[static_cast<unsigned>(Group::recent)] = recent;
    group_sizes_[static_cast<unsigned>(Group::news)] = news;
    group_sizes_[static_cast<unsigned>(Group::sports)] = sports;
    group_sizes_[static_cast<unsigned>(Group::kids)] = kids;
}

// The channels the group, the filters and the search leave, in the order of
// the alphabet. A group and a picture size are bytes of the index, so a list
// of any length is narrowed by them at once; only a search or a filter by
// word reads the channels themselves.
void Model::rebuild_visible()
{
    if (index_.size() != catalog_.size())
        index_.build(catalog_);
    if (marks_.size() != catalog_.size())
        mark_lists();
    std::uint8_t trait_wanted = 0;
    std::uint8_t mark_wanted = 0;
    switch (group_)
    {
    case Group::favorites:
        mark_wanted = kFavoriteMark;
        break;
    case Group::recent:
        mark_wanted = kRecentMark;
        break;
    case Group::news:
        trait_wanted = CatalogIndex::kNews;
        break;
    case Group::sports:
        trait_wanted = CatalogIndex::kSports;
        break;
    case Group::kids:
        trait_wanted = CatalogIndex::kKids;
        break;
    default:
        break;
    }
    const bool by_word =
        !query_.empty() || !country_.empty() || !category_.empty() || !language_.empty();
    const auto guide_time = static_cast<std::int64_t>(platform::unix_time());

    visible_.clear();
    letter_starts_.fill(-1);
    for (const std::uint32_t index : index_.order)
    {
        if ((marks_[index] & 4u) != 0)
            continue;
        if (!provider_category_.empty())
        {
            const auto channel = catalog_[index];
            bool matches = category_belongs(channel.group_title, provider_category_);
            for (const auto category : channel.alternate_group_titles)
                matches = matches || category_belongs(category, provider_category_);
            if (!matches)
                continue;
        }
        if (group_ == Group::favorites && !folder_.empty() &&
            !folder_channels_.contains(std::string(catalog_[index].id)))
            continue;
        const std::uint8_t trait = index_.traits[index];
        if ((trait & trait_wanted) != trait_wanted || (marks_[index] & mark_wanted) != mark_wanted)
            continue;
        const unsigned quality = trait & CatalogIndex::kQualityMask;
        if (quality_ != kQualityAny && quality != quality_)
            continue;
        if (by_word)
        {
            const iptv::ChannelView channel = catalog_[index];
            if (!matches_filters(channel, country_, category_, language_) ||
                (!matches_query(channel, query_, quality) &&
                 !guide_.matches_now(channel.id, query_, guide_time)))
                continue;
        }
        int &start = letter_starts_[index_.letters[index]];
        if (start < 0)
            start = static_cast<int>(visible_.size());
        visible_.push_back(index);
    }
    visible_count_ = static_cast<unsigned>(visible_.size());
    ++revision_;
}

int Model::letter_at(unsigned position) const
{
    return position < visible_count_ ? index_.letters[visible_[position]] : 0;
}

int Model::position_of(std::string_view channel_id) const
{
    const std::size_t index = catalog_.Find(channel_id);
    if (index == iptv::Catalog::npos || index >= index_.ranks.size())
        return -1;
    // The list is in the order of the alphabet, and so are the ranks: the
    // channel is where its rank falls.
    const std::uint32_t rank = index_.ranks[index];
    const auto first = visible_.begin();
    const auto last = first + visible_count_;
    const auto found =
        std::lower_bound(first, last, rank, [this](std::uint32_t entry, std::uint32_t wanted)
                         { return index_.ranks[entry] < wanted; });
    return found != last && *found == index ? static_cast<int>(found - first) : -1;
}

std::optional<iptv::ChannelView> Model::find(std::string_view channel_id) const
{
    const std::size_t index = catalog_.Find(channel_id);
    if (index == iptv::Catalog::npos)
    {
        const auto media = vod_.catalog().Find(channel_id);
        return media == iptv::Catalog::npos
                   ? std::nullopt
                   : std::optional<iptv::ChannelView>(vod_.catalog()[media]);
    }
    return catalog_[index];
}

// ---- groups, search and filters ----------------------------------------------

void Model::set_group(Group group)
{
    if (group == group_ || group >= Group::count)
        return;
    group_ = group;
    rebuild_visible();
    diag::event("list %d chosen: %u of %u channels shown", static_cast<int>(group), visible_count(),
                channel_count());
}

void Model::set_query(std::string_view query)
{
    const std::string next(query.substr(0, IPTV_IME_MAX_TEXT_BYTES - 1u));
    if (next == query_)
        return;
    query_ = next;
    rebuild_visible();
    diag::event("search \"%s\": %u of %u channels shown", query_.c_str(), visible_count(),
                channel_count());
}

void Model::set_country(std::string_view value)
{
    if (country_ == value)
        return;
    country_ = value;
    rebuild_visible();
}

void Model::set_category(std::string_view value)
{
    if (category_ == value)
        return;
    category_ = value;
    rebuild_visible();
}

void Model::set_language(std::string_view value)
{
    if (language_ == value)
        return;
    language_ = value;
    rebuild_visible();
}

void Model::set_quality(unsigned quality)
{
    if (quality >= kQualityCount || quality == quality_)
        return;
    quality_ = quality;
    rebuild_visible();
}

bool Model::filtering() const
{
    return !provider_category_.empty() || !query_.empty() || !country_.empty() ||
           !category_.empty() || !language_.empty() || quality_ != kQualityAny;
}

void Model::clear_filters()
{
    if (!filtering())
        return;
    iptv_ime_cancel();
    query_.clear();
    provider_category_.clear();
    country_.clear();
    category_.clear();
    language_.clear();
    quality_ = kQualityAny;
    rebuild_visible();
}

bool Model::ask_query()
{
    if (!keyboard_ready_)
    {
        notify(Level::warning, "The keyboard is not available",
               "Close ProsperoTV and open it again.");
        return false;
    }
    iptv_ime_request_prompt(query_.c_str(), "Search channels or programmes on now",
                            "Channel or programme title", IPTV_IME_BUFFER_CHARACTERS,
                            &Model::on_query, this);
    return true;
}

void Model::on_query(const char *text, void *self)
{
    if (self != nullptr && text != nullptr)
        static_cast<Model *>(self)->set_query(text);
}

// ---- one channel ----------------------------------------------------------------

bool Model::is_favorite(const iptv::ChannelView &channel) const
{
    const auto index = catalog_.Find(channel.id);
    if (index < marks_.size())
        return (marks_[index] & kFavoriteMark) != 0;
    return iptv::IsFavorite(user_, channel.id);
}

bool Model::is_recent(const iptv::ChannelView &channel) const
{
    const auto index = catalog_.Find(channel.id);
    if (index < marks_.size())
        return (marks_[index] & kRecentMark) != 0;
    return iptv::IsRecentChannel(user_, channel.id);
}

Model::Starred Model::toggle_favorite(unsigned catalog_index)
{
    if (catalog_index >= channel_count())
        return Starred::failed;
    const std::vector<std::string> previous = user_.favorite_ids;
    const bool favorite = iptv::ToggleFavorite(&user_, catalog_[catalog_index].id);
    if (user_.favorite_ids == previous)
        return Starred::failed;
    if (iptv::SaveUserState(path("iptv-favorites-v1.bin"), path("iptv-history-v1.bin"), user_) !=
        iptv::UserStateStatus::ok)
    {
        user_.favorite_ids = previous;
        return Starred::failed;
    }
    mark_lists();
    recount_groups();
    // Only the favorites list changes shape; everywhere else the channel
    // stays where it is and only wears a star.
    if (group_ == Group::favorites)
        rebuild_visible();
    diag::event("favorite %s: \"%s\"", favorite ? "added" : "removed",
                std::string(catalog_[catalog_index].name).c_str());
    return favorite ? Starred::added : Starred::removed;
}

// ---- playback -----------------------------------------------------------------------

std::optional<PlayRequest> Model::preview_request(std::string_view channel_id) const
{
    const auto index = catalog_.Find(channel_id);
    if (index >= catalog_.size())
        return {};
    const auto channel = catalog_[index];
    PlayRequest request;
    request.channel_id = channel.id;
    request.channel_name = channel.name;
    request.source_id = channel.source_id;
    if (!channel.url.empty())
        request.urls.emplace_back(channel.url);
    for (const auto url : channel.alternate_urls)
        if (!url.empty())
            request.urls.emplace_back(url);
    request.user_agent = channel.http_user_agent;
    request.referrer = channel.http_referrer;
    request.record_channel_result = false;
    if (active_source_ == iptv::SourceKind::Portal)
    {
        request.portal = portal_;
        request.portal_command = channel.portal_command;
    }
    return request.urls.empty() && request.portal_command.empty() ? std::nullopt
                                                                  : std::optional{request};
}

bool Model::play(unsigned catalog_index)
{
    if (catalog_index >= channel_count())
        return false;
    const iptv::ChannelView channel = catalog_[catalog_index];
    play_request_ = {};
    play_request_.channel_id = channel.id;
    play_request_.channel_name = channel.name;
    if (!channel.url.empty())
        play_request_.urls.emplace_back(channel.url);
    for (const std::string_view alternate : channel.alternate_urls)
        if (!alternate.empty())
            play_request_.urls.emplace_back(alternate);
    play_request_.user_agent = channel.http_user_agent;
    play_request_.referrer = channel.http_referrer;
    play_request_.source_id = channel.source_id;
    if (active_source_ == iptv::SourceKind::Portal)
    {
        play_request_.portal = portal_;
        play_request_.portal_command = channel.portal_command;
        if (channel.portal_command.empty())
            return false;
    }
    play_request_.reconnect_live = active_source_ == iptv::SourceKind::Xtream;
    play_requested_ = !play_request_.urls.empty();
    diag::event("play asked: \"%s\" id=%s addresses=%zu source=%d own user agent=%s referrer=%s",
                play_request_.channel_name.c_str(), play_request_.channel_id.c_str(),
                play_request_.urls.size(), static_cast<int>(active_source_),
                play_request_.user_agent.empty() ? "no" : "yes",
                play_request_.referrer.empty() ? "no" : "yes");
    for (const std::string &address : play_request_.urls)
        diag::event("  address: %s", redact_address(address).c_str());
    if (!play_requested_)
        return false;
    const std::vector<std::string> previous = user_.recent_channel_ids;
    (void)iptv::AddRecentChannel(&user_, channel.id);
    if (iptv::SaveUserState(path("iptv-favorites-v1.bin"), path("iptv-history-v1.bin"), user_) !=
        iptv::UserStateStatus::ok)
        user_.recent_channel_ids = previous;
    if (library_.ready() && !library_.remember_channel(selected_source_id_, channel.id))
        notify(Level::warning, "The last channel could not be saved");
    mark_lists();
    return true;
}

bool Model::take_play_request(PlayRequest *request)
{
    if (request == nullptr || !play_requested_)
        return false;
    *request = std::move(play_request_);
    play_request_ = {};
    play_requested_ = false;
    return true;
}

void Model::report_playback_failure(const char *channel_id, const char *channel_name, int result,
                                    unsigned attempts, const char *detail)
{
    diag::event("playback failed: \"%s\" id=%s result=%d",
                channel_name != nullptr ? channel_name : "",
                channel_id != nullptr ? channel_id : "", result);
    if (result >= 0)
        return;
    failure_ = {};
    failure_.channel_id = channel_id != nullptr ? channel_id : "";
    failure_.channel_name =
        channel_name != nullptr && *channel_name != '\0' ? channel_name : "this channel";
    failure_.reason =
        detail != nullptr && *detail != '\0' ? detail : "The channel may be offline right now.";
    failure_.attempts = attempts;
    failure_.can_retry = catalog_.Find(failure_.channel_id) != iptv::Catalog::npos;
    has_failure_ = true;
    std::fprintf(stderr, "[ProsperoTV][player] channel=%s result=%d attempts=%u reason=%s\n",
                 failure_.channel_id.c_str(), result, attempts, failure_.reason.c_str());
}

void Model::dismiss_failure()
{
    has_failure_ = false;
    failure_ = {};
}

bool Model::retry_failure()
{
    if (!has_failure_)
        return false;
    const std::string channel_id = failure_.channel_id;
    dismiss_failure();
    const std::size_t index = catalog_.Find(channel_id);
    return index != iptv::Catalog::npos && play(static_cast<unsigned>(index));
}

// ---- sources -------------------------------------------------------------------------

void Model::use_source(iptv::SourceKind source)
{
    if (library_.ready())
    {
        use_saved_source(static_cast<int>(source) + 1);
        return;
    }
    diag::event("source chosen: %d (set up=%d, an update running=%d)", static_cast<int>(source),
                is_set_up(source) ? 1 : 0, refresh_thread_ != nullptr ? 1 : 0);
    if (refresh_thread_ != nullptr)
    {
        notify(Level::warning, "An update is running",
               "Wait for it to finish, then choose the source again.");
        return;
    }
    if (!is_set_up(source))
    {
        edit_source(source);
        return;
    }
    const bool changed = source != active_source_;
    if (changed)
    {
        active_source_ = source;
        load_cache();
    }
    if (iptv::SaveActiveSource(path("iptv-active-source-v1.txt"), source) !=
        iptv::SourceStateStatus::ok)
        notify(Level::warning, "The choice could not be saved",
               "It holds until ProsperoTV is closed.");
    else if (changed)
        notify(Level::ready, std::string("Now using ") + source_name(source));
    set_source_text(std::string("Using ") + source_name(source),
                    catalog_loaded_ ? "Showing the saved copy while a new one downloads."
                                    : "Nothing is saved yet. Downloading it now.");
    refresh();
}

void Model::edit_source(iptv::SourceKind source)
{
    if (source == iptv::SourceKind::BuiltIn)
    {
        notify(Level::ready, "The iptv-org catalog is built in", "It has nothing to set up.");
        return;
    }
    if (refresh_thread_ != nullptr)
    {
        notify(Level::warning, "An update is running",
               "Wait for it to finish, then edit the source.");
        return;
    }
    if (!keyboard_ready_)
    {
        notify(Level::warning, "The keyboard is not available",
               "Close ProsperoTV and open it again.");
        return;
    }
    editing_source_id_ = static_cast<int>(source) + 1;
    if (source == iptv::SourceKind::Portal)
    {
        portal_form_ = portal_;
        account_step_ = AccountStep::portal_address;
        account_prompt_pending_ = true;
        return;
    }
    if (source == iptv::SourceKind::Custom)
    {
        iptv_ime_request_prompt(custom_url_.c_str(), "Playlist address",
                                "http(s)://host/playlist.m3u", IPTV_IME_BUFFER_CHARACTERS,
                                &Model::on_custom_url, this);
        return;
    }
    account_form_ = xtream_;
    account_step_ = AccountStep::server;
    account_prompt_pending_ = true;
}

void Model::on_custom_url(const char *text, void *self)
{
    if (self != nullptr && text != nullptr)
        static_cast<Model *>(self)->apply_custom_url(text);
}

void Model::apply_custom_url(const char *url)
{
    if (refresh_thread_ != nullptr)
    {
        notify(Level::warning, "The address was not changed", "An update was still running.");
        return;
    }
    if (!iptv::http::IsSupportedPlaylistUrl(url))
    {
        notify(Level::error, "That address cannot be used",
               "It must start with http:// or https:// and have no spaces.");
        return;
    }
    if (save_source_form(url, nullptr) && editing_source_id_ == 2)
        (void)iptv::SaveCustomSourceUrl(path("iptv-custom-source-v1.txt"), url);
}

void Model::continue_account_form()
{
    if (!account_prompt_pending_ || !keyboard_ready_)
        return;
    account_prompt_pending_ = false;
    switch (account_step_)
    {
    case AccountStep::server:
        iptv_ime_request_prompt(account_form_.server_url.c_str(), "Xtream server (1 of 3)",
                                "http(s)://provider.example:port", IPTV_IME_BUFFER_CHARACTERS,
                                &Model::on_account_server, this);
        break;
    case AccountStep::username:
        iptv_ime_request_prompt(account_form_.username.c_str(), "Xtream user name (2 of 3)",
                                "User name", IPTV_IME_BUFFER_CHARACTERS,
                                &Model::on_account_username, this);
        break;
    case AccountStep::password:
        iptv_ime_request_password("Xtream password (3 of 3)", "Password",
                                  IPTV_IME_BUFFER_CHARACTERS, &Model::on_account_password, this);
        break;
    case AccountStep::portal_address:
        iptv_ime_request_prompt(portal_form_.url.c_str(), "Portal address (1 of 2)",
                                "http(s)://provider.example/stalker_portal/c/",
                                IPTV_IME_BUFFER_CHARACTERS, &Model::on_portal_address, this);
        break;
    case AccountStep::portal_mac:
        iptv_ime_request_prompt(portal_form_.mac.c_str(), "Portal MAC code (2 of 2)",
                                "00:1A:79:12:34:56", 17, &Model::on_portal_mac, this);
        break;
    case AccountStep::none:
        break;
    }
}

void Model::on_account_server(const char *text, void *self)
{
    if (self != nullptr && text != nullptr)
        static_cast<Model *>(self)->apply_account_server(text);
}

void Model::on_account_username(const char *text, void *self)
{
    if (self != nullptr && text != nullptr)
        static_cast<Model *>(self)->apply_account_username(text);
}

void Model::on_account_password(const char *text, void *self)
{
    if (self != nullptr && text != nullptr)
        static_cast<Model *>(self)->apply_account_password(text);
}

void Model::apply_account_server(const char *server)
{
    std::string normalized;
    if (server == nullptr || !iptv::NormalizeXtreamServerUrl(server, &normalized))
    {
        account_step_ = AccountStep::none;
        notify(Level::error, "That server address cannot be used",
               "It must start with http:// or https://.");
        return;
    }
    account_form_.server_url = std::move(normalized);
    account_step_ = AccountStep::username;
    account_prompt_pending_ = true;
}

void Model::apply_account_username(const char *username)
{
    if (!valid_credential(username))
    {
        account_step_ = AccountStep::none;
        notify(Level::error, "The user name cannot be empty");
        return;
    }
    account_form_.username = username;
    account_step_ = AccountStep::password;
    account_prompt_pending_ = true;
}

void Model::apply_account_password(const char *password)
{
    account_step_ = AccountStep::none;
    if (!valid_credential(password))
    {
        notify(Level::error, "The password cannot be empty");
        return;
    }
    account_form_.password = password;
    if (!iptv::ValidateXtreamCredentials(account_form_))
    {
        notify(Level::error, "The account was not accepted",
               "Check the server address, the user name and the password.");
        return;
    }
    if (save_source_form(account_form_.server_url, &account_form_) && editing_source_id_ == 3)
        (void)iptv::SaveXtreamCredentials(path("prosperotv-xtream-v1.txt"), account_form_);
    account_form_ = {};
}

// ---- the download ---------------------------------------------------------------------

void Model::refresh()
{
    vod_.stop();
    stop_guide();
    next_schedule_check_ = platform::unix_time() + 3600;
    diag::event("channel list update asked: source=%d set up=%d already running=%d",
                static_cast<int>(active_source_), is_set_up(active_source_) ? 1 : 0,
                refresh_thread_ != nullptr ? 1 : 0);
    if (refresh_thread_ != nullptr)
    {
        refresh_queued_ = true;
        return;
    }
    refresh_queued_ = false;
    if (!is_set_up(active_source_))
    {
        if (active_source_ == iptv::SourceKind::Xtream ||
            active_source_ == iptv::SourceKind::Portal)
            edit_source(active_source_);
        return;
    }

    const bool custom = active_source_ == iptv::SourceKind::Custom;
    const bool account = active_source_ == iptv::SourceKind::Xtream;
    refresh_source_ = active_source_;
    refresh_url_ = custom ? custom_url_ : account ? std::string() : std::string(kCatalogUrl);
    refresh_cache_path_ = cache_path(active_source_);
    refresh_source_id_ = source_id(active_source_);
    refresh_account_ = account ? xtream_ : iptv::XtreamCredentials{};
    refresh_portal_ = active_source_ == iptv::SourceKind::Portal ? portal_ : PortalCredentials{};
    const SourceHealth before = health_[static_cast<unsigned>(active_source_)];
    health_[static_cast<unsigned>(active_source_)] = SourceHealth::refreshing;

    refresh_done_.store(false, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    refresh_count_.store(0, std::memory_order_relaxed);
    pending_saved_ = false;
    pending_account_ = iptv::XtreamStatus::ok;
    pending_account_message_.clear();
    set_status("Updating", Level::busy);
    set_source_text(std::string("Downloading ") + source_name(active_source_),
                    catalog_loaded_ ? "The saved channels stay available meanwhile."
                                    : "This can take a minute the first time.");
    catalog_failed_ = false;
    catalog_error_.clear();

    refresh_thread_ = platform::thread_start(&Model::refresh_entry, this, kCatalogThreadStackBytes,
                                             "iptv-catalog");
    if (refresh_thread_ != nullptr)
        return;

    health_[static_cast<unsigned>(active_source_)] =
        before == SourceHealth::refreshing ? SourceHealth::error : before;
    set_status("Update failed", Level::error);
    set_source_text("The update could not start", "Press Options to try again.");
    if (!catalog_loaded_)
    {
        health_[static_cast<unsigned>(active_source_)] = SourceHealth::error;
        catalog_failed_ = true;
        catalog_error_ = "The update could not start. Try again.";
    }
    else
    {
        notify(Level::warning, "The update could not start", "Press Options to try again.");
    }
}

void *Model::refresh_entry(void *self)
{
    static_cast<Model *>(self)->run_refresh();
    return nullptr;
}

// Runs on the worker thread. It touches only the pending_ and refresh_
// members, which the frame loop leaves alone until refresh_done_ is set.
//
// The list is read as it arrives: each piece of the download goes to a parser
// that puts its channels into pending_catalog_ and keeps nothing else, so a
// list of a hundred megabytes costs the memory of its channels and no more.
// The index of the new catalog is built here too, before the frame loop is
// told: taking the catalog over is then a matter of a few moves.
void Model::run_refresh()
{
    pending_fetch_ = {};
    pending_catalog_ = {};
    pending_index_.clear();
    pending_report_ = {};
    pending_saved_ = false;
    pending_account_ = iptv::XtreamStatus::ok;
    pending_account_stage_.clear();
    pending_account_message_.clear();
    pending_network_ = platform::network_init();

    const auto stopping = [this]() { return stop_requested_.load(std::memory_order_acquire); };
    if (pending_network_ == iptv::http::Status::ok && !stopping())
    {
        const iptv::http::RequestControl control{
            [](void *context) {
                return static_cast<const Model *>(context)->stop_requested_.load(
                    std::memory_order_acquire);
            },
            this};
        if (refresh_source_ == iptv::SourceKind::Portal)
        {
            PortalClient client(refresh_portal_, &control);
            const bool ok = client.load(&pending_catalog_, &pending_report_, &refresh_count_);
            pending_fetch_.status =
                ok ? iptv::http::Status::ok : iptv::http::Status::request_failed;
            pending_account_message_ = client.error();
        }
        else if (refresh_source_ == iptv::SourceKind::Xtream)
        {
            // The sign-in and the categories are small and read whole.
            iptv::http::ListBuffer response =
                iptv::http::AllocateListBuffer(iptv::kMaxXtreamReplyBytes);
            std::string endpoint;
            std::vector<iptv::XtreamCategory> categories;
            iptv::XtreamAuth auth;
            bool reachable = true;
            const auto fetch = [&](std::string_view action)
            {
                if (!iptv::BuildXtreamApiUrl(refresh_account_, action, &endpoint))
                {
                    pending_account_ = iptv::XtreamStatus::invalid_argument;
                    return false;
                }
                pending_fetch_ = platform::fetch(endpoint.c_str(), response.data(), response.size(),
                                                 response.max_bytes, &control);
                return pending_fetch_.status == iptv::http::Status::ok && !stopping();
            };
            pending_account_stage_ = "authentication";
            if (fetch(""))
            {
                pending_account_ = iptv::ParseXtreamAuth(
                    std::string_view(response.data(), pending_fetch_.bytes), &auth);
                if (pending_account_ != iptv::XtreamStatus::ok)
                    pending_account_message_ = auth.message;
            }
            else
            {
                reachable = false;
            }
            if (reachable && pending_account_ == iptv::XtreamStatus::ok)
            {
                pending_account_stage_ = "categories";
                if (fetch("get_live_categories"))
                    pending_account_ = iptv::ParseXtreamCategories(
                        std::string_view(response.data(), pending_fetch_.bytes), &categories);
                else
                    reachable = false;
            }
            response = {};
            if (reachable && pending_account_ == iptv::XtreamStatus::ok)
            {
                pending_account_stage_ = "live-streams";
                if (!iptv::BuildXtreamApiUrl(refresh_account_, "get_live_streams", &endpoint))
                {
                    pending_account_ = iptv::XtreamStatus::invalid_argument;
                }
                else
                {
                    iptv::XtreamStreamsParser parser(refresh_account_, categories,
                                                     refresh_source_id_, &pending_catalog_,
                                                     &pending_report_);
                    struct Receiver
                    {
                        Model *model;
                        iptv::XtreamStreamsParser *parser;
                    } receiver{this, &parser};
                    const iptv::http::ListSink sink{
                        [](void *context, const char *data, std::size_t bytes)
                        {
                            auto *to = static_cast<Receiver *>(context);
                            const bool read = to->parser->Feed(std::string_view(data, bytes));
                            to->model->refresh_count_.store(
                                static_cast<unsigned>(to->model->pending_catalog_.size()),
                                std::memory_order_relaxed);
                            // Enough once the catalog is full.
                            return read && !to->parser->full();
                        },
                        &receiver};
                    pending_fetch_ = platform::fetch_list(endpoint.c_str(), sink,
                                                          iptv::kMaxXtreamResponseBytes, &control);
                    // Stopped by the parser: what it says is the answer.
                    if (pending_fetch_.status == iptv::http::Status::stopped)
                        pending_fetch_.status = iptv::http::Status::ok;
                    if (pending_fetch_.status == iptv::http::Status::ok && !stopping())
                        pending_account_ = parser.Finish();
                    else
                        pending_catalog_.Clear();
                }
            }
        }
        else
        {
            iptv::M3uParser parser(&pending_catalog_, refresh_source_id_, {}, &pending_report_);
            struct Receiver
            {
                Model *model;
                iptv::M3uParser *parser;
                std::size_t bytes = 0;
            } receiver{this, &parser};
            const iptv::http::ListSink sink{
                [](void *context, const char *data, std::size_t bytes)
                {
                    auto *to = static_cast<Receiver *>(context);
                    to->bytes += bytes;
                    if (!to->parser->Feed(std::string_view(data, bytes)))
                        return false;
                    const std::size_t channels = to->model->pending_catalog_.size();
                    to->model->refresh_count_.store(static_cast<unsigned>(channels),
                                                    std::memory_order_relaxed);
                    // Enough once the catalog is full, or when this is no playlist.
                    return !to->parser->full() &&
                           (channels != 0 || to->bytes < kPlaylistProbeBytes);
                },
                &receiver};
            pending_fetch_ = platform::fetch_list(refresh_url_.c_str(), sink,
                                                  iptv::http::kMaxListBytes, &control);
            if (pending_fetch_.status == iptv::http::Status::stopped)
                pending_fetch_.status = pending_report_.input_too_large
                                            ? iptv::http::Status::response_too_large
                                            : iptv::http::Status::ok;
            if (pending_fetch_.status == iptv::http::Status::ok && !stopping())
                parser.Finish();
            else
                pending_catalog_.Clear();
        }
        if (!pending_catalog_.empty() && !stopping())
        {
            pending_saved_ =
                iptv::SaveCatalog(refresh_cache_path_, pending_catalog_) == iptv::StoreStatus::ok;
            pending_index_.build(pending_catalog_);
        }
    }
    if (pending_network_ == iptv::http::Status::ok)
        platform::network_shutdown();
    refresh_done_.store(true, std::memory_order_release);
}

bool Model::join_refresh()
{
    if (refresh_thread_ == nullptr)
        return true;
    const int joined = platform::thread_join(refresh_thread_);
    if (joined != 0)
    {
        if (!refresh_done_.load(std::memory_order_acquire))
            return false;
        const int detached = platform::thread_detach(refresh_thread_);
        std::fprintf(stderr, "[ProsperoTV] refresh join failed: %d; detach fallback: %d\n", joined,
                     detached);
    }
    refresh_thread_ = nullptr;
    return true;
}

// The account's last answer, without the account: for looking into a provider
// that will not sign in.
void Model::save_account_receipt() const
{
    const std::string target = path("prosperotv-xtream-receipt.txt");
    const std::string temporary = target + ".tmp";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr)
        return;
    std::fprintf(
        file,
        "PROSPEROTV_XTREAM_RECEIPT_V1\n"
        "stage=%s\nnetwork_status=%u\nfetch_status=%u\nhttp_status=%d\n"
        "native_error=0x%08x\nresponse_bytes=%llu\nxtream_status=%u\n"
        "xtream_description=%s\nlines_seen=%u\naccepted=%u\nskipped=%u\nchannels=%llu\n",
        pending_account_stage_.empty() ? "none" : pending_account_stage_.c_str(),
        static_cast<unsigned>(pending_network_), static_cast<unsigned>(pending_fetch_.status),
        pending_fetch_.http_status, static_cast<unsigned>(pending_fetch_.native_error),
        static_cast<unsigned long long>(pending_fetch_.bytes),
        static_cast<unsigned>(pending_account_), iptv::XtreamStatusDescription(pending_account_),
        static_cast<unsigned>(pending_report_.lines_seen),
        static_cast<unsigned>(pending_report_.accepted),
        static_cast<unsigned>(pending_report_.skipped),
        static_cast<unsigned long long>(pending_catalog_.size()));
    const bool written = std::ferror(file) == 0 && std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
    {
        std::remove(temporary.c_str());
        return;
    }
    std::remove(target.c_str());
    if (std::rename(temporary.c_str(), target.c_str()) != 0)
        std::remove(temporary.c_str());
}

void Model::consume_refresh()
{
    if (refresh_thread_ == nullptr || !refresh_done_.load(std::memory_order_acquire))
        return;
    if (!join_refresh())
        return;
    refresh_done_.store(false, std::memory_order_relaxed);

    const bool account = refresh_source_ == iptv::SourceKind::Xtream;
    if (account)
        save_account_receipt();
    const bool success = pending_network_ == iptv::http::Status::ok &&
                         pending_fetch_.status == iptv::http::Status::ok &&
                         (!account || pending_account_ == iptv::XtreamStatus::ok) &&
                         !pending_catalog_.empty() &&
                         pending_index_.size() == pending_catalog_.size();
    const unsigned source = static_cast<unsigned>(refresh_source_);
    diag::event(
        "channel list update ended: source=%u success=%d network=%d fetch status=%d http=%d "
        "native=0x%08x bytes=%zu account=%d channels=%zu skipped=%zu more than held=%d saved=%d",
        source, success ? 1 : 0, static_cast<int>(pending_network_),
        static_cast<int>(pending_fetch_.status), pending_fetch_.http_status,
        static_cast<unsigned>(pending_fetch_.native_error), pending_fetch_.bytes,
        account ? static_cast<int>(pending_account_) : -1, pending_catalog_.size(),
        static_cast<std::size_t>(pending_report_.skipped), pending_report_.catalog_full ? 1 : 0,
        pending_saved_ ? 1 : 0);
    if (success)
    {
        catalog_ = std::move(pending_catalog_);
        index_ = std::move(pending_index_);
        catalog_loaded_ = true;
        catalog_failed_ = false;
        catalog_error_.clear();
        saved_unix_ = pending_saved_ ? platform::unix_time() : 0;
        (void)iptv::LoadPlaybackResults(path("prosperotv-playback-history.sqlite3"),
                                        catalog_.source_id, &catalog_);
        adopt_catalog();
        next_guide_check_ = 0;
        if (has_failure_)
            failure_.can_retry = catalog_.Find(failure_.channel_id) != iptv::Catalog::npos;
        health_[source] = pending_saved_ ? SourceHealth::ready : SourceHealth::stale;
        const std::string count = group_digits(channel_count()) + " channels";
        set_status(pending_saved_ ? "Up to date" : "Not saved",
                   pending_saved_ ? Level::ready : Level::warning);
        set_source_text(pending_saved_ ? "Up to date" : "Downloaded, but not saved",
                        pending_saved_
                            ? count + " from " + source_name(refresh_source_) + "."
                            : count + ". The console's storage refused the copy, so it lasts until "
                                      "ProsperoTV is closed.");
        notify(pending_saved_ ? Level::ready : Level::warning, "Channel list updated", count);
        if (pending_report_.catalog_full)
            notify(Level::warning, "This source has more channels than ProsperoTV holds",
                   "Showing its first " + group_digits(channel_count()) + ".");
    }
    else
    {
        const std::string problem =
            refresh_source_ == iptv::SourceKind::Portal && !pending_account_message_.empty()
                ? pending_account_message_
                : fetch_problem(pending_network_, pending_fetch_, account, pending_account_,
                                pending_account_message_, pending_report_.skipped);
        if (catalog_loaded_)
        {
            health_[source] = SourceHealth::stale;
            set_status("Saved copy", Level::warning);
            set_source_text("The update failed", problem + " Showing the " +
                                                     group_digits(channel_count()) +
                                                     " channels saved on this console.");
            notify(Level::warning, "The channel list could not be updated", problem);
        }
        else
        {
            health_[source] = SourceHealth::error;
            set_status("No channels", Level::error);
            set_source_text("The channel list could not be downloaded", problem);
            catalog_failed_ = true;
            catalog_error_ = problem;
        }
    }
    pending_catalog_ = {};
    pending_index_.clear();
    refresh_count_.store(0, std::memory_order_relaxed);
    if (refresh_queued_)
    {
        refresh_queued_ = false;
        refresh();
    }
}

} // namespace ptv
