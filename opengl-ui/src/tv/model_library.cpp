// ProsperoTV - Source selection and personal browsing lists.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/i18n.hpp"
#include "tv/model.hpp"
#include "tv/usb_playlist.hpp"
#include "tv/category_path.hpp"
#include "tv/platform.hpp"
#include "tv/settings.hpp"
#include "tv/local_tv.hpp"
#include "iptv_ime.h"

#include <algorithm>

namespace ptv
{
bool Model::export_shared_library(SharedLibrary &out)
{
    out = {};
    if (!require_parent() || !library_.ready())
        return false;
    for (const auto &source : library_.sources())
    {
        if (source.kind == 1 && usb_playlist_path(source.url))
            ++out.skipped_local;
        else if (portable_source(source))
            out.sources.push_back(
                {source, library_.guide_url(source.id), library_.guide_mapping(source.id)});
    }
    out.favorites = user_.favorite_ids;
    for (const auto &folder : library_.folders())
    {
        const auto ids = library_.folder_channels(folder);
        out.folders[folder] = {ids.begin(), ids.end()};
    }
    std::string checked;
    return encode_shared_library(out, checked);
}
void Model::load_library()
{
    selected_source_id_ = static_cast<int>(active_source_) + 1;
    const auto settings = load_settings(data_dir_);
    hide_failed_ = settings.hide_failed;
    if (!library_.open(path("prosperotv-library.sqlite3")))
    {
        notify(Level::warning, tr("Library settings could not be opened"));
        return;
    }
    sources_ = library_.sources();
    // Import without altering any original credentials or catalog files.
    const SavedSource originals[] = {
        {1, 0, "iptv-org public catalog", builtin_url(), {}, {}, {}, RefreshSchedule::daily},
        {2, 1, "Custom playlist", custom_url_, {}, {}, {}, RefreshSchedule::daily},
        {3,
         2,
         "Xtream Codes account",
         xtream_.server_url,
         xtream_.username,
         xtream_.password,
         {},
         RefreshSchedule::daily},
        {4, 3, "MAC-code portal", {}, {}, {}, {}, RefreshSchedule::daily}};
    for (SavedSource source : originals)
        if (saved_source(source.id) == nullptr && !library_.save_source(&source))
            notify(Level::warning, tr("A source could not be imported"));
    sources_ = library_.sources();
    const auto saved = library_.selected_source();
    if (saved != 0 && saved_source(saved) != nullptr && saved_source(saved)->kind <= 5)
        selected_source_id_ = saved;
    const auto last = library_.last_channel();
    if (settings.resume_last && last.first != 0 && saved_source(last.first) != nullptr &&
        saved_source(last.first)->kind <= 5)
        selected_source_id_ = last.first;
    if (const auto *source = saved_source(selected_source_id_))
        select_source_record(*source);
    else
        selected_source_id_ = 1;
    warn_account_expiry();
}

const SavedSource *Model::saved_source(std::int64_t id) const
{
    for (const auto &source : sources_)
        if (source.id == id)
            return &source;
    return nullptr;
}

void Model::select_source_record(const SavedSource &source)
{
    cancel_source_check("Check stopped for source change");
    clear_home_playback();
    set_vod_overview(false);
    end_search();
    const bool changed = selected_source_id_ != source.id;
    selected_source_id_ = source.id;
    active_source_ = static_cast<iptv::SourceKind>(source.kind);
    schedule_ = source.schedule;
    if (source.kind == 1)
        custom_url_ = source.url;
    if (source.kind == 2)
        xtream_ = {source.url, source.username, source.password};
    if (source.kind == 3)
        portal_ = {source.url, source.mac, source.user_agent, source.referrer,
                   source.extra_headers};
    if (source.kind >= 4)
        local_source_ = source;
    vod_.configure(source.kind == 2 ? xtream_ : iptv::XtreamCredentials{},
                   cache_path(active_source_), schedule_, source.user_agent, source.referrer,
                   source.extra_headers);
    if (source.kind == 1)
        vod_.configure_playlist(&catalog_);
    refresh_counts();
    if (changed)
    {
        view.vod_kind = -1;
        view.vod_focus = 0;
        view.vod_category.clear();
        view.vod_series.clear();
        view.vod_series_name.clear();
        view.vod_series_cover.clear();
        view.vod_series_group.clear();
        view.vod_series_adult = false;
        view.vod_query.clear();
        view.vod_all = false;
    }
}

bool Model::refresh_needed() const
{
    if (playlist_cache_needs_refresh_)
        return true;
    if (catalog_loaded_ && schedule_ == RefreshSchedule::manual)
        return false;
    return !catalog_loaded_ || refresh_due(schedule_, saved_unix_, platform::unix_time());
}

void Model::apply_source_http(PlayRequest &request) const
{
    if (const auto *source = saved_source(selected_source_id_))
    {
        request.extra_headers = source->extra_headers;
        // USB lists have no HTTP origin; playback scopes to each initial stream URL.
        if (!usb_playlist_path(source->url) && !request.extra_headers.empty() &&
            request.credential_origin.empty())
            request.credential_origin = source->url;
        if (request.user_agent.empty())
            request.user_agent = source->user_agent;
        if (request.referrer.empty())
            request.referrer = source->referrer;
    }
}

void Model::edit_source_http(std::int64_t id)
{
    if (!require_parent() || refreshing() || iptv_ime_busy() || !keyboard_ready_ ||
        account_step_ != AccountStep::none)
        return;
    const auto *source = saved_source(id);
    if (!source || source->kind == 0)
        return;
    if (source->user_agent.size() > IPTV_IME_BUFFER_CHARACTERS ||
        source->referrer.size() > IPTV_IME_BUFFER_CHARACTERS)
    {
        notify(Level::warning, tr("Edit long HTTP settings from the paired phone"));
        return;
    }
    http_form_ = *source;
    account_step_ = AccountStep::http_user_agent;
    account_prompt_pending_ = true;
}

bool Model::set_source_user_agent(std::int64_t id, const std::string &user_agent)
{
    if (!require_parent() || refreshing() || iptv_ime_busy() || account_step_ != AccountStep::none)
        return false;
    const auto *source = saved_source(id);
    if (!source || source->kind == 0)
        return false;
    auto updated = *source;
    updated.user_agent = user_agent;
    return commit_source(std::move(updated), false);
}

void Model::on_http_user_agent(const char *text, void *self)
{
    if (!self)
        return;
    auto &model = *static_cast<Model *>(self);
    if (!text)
    {
        model.account_step_ = AccountStep::none;
        model.http_form_ = {};
        return;
    }
    model.http_form_.user_agent = text;
    model.account_step_ = AccountStep::http_referrer;
    model.account_prompt_pending_ = true;
}

void Model::on_http_referrer(const char *text, void *self)
{
    if (!self)
        return;
    auto &model = *static_cast<Model *>(self);
    if (!text)
    {
        model.account_step_ = AccountStep::none;
        model.http_form_ = {};
        return;
    }
    model.http_form_.referrer = text;
    model.account_step_ = AccountStep::http_extra;
    model.account_prompt_pending_ = true;
}

void Model::on_http_extra(const char *text, void *self)
{
    if (!self)
        return;
    auto &model = *static_cast<Model *>(self);
    if (!text)
    {
        model.account_step_ = AccountStep::none;
        model.http_form_ = {};
        return;
    }
    const std::string line(text);
    if (!line.empty())
    {
        std::vector<iptv::http::NamedHeader> incoming, existing;
        bool valid = line == "-";
        std::string updated;
        if (!valid && line.find('\n') == line.npos &&
            iptv::http::ParseExtraHeaders(line, &incoming) && incoming.size() == 1 &&
            iptv::http::ParseExtraHeaders(model.http_form_.extra_headers, &existing))
        {
            const auto lower = [](std::string value)
            {
                for (char &c : value)
                    if (c >= 'A' && c <= 'Z')
                        c += 'a' - 'A';
                return value;
            };
            for (const auto &field : existing)
                if (lower(field.name) != lower(incoming[0].name))
                    updated += field.name + ": " + field.value + "\n";
            updated += incoming[0].name + ": " + incoming[0].value;
            valid = iptv::http::ParseExtraHeaders(updated);
        }
        if (valid)
            model.http_form_.extra_headers = std::move(updated);
        else
            model.notify(Level::warning, tr("Invalid HTTP header"));
        model.account_prompt_pending_ = true;
        return;
    }
    model.account_step_ = AccountStep::none;
    model.commit_source(model.http_form_);
    model.http_form_ = {};
}

void Model::warn_account_expiry()
{
    const auto *source = saved_source(selected_source_id_);
    if (!source || source->kind != 2)
        return;
    const int days = account_expiry_notice(source->account, platform::unix_time());
    if (days)
        notify(Level::warning,
               days < 0 ? tr("Account expiry date has passed")
                        : tr("Account expires within a week"),
               source->name + ". " + tr("Check the account details in Sources."));
}

void Model::use_saved_source(std::int64_t id)
{
    if (refreshing())
    {
        notify(Level::warning, tr("An update is running"),
               tr("Wait for it to finish, then choose a source."));
        return;
    }
    const auto *source = saved_source(id);
    if (source == nullptr || source->kind > 5)
        return;
    if (source->kind != 0 && source->url.empty())
    {
        edit_saved_source(id);
        return;
    }
    if (!library_.select_source(id))
    {
        notify(Level::error, tr("The source choice could not be saved"));
        return;
    }
    const bool changed = id != selected_source_id_;
    select_source_record(*source);
    if (changed)
    {
        provider_category_.clear();
        load_cache();
    }
    if (id <= 3)
        (void)iptv::SaveActiveSource(path("iptv-active-source-v1.txt"), active_source_);
    set_source_text(source->name, catalog_loaded_ ? tr("Showing the saved channel list.")
                                                  : tr("Downloading the first copy."));
    if (refresh_needed())
        refresh();
    else
        set_status(tr("Saved copy"), Level::ready);
    warn_account_expiry();
}

void Model::add_source(iptv::SourceKind kind)
{
    if (!require_parent() || iptv_ime_busy())
        return;
    if (kind == iptv::SourceKind::BuiltIn || refreshing() || !keyboard_ready_)
        return;
    if (kind == iptv::SourceKind::Custom)
        iptv_ime_request_prompt("", tr("Playlist address"), "http(s)://host/playlist.m3u",
                                IPTV_IME_BUFFER_CHARACTERS, &Model::on_custom_url, this);
    else
        edit_source(kind);
    editing_source_id_ = 0;
    if (kind == iptv::SourceKind::Xtream)
        account_form_ = {};
    if (kind == iptv::SourceKind::Portal)
        portal_form_ = {};
    if (kind == iptv::SourceKind::HDHomeRun || kind == iptv::SourceKind::Tvheadend)
    {
        local_form_ = {};
        local_form_.kind = static_cast<int>(kind);
    }
}

void Model::edit_saved_source(std::int64_t id)
{
    if (!require_parent() || iptv_ime_busy())
        return;
    const auto *source = saved_source(id);
    if (source == nullptr || source->kind == 0 || source->kind > 5 || refreshing() ||
        !keyboard_ready_)
        return;
    const auto copy = *source;
    if (copy.kind == 1)
        iptv_ime_request_prompt(copy.url.c_str(), tr("Playlist address"),
                                "http(s)://host/playlist.m3u", IPTV_IME_BUFFER_CHARACTERS,
                                &Model::on_custom_url, this);
    else
        edit_source(static_cast<iptv::SourceKind>(source->kind));
    editing_source_id_ = id;
    if (copy.kind == 2)
        account_form_ = {copy.url, copy.username, copy.password};
    if (copy.kind == 3)
        portal_form_ = {copy.url, copy.mac};
    if (copy.kind >= 4)
        local_form_ = copy;
}

void Model::on_local_address(const char *text, void *self)
{
    auto &model = *static_cast<Model *>(self);
    if (!text || !local_tv_address(text, &model.local_form_.url))
    {
        model.account_step_ = AccountStep::none;
        model.notify(Level::error, tr("That TV server address cannot be used"),
                     tr("Enter its HTTP or HTTPS address, including its port when needed."));
        return;
    }
    if (model.local_form_.kind == 4)
        model.commit_local_form();
    else
    {
        model.account_step_ = AccountStep::local_username;
        model.account_prompt_pending_ = true;
    }
}

void Model::on_local_username(const char *text, void *self)
{
    auto &model = *static_cast<Model *>(self);
    if (!text)
    {
        model.account_step_ = AccountStep::none;
        return;
    }
    model.local_form_.username = text;
    if (model.local_form_.username.empty())
    {
        model.local_form_.password.clear();
        model.commit_local_form();
    }
    else
    {
        model.account_step_ = AccountStep::local_password;
        model.account_prompt_pending_ = true;
    }
}

void Model::on_local_password(const char *text, void *self)
{
    auto &model = *static_cast<Model *>(self);
    if (!text)
    {
        model.account_step_ = AccountStep::none;
        return;
    }
    model.local_form_.password = text;
    model.commit_local_form();
}

void Model::commit_local_form()
{
    account_step_ = AccountStep::none;
    local_form_.id = editing_source_id_;
    if (local_form_.name.empty())
        local_form_.name = local_form_.kind == 4 ? "HDHomeRun" : "Tvheadend";
    if (!valid_source(local_form_))
        notify(Level::error, tr("The TV server details could not be saved"),
               tr("Check the address and account details."));
    else
        (void)commit_source(local_form_);
    local_form_ = {};
}

bool Model::save_source_form(std::string_view url, const iptv::XtreamCredentials *account)
{
    SavedSource source;
    if (const auto *previous = saved_source(editing_source_id_))
        source = *previous;
    source.id = editing_source_id_;
    source.kind = account != nullptr ? 2 : 1;
    source.url = url;
    if (source.name.empty())
        source.name = std::string(account != nullptr ? "Xtream account " : "Playlist ") +
                      std::to_string(sources_.size() - 2);
    if (account != nullptr)
    {
        source.username = account->username;
        source.password = account->password;
    }
    return commit_source(std::move(source));
}

bool Model::commit_source(SavedSource source, bool activate)
{
    if (!require_parent())
        return false;
    if (!library_.save_source(&source))
    {
        notify(Level::error, tr("The source could not be saved"));
        return false;
    }
    sources_ = library_.sources();
    // Editing the selected source must discard the old source's in-memory catalog.
    if (source.id == selected_source_id_)
    {
        select_source_record(source);
        load_cache();
    }
    if (activate || source.id == selected_source_id_)
        use_saved_source(source.id);
    else
        ++revision_;
    return true;
}

void Model::on_portal_address(const char *text, void *self)
{
    if (!self || !text)
        return;
    auto &model = *static_cast<Model *>(self);
    if (!portal_endpoint(text, &model.portal_form_.url))
    {
        model.account_step_ = AccountStep::none;
        model.notify(Level::error, tr("That portal address cannot be used"),
                     tr("Enter the provider's HTTP or HTTPS portal address."));
        return;
    }
    model.account_step_ = AccountStep::portal_mac;
    model.account_prompt_pending_ = true;
}

void Model::on_portal_mac(const char *text, void *self)
{
    if (!self || !text)
        return;
    auto &model = *static_cast<Model *>(self);
    model.account_step_ = AccountStep::none;
    if (!portal_mac(text, &model.portal_form_.mac))
    {
        model.notify(Level::error, tr("That MAC code cannot be used"),
                     tr("Use the six pairs supplied by the provider, such as 00:1A:79:12:34:56."));
        return;
    }
    SavedSource source;
    if (const auto *previous = model.saved_source(model.editing_source_id_))
        source = *previous;
    source.id = model.editing_source_id_;
    source.kind = 3;
    source.url = model.portal_form_.url;
    source.mac = model.portal_form_.mac;
    if (source.name.empty())
        source.name = "MAC-code portal " + std::to_string(model.sources_.size() - 2);
    (void)model.commit_source(std::move(source));
    model.portal_form_ = {};
}

bool Model::remove_source(std::int64_t id)
{
    if (!require_parent())
        return false;
    if (id <= 4 || refreshing() || !library_.remove_source(id))
        return false;
    sources_ = library_.sources();
    if (selected_source_id_ == id)
        use_saved_source(1);
    return true;
}

bool Model::set_schedule(std::int64_t id, RefreshSchedule schedule)
{
    if (!require_parent())
        return false;
    const auto *source = saved_source(id);
    if (source == nullptr)
        return false;
    auto next = *source;
    next.schedule = schedule;
    if (!library_.save_source(&next))
        return false;
    sources_ = library_.sources();
    if (id == selected_source_id_)
    {
        schedule_ = schedule;
        vod_.configure(active_source_ == iptv::SourceKind::Xtream ? xtream_
                                                                  : iptv::XtreamCredentials{},
                       cache_path(active_source_), schedule_, next.user_agent, next.referrer,
                       next.extra_headers);
        if (active_source_ == iptv::SourceKind::Custom)
            vod_.configure_playlist(&catalog_);
    }
    ++revision_;
    return true;
}

bool Model::play_vod(unsigned index)
{
    if (vod_.kind() == VodKind::series || pin_prompt() || !vod_allowed(index))
        return false;
    const auto item = vod_.catalog()[index];
    return queue_vod(item, vod_.kind() == VodKind::episodes ? vod_.series() : std::string());
}
bool Model::queue_vod(const iptv::ChannelView &item, const std::string &series, bool inherit_series)
{
    if (!playback_allowed())
        return false;
    if (inherit_series)
        clear_home_playback();
    auto snapshot = item.Copy();
    if (inherit_series && !series.empty())
    {
        snapshot.group_title = view.vod_series_group;
        snapshot.adult = snapshot.adult || view.vod_series_adult;
        if (!view.vod_series_name.empty())
            snapshot.name = view.vod_series_name + " / " + snapshot.name;
    }
    if (!library_.remember_vod(selected_source_id_, source_id(active_source_), snapshot, series,
                               static_cast<std::int64_t>(platform::unix_time())))
        notify(Level::warning, tr("Continue watching could not be saved"), "");
    sleep_timer.wake();
    play_request_ = {};
    play_request_.channel_id = item.id;
    play_request_.channel_name = item.name;
    play_request_.urls = {std::string(item.url)};
    for (const auto url : item.alternate_urls)
        if (!url.empty())
            play_request_.urls.emplace_back(url);
    play_request_.source_id = item.source_id;
    play_request_.user_agent = item.http_user_agent;
    play_request_.referrer = item.http_referrer;
    apply_source_http(play_request_);
    play_request_.record_channel_result = false;
    play_request_.on_demand = true;
    play_request_.start_position_us = library_.playback_progress(item.id).first;
    play_request_.series_id = series;
    play_requested_ = true;
    return true;
}
bool Model::save_vod_progress(std::string_view id, std::uint64_t position_us,
                              std::uint64_t duration_us)
{
    const bool saved = library_.save_playback_progress(id, position_us, duration_us);
    if (saved)
        ++revision_;
    return saved;
}

bool Model::finish_vod(const PlayRequest &request, std::uint64_t position_us,
                       std::uint64_t duration_us, bool completed)
{
    if (!request.on_demand)
        return false;
    if (!save_vod_progress(request.channel_id, completed ? 0 : position_us, duration_us))
        notify(Level::error, tr("Playback position could not be saved"), "");
    if (completed && !sleep_timer.sleeping() && home_source_ != 0)
        return next_home_episode(request);
    if (!completed || sleep_timer.sleeping() || request.source_id != source_id(active_source_) ||
        vod_.kind() != VodKind::episodes || request.series_id != vod_.series())
        return false;
    const auto &catalog = vod_.catalog();
    const auto current = catalog.Find(request.channel_id);
    if (current == iptv::Catalog::npos)
        return false;
    unsigned next = static_cast<unsigned>(catalog.size());
    for (unsigned i = 0; i < catalog.size(); ++i)
        if (catalog[i].source_line > catalog[current].source_line &&
            (next == catalog.size() || catalog[i].source_line < catalog[next].source_line))
            next = i;
    // Do not skip a locked episode to autoplay something later in the series.
    return next < catalog.size() && play_vod(next);
}

bool Model::ask_vod_query()
{
    if (!keyboard_ready_)
        return false;
    iptv_ime_request_prompt(view.vod_query.c_str(), tr("Search on demand"),
                            tr("Movie, show or episode title"), IPTV_IME_BUFFER_CHARACTERS,
                            &Model::on_vod_query, this);
    return true;
}
void Model::on_vod_query(const char *text, void *self)
{
    if (!self || !text)
        return;
    auto &model = *static_cast<Model *>(self);
    model.view.vod_query = text;
    model.view.vod_focus = 0;
    ++model.revision_;
}

void Model::set_hide_failed(bool hide)
{
    if (hide_failed_ == hide)
        return;
    hide_failed_ = hide;
    mark_visibility();
    recount_groups();
    rebuild_visible();
}

bool Model::check_sleep_timer()
{
    if (!sleep_timer.expire(platform::monotonic_us()))
        return false;
    play_requested_ = false;
    play_request_ = {};
    notify(Level::ready, tr("Sleep timer finished"), tr("Choose a channel to watch again."));
    return true;
}

void Model::resume_last(bool enabled)
{
    if (sleep_timer.sleeping())
        return;
    if (resume_attempted_)
        return;
    if (!enabled)
    {
        resume_attempted_ = true;
        return;
    }
    if (!has_catalog())
        return;
    resume_attempted_ = true;
    const auto last = library_.last_channel();
    if (last.second.empty() && user_.recent_channel_ids.empty())
        return;
    const auto index =
        catalog_.Find(last.second.empty() ? user_.recent_channel_ids.front() : last.second);
    if (index == iptv::Catalog::npos || (marks_[index] & 4u) != 0 ||
        catalog_[index].playback_status == iptv::PlaybackStatus::failed)
        return;
    (void)play(static_cast<unsigned>(index));
}

void Model::mark_visibility()
{
    protected_categories_.clear();
    if (marks_.size() != catalog_.size())
        return;
    std::map<std::string, unsigned> categories;
    for (std::size_t i = 0; i < catalog_.size(); ++i)
    {
        marks_[i] &= static_cast<std::uint8_t>(~4u);
        const auto channel = catalog_[i];
        const bool allowed =
            channel.media_kind == iptv::MediaKind::live && content_allowed(channel);
        if (allowed && !parental_.unlocked())
        {
            if (!channel.group_title.empty())
                ++categories[std::string(channel.group_title)];
            for (const auto group : channel.alternate_group_titles)
                if (!group.empty() && group != channel.group_title)
                    ++categories[std::string(group)];
        }
        bool hidden = !allowed || hidden_channels_.contains(std::string(channel.id)) ||
                      (hide_failed_ && channel.playback_status == iptv::PlaybackStatus::failed);
        if (!hidden_categories_.empty())
        {
            hidden = hidden || category_hidden(channel.group_title);
            for (const auto category : channel.alternate_group_titles)
                hidden = hidden || category_hidden(category);
        }
        if (hidden)
            marks_[i] |= 4u;
    }
    for (auto &[category, count] : categories)
        protected_categories_.push_back({std::move(category), count});
}

bool Model::move_channel(std::string_view id, int direction)
{
    if (group_ == Group::recently_added || group_ == Group::most_watched)
        return false;
    if (direction != -1 && direction != 1)
        return false;
    const int from = position_of(id);
    const int to = from + direction;
    if (from < 0 || to < 0 || to >= static_cast<int>(visible_count_))
        return false;
    const bool favorites = group_ == Group::favorites;
    auto order = favorites ? favorite_order_ : channel_order_;
    std::erase_if(order,
                  [&](const auto &saved)
                  {
                      const auto at = catalog_.Find(saved);
                      return at == iptv::Catalog::npos ||
                             catalog_[at].media_kind != iptv::MediaKind::live;
                  });
    std::unordered_set<std::string> known(order.begin(), order.end());
    for (const auto at : index_.order)
        if (catalog_[at].media_kind == iptv::MediaKind::live &&
            (!favorites || is_favorite(catalog_[at])) &&
            known.insert(std::string(catalog_[at].id)).second)
            order.emplace_back(catalog_[at].id);
    const auto neighbor = catalog_[visible_[static_cast<unsigned>(to)]].id;
    const auto first = std::find(order.begin(), order.end(), id);
    const auto second = std::find(order.begin(), order.end(), neighbor);
    if (first == order.end() || second == order.end())
        return false;
    std::iter_swap(first, second);
    if (!library_.save_channel_order(source_id(active_source_), favorites, order))
        return false;
    (favorites ? favorite_order_ : channel_order_) = std::move(order);
    rebuild_visible();
    return true;
}

bool Model::reset_channel_order()
{
    if (group_ == Group::recently_added || group_ == Group::most_watched)
        return false;
    const bool favorites = group_ == Group::favorites;
    if (!library_.save_channel_order(source_id(active_source_), favorites, {}))
        return false;
    (favorites ? favorite_order_ : channel_order_).clear();
    rebuild_visible();
    return true;
}

bool Model::rename_channel(std::string_view id, std::string_view name)
{
    const auto at = catalog_.Find(id);
    if (at == iptv::Catalog::npos || !content_allowed(catalog_[at]) ||
        catalog_[at].media_kind != iptv::MediaKind::live ||
        !library_.rename_channel(source_id(active_source_), id, name))
        return false;
    channel_names_ = library_.channel_names(source_id(active_source_));
    index_.build(catalog_, channel_names_);
    rebuild_visible();
    return true;
}

bool Model::hide_channel(std::string_view id, bool hidden)
{
    const auto at = catalog_.Find(id);
    if (at == iptv::Catalog::npos || !content_allowed(catalog_[at]) ||
        catalog_[at].media_kind != iptv::MediaKind::live ||
        !library_.hide_channel(source_id(active_source_), id, hidden))
        return false;
    hidden_channels_ = library_.hidden_channels(source_id(active_source_));
    mark_visibility();
    recount_groups();
    rebuild_visible();
    return true;
}

std::vector<unsigned> Model::hidden_channels() const
{
    std::vector<unsigned> result;
    for (const auto at : index_.order)
        if (catalog_[at].media_kind == iptv::MediaKind::live &&
            hidden_channels_.contains(std::string(catalog_[at].id)) &&
            content_allowed(catalog_[at]))
            result.push_back(at);
    return result;
}

void Model::set_provider_category(std::string_view category)
{
    provider_category_ = category;
    rebuild_visible();
}

bool Model::category_hidden(std::string_view category) const
{
    for (category = category_trim(category); !category.empty();
         category = category_parent(category))
        if (hidden_categories_.contains(std::string(category)))
            return true;
    return false;
}

bool Model::hide_category(std::string_view category, bool hidden)
{
    category = category_trim(category);
    if (!hidden && category_hidden(category_parent(category)))
        return false;
    if (!library_.hide_category(source_id(active_source_), category, hidden))
        return false;
    hidden_categories_ = library_.hidden_categories(source_id(active_source_));
    mark_visibility();
    recount_groups();
    rebuild_visible();
    return true;
}

void Model::set_folder(std::string_view folder)
{
    folder_ = folder;
    folder_channels_ = library_.folder_channels(folder_);
    rebuild_visible();
}

bool Model::create_folder(std::string_view name)
{
    return library_.add_folder(name);
}

bool Model::rename_folder(std::string_view name, std::string_view replacement)
{
    if (!library_.rename_folder(name, replacement))
        return false;
    if (folder_ == name)
        set_folder(replacement);
    return true;
}

bool Model::remove_folder(std::string_view name)
{
    if (!library_.remove_folder(name))
        return false;
    if (folder_ == name)
        set_folder({});
    return true;
}

bool Model::in_folder(std::string_view folder, std::string_view channel) const
{
    return library_.folder_channels(folder).contains(std::string(channel));
}

bool Model::put_in_folder(std::string_view folder, unsigned index, bool included)
{
    if (index >= channel_count())
        return false;
    const bool previous = in_folder(folder, catalog_[index].id);
    if (!library_.put_in_folder(folder, catalog_[index].id, included))
        return false;
    if (included && !is_favorite(catalog_[index]) && toggle_favorite(index) != Starred::added)
    {
        (void)library_.put_in_folder(folder, catalog_[index].id, previous);
        return false;
    }
    if (folder_ == folder)
        set_folder(folder_);
    return true;
}
} // namespace ptv

namespace ptv
{
bool Model::import_usb_playlist(const std::string &file)
{
    if (!require_parent() || refreshing() || !usb_playlist_path(file))
        return false;
    const auto available = usb_playlists(usb_root_);
    if (std::find(available.begin(), available.end(), file) == available.end())
    {
        notify(Level::error, tr("The USB playlist is no longer available"));
        return false;
    }
    for (const auto &source : sources_)
        if (source.kind == 1 && source.url == file)
        {
            use_saved_source(source.id);
            refresh();
            return true;
        }
    SavedSource source;
    source.kind = 1;
    source.name = file.substr(file.find_last_of('/') + 1);
    if (source.name.size() > 128)
        source.name.resize(128);
    source.url = file;
    source.schedule = RefreshSchedule::manual;
    return commit_source(std::move(source));
}
} // namespace ptv
