// ProsperoTV - Source selection and personal browsing lists.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/model.hpp"
#include "tv/category_path.hpp"
#include "tv/platform.hpp"
#include "tv/settings.hpp"
#include "tv/local_tv.hpp"
#include "iptv_ime.h"

#include <algorithm>

namespace ptv
{
void Model::load_library()
{
    selected_source_id_ = static_cast<int>(active_source_) + 1;
    const auto settings = load_settings(data_dir_);
    hide_failed_ = settings.hide_failed;
    if (!library_.open(path("prosperotv-library.sqlite3")))
    {
        notify(Level::warning, "Library settings could not be opened");
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
            notify(Level::warning, "A source could not be imported");
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
    const bool changed = selected_source_id_ != source.id;
    selected_source_id_ = source.id;
    active_source_ = static_cast<iptv::SourceKind>(source.kind);
    schedule_ = source.schedule;
    if (source.kind == 1)
        custom_url_ = source.url;
    if (source.kind == 2)
        xtream_ = {source.url, source.username, source.password};
    if (source.kind == 3)
        portal_ = {source.url, source.mac};
    if (source.kind >= 4)
        local_source_ = source;
    vod_.configure(source.kind == 2 ? xtream_ : iptv::XtreamCredentials{},
                   cache_path(active_source_), schedule_);
    if (changed)
    {
        view.vod_kind = -1;
        view.vod_focus = 0;
        view.vod_category.clear();
        view.vod_series.clear();
        view.vod_series_name.clear();
        view.vod_series_cover.clear();
        view.vod_query.clear();
        view.vod_all = false;
    }
}

bool Model::refresh_needed() const
{
    if (catalog_loaded_ && schedule_ == RefreshSchedule::manual)
        return false;
    return !catalog_loaded_ || refresh_due(schedule_, saved_unix_, platform::unix_time());
}

void Model::use_saved_source(std::int64_t id)
{
    if (refreshing())
    {
        notify(Level::warning, "An update is running",
               "Wait for it to finish, then choose a source.");
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
        notify(Level::error, "The source choice could not be saved");
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
    set_source_text(source->name, catalog_loaded_ ? "Showing the saved channel list."
                                                  : "Downloading the first copy.");
    if (refresh_needed())
        refresh();
    else
        set_status("Saved copy", Level::ready);
}

void Model::add_source(iptv::SourceKind kind)
{
    if (kind == iptv::SourceKind::BuiltIn || refreshing() || !keyboard_ready_)
        return;
    if (kind == iptv::SourceKind::Custom)
        iptv_ime_request_prompt("", "Playlist address", "http(s)://host/playlist.m3u",
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
    const auto *source = saved_source(id);
    if (source == nullptr || source->kind == 0 || source->kind > 5 || refreshing() ||
        !keyboard_ready_)
        return;
    const auto copy = *source;
    if (copy.kind == 1)
        iptv_ime_request_prompt(copy.url.c_str(), "Playlist address", "http(s)://host/playlist.m3u",
                                IPTV_IME_BUFFER_CHARACTERS, &Model::on_custom_url, this);
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
        model.notify(Level::error, "That TV server address cannot be used",
                     "Enter its HTTP or HTTPS address, including its port when needed.");
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
        notify(Level::error, "The TV server details could not be saved",
               "Check the address and account details.");
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

bool Model::commit_source(SavedSource source)
{
    if (!library_.save_source(&source))
    {
        notify(Level::error, "The source could not be saved");
        return false;
    }
    sources_ = library_.sources();
    // Editing the selected source must discard the old source's in-memory catalog.
    if (source.id == selected_source_id_)
    {
        select_source_record(source);
        load_cache();
    }
    use_saved_source(source.id);
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
        model.notify(Level::error, "That portal address cannot be used",
                     "Enter the provider's HTTP or HTTPS portal address.");
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
        model.notify(Level::error, "That MAC code cannot be used",
                     "Use the six pairs supplied by the provider, such as 00:1A:79:12:34:56.");
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
    if (id <= 4 || refreshing() || !library_.remove_source(id))
        return false;
    sources_ = library_.sources();
    if (selected_source_id_ == id)
        use_saved_source(1);
    return true;
}

bool Model::set_schedule(std::int64_t id, RefreshSchedule schedule)
{
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
                       cache_path(active_source_), schedule_);
    }
    ++revision_;
    return true;
}

bool Model::play_vod(unsigned index)
{
    if (vod_.kind() == VodKind::series || index >= vod_.catalog().size())
        return false;
    const auto item = vod_.catalog()[index];
    play_request_ = {};
    play_request_.channel_id = item.id;
    play_request_.channel_name = item.name;
    play_request_.urls = {std::string(item.url)};
    play_request_.source_id = item.source_id;
    play_request_.record_channel_result = false;
    play_requested_ = true;
    return true;
}
bool Model::ask_vod_query()
{
    if (!keyboard_ready_)
        return false;
    iptv_ime_request_prompt(view.vod_query.c_str(), "Search on demand",
                            "Movie, show or episode title", IPTV_IME_BUFFER_CHARACTERS,
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

void Model::resume_last(bool enabled)
{
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
    if (marks_.size() != catalog_.size())
        return;
    for (std::size_t i = 0; i < catalog_.size(); ++i)
    {
        marks_[i] &= static_cast<std::uint8_t>(~4u);
        const auto channel = catalog_[i];
        bool hidden = hide_failed_ && channel.playback_status == iptv::PlaybackStatus::failed;
        if (!hidden_categories_.empty())
        {
            hidden = hidden || category_hidden(channel.group_title);
            for (const auto category : channel.alternate_group_titles)
                hidden = hidden || category_hidden(category);
        }
        if (hidden)
            marks_[i] |= 4u;
    }
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
