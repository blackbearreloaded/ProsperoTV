// ProsperoTV - Xtream on-demand catalogues and per-series episode lists.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/vod.hpp"
#include "tv/platform.hpp"
#include "tv/catalog_index.hpp"
#include "iptv_json.h"
#include "iptv_store.h"
#include "iptv_source_state.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace ptv
{
namespace
{
using namespace iptv::json;
constexpr std::size_t kLimit = 100000;
std::string identifier(std::uint64_t source, VodKind kind, std::string_view id)
{
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix),
                  "vod:%016llx:%d:", static_cast<unsigned long long>(source),
                  static_cast<int>(kind));
    return prefix + std::string(id);
}
unsigned integer(std::string_view text)
{
    unsigned out = 0;
    const auto value = std::from_chars(text.data(), text.data() + text.size(), out);
    return value.ec == std::errc{} && value.ptr == text.data() + text.size() ? out : 0;
}
bool fetch(const std::string &url, std::string *reply, const iptv::http::RequestControl *control)
{
    constexpr std::size_t limit = 16u * 1024u * 1024u;
    auto buffer = std::make_unique<char[]>(limit + 1);
    const auto result = platform::fetch(url.c_str(), buffer.get(), limit + 1, limit, control);
    if (result.status != iptv::http::Status::ok)
        return false;
    reply->assign(buffer.get(), result.bytes);
    return true;
}
struct Parser
{
    const iptv::XtreamCredentials &account;
    VodKind kind;
    iptv::Catalog &catalog;
    std::unordered_map<std::string, std::string> categories;
    std::unordered_set<std::string> ids;
    std::size_t text_bytes = 0;
    bool full = false;
    bool item(JsonReader *reader, std::string_view season = {})
    {
        std::string id, name, logo, category, extension, episode, season_number(season);
        if (!ReadObject(reader,
                        [&](const std::string &key, JsonReader *value)
                        {
                            if (key == (kind == VodKind::series     ? "series_id"
                                        : kind == VodKind::episodes ? "id"
                                                                    : "stream_id"))
                                return value->StringOrScalar(&id, 64);
                            if (key == "name" || key == "title")
                                return value->StringOrScalar(&name);
                            if (key == "stream_icon" || key == "cover")
                                return value->StringOrScalar(&logo, iptv::kDefaultMaxUrlBytes);
                            if (key == "category_id")
                                return value->StringOrScalar(&category, 64);
                            if (key == "container_extension")
                                return value->StringOrScalar(&extension, 12);
                            if (key == "episode_num")
                                return value->StringOrScalar(&episode, 12);
                            if (key == "season")
                                return value->StringOrScalar(&season_number, 12);
                            return value->SkipValue();
                        }))
            return false;
        if (id.empty() || id == "null" || name.empty() || name == "null")
            return true;
        if (extension == "null")
            extension.clear();
        iptv::Channel item;
        item.source_id = catalog.source_id;
        item.id = identifier(item.source_id, kind, id);
        item.tvg_id = id;
        item.name = std::move(name);
        if (kind == VodKind::series)
        {
            if (!iptv::BuildXtreamSeriesUrl(account, id, &item.url))
                return true;
        }
        else if (!iptv::BuildXtreamMediaUrl(account, kind == VodKind::episodes, id, extension,
                                            &item.url))
            return true;
        if (!ids.insert(id).second)
            return true;
        std::string canonical;
        if (iptv::CanonicalizeStreamUrl(logo, &canonical))
            item.tvg_logo = std::move(canonical);
        const auto group = categories.find(category);
        item.group_title = group == categories.end() ? "Other" : group->second;
        if (kind == VodKind::episodes)
        {
            const auto season_id = std::min(999u, integer(season_number));
            const auto episode_id = std::min(99999u, integer(episode));
            item.group_title = "Season " + std::to_string(season_id);
            item.source_line = season_id * 100000u + episode_id;
            if (episode_id)
                item.name = "Episode " + std::to_string(episode_id) + " · " + item.name;
        }
        const auto bytes = item.id.size() + item.name.size() + item.url.size() +
                           item.tvg_logo.size() + item.group_title.size();
        if (catalog.size() >= kLimit || bytes > 96u * 1024u * 1024u - text_bytes)
        {
            full = true;
            return false;
        }
        text_bytes += bytes;
        return catalog.Add(item);
    }
};
} // namespace

bool load_vod(const iptv::XtreamCredentials &account, VodKind kind, std::string_view series,
              iptv::Catalog *catalog, const iptv::http::RequestControl *control, std::string *error)
{
    if (!catalog || !error)
        return false;
    catalog->Clear();
    error->clear();
    if (!iptv::ValidateXtreamCredentials(account))
    {
        *error = "Set up an Xtream account in Sources.";
        return false;
    }
    catalog->source_id = iptv::XtreamSourceId(account);
    Parser parser{account, kind, *catalog, {}, {}};
    std::string url, reply;
    bool ok = false;
    if (kind == VodKind::episodes)
    {
        if (iptv::BuildXtreamSeriesUrl(account, series, &url) && fetch(url, &reply, control))
        {
            JsonReader reader(reply);
            bool found = false;
            ok = ReadObject(&reader,
                            [&](const std::string &key, JsonReader *value)
                            {
                                if (key != "episodes")
                                    return value->SkipValue();
                                found = true;
                                if (value->Peek() == '[')
                                    return ReadArray(value, [&](JsonReader *episode)
                                                     { return parser.item(episode); });
                                return ReadObject(
                                    value,
                                    [&](const std::string &season, JsonReader *episodes) {
                                        return ReadArray(episodes, [&](JsonReader *episode)
                                                         { return parser.item(episode, season); });
                                    });
                            }) &&
                 reader.Finished() && found;
        }
    }
    else
    {
        std::vector<iptv::XtreamCategory> categories;
        iptv::BuildXtreamApiUrl(
            account, kind == VodKind::movies ? "get_vod_categories" : "get_series_categories",
            &url);
        if (fetch(url, &reply, control) &&
            iptv::ParseXtreamCategories(reply, &categories) == iptv::XtreamStatus::ok)
        {
            for (const auto &category : categories)
                parser.categories.emplace(category.id, category.name);
            iptv::BuildXtreamApiUrl(
                account, kind == VodKind::movies ? "get_vod_streams" : "get_series", &url);
            ListSplitter splitter;
            struct State
            {
                ListSplitter &splitter;
                Parser &parser;
            } state{splitter, parser};
            const iptv::http::ListSink sink{
                [](void *self, const char *bytes, std::size_t count)
                {
                    auto &s = *static_cast<State *>(self);
                    return s.splitter.Feed({bytes, count},
                                           [&](std::string_view item)
                                           {
                                               JsonReader reader(item);
                                               return s.parser.item(&reader) && reader.Finished();
                                           });
                },
                &state};
            const auto response =
                platform::fetch_list(url.c_str(), sink, iptv::http::kMaxListBytes, control);
            ok = response.status == iptv::http::Status::ok && splitter.complete() &&
                 splitter.found();
        }
    }
    if (control && control->cancelled && control->cancelled(control->context))
        ok = false;
    if (!ok)
    {
        catalog->Clear();
        *error = parser.full ? "This on-demand list is too large to load."
                             : "The provider's on-demand list could not be downloaded.";
    }
    return ok;
}

VodLibrary::~VodLibrary()
{
    stop();
}
void VodLibrary::configure(iptv::XtreamCredentials account, std::string cache,
                           RefreshSchedule schedule)
{
    schedule_ = schedule;
    if (account.server_url == account_.server_url && account.username == account_.username &&
        account.password == account_.password && cache == cache_)
        return;
    stop();
    account_ = std::move(account);
    cache_ = std::move(cache);
    key_.clear();
    catalog_.Clear();
    east_asian_ = korean_ = false;
    loaded_ = false;
    status_ = available()
                  ? "Choose Movies or TV shows."
                  : "Choose an Xtream account in Sources to browse its movies and TV shows.";
    ++revision_;
}
std::string VodLibrary::file() const
{
    // The provider ID never becomes a path component.
    return cache_ + ".vod-" + std::to_string(static_cast<int>(kind_)) + "-" +
           std::to_string(iptv::CustomSourceId(series_)) + ".sqlite3";
}
void VodLibrary::note_scripts()
{
    east_asian_ = korean_ = false;
    for (std::size_t i = 0; i < catalog_.size() && (!east_asian_ || !korean_); ++i)
    {
        ptv::note_scripts(catalog_[i].name, &east_asian_, &korean_);
        ptv::note_scripts(catalog_[i].group_title, &east_asian_, &korean_);
    }
}
void VodLibrary::select(VodKind kind, std::string series, bool force)
{
    if (!available())
        return;
    const auto key = std::to_string(static_cast<int>(kind)) + ":" + series;
    if (key == key_ && busy())
        return;
    if (key != key_)
    {
        stop();
        kind_ = kind;
        series_ = std::move(series);
        key_ = key;
        iptv::StoreReport report;
        loaded_ = iptv::LoadCatalog(file(), &catalog_, {}, &report) == iptv::StoreStatus::ok &&
                  catalog_.source_id == iptv::XtreamSourceId(account_);
        if (!loaded_)
            catalog_.Clear();
        saved_ = loaded_ ? report.saved_unix : 0;
        note_scripts();
        ++revision_;
    }
    if (force || !loaded_ || refresh_due(schedule_, saved_, platform::unix_time()))
        requested_ = true;
    status_ = requested_ ? (loaded_ ? "Updating the saved list…" : "Loading the provider's list…")
              : catalog_.empty() ? "The provider has no titles in this list."
                                 : "Saved on-demand list";
}
void VodLibrary::poll(bool can_download)
{
    if (thread_ && done_.load())
    {
        platform::thread_join(thread_);
        thread_ = nullptr;
        if (ok_)
        {
            catalog_ = std::move(pending_);
            loaded_ = true;
            note_scripts();
            saved_ = platform::unix_time();
            status_ = catalog_.empty() ? "The provider has no titles in this list."
                      : saved_ok_      ? "On-demand list up to date"
                                       : "List loaded, but it could not be saved.";
        }
        else
            status_ = error_ + (loaded_ ? " Showing the saved titles." : "");
        pending_.Clear();
        ++revision_;
    }
    if (!can_download || thread_ || !requested_)
        return;
    requested_ = false;
    ok_ = saved_ok_ = false;
    stop_.store(false);
    done_.store(false);
    thread_ = platform::thread_start(work, this, 2u * 1024u * 1024u, "ptv-vod");
    if (!thread_)
    {
        status_ = "The on-demand download could not start.";
        ++revision_;
    }
}
void VodLibrary::stop()
{
    requested_ = false;
    if (!thread_)
        return;
    stop_.store(true);
    while (!done_.load())
    {
        platform::network_cancel();
        platform::sleep_ms(5);
    }
    poll(false);
}
void *VodLibrary::work(void *self)
{
    auto &v = *static_cast<VodLibrary *>(self);
    const iptv::http::RequestControl control{
        [](void *self) { return static_cast<VodLibrary *>(self)->stop_.load(); }, &v};
    if (platform::network_init() == iptv::http::Status::ok)
    {
        v.ok_ = load_vod(v.account_, v.kind_, v.series_, &v.pending_, &control, &v.error_);
        if (v.ok_)
            v.saved_ok_ = iptv::SaveCatalog(v.file(), v.pending_) == iptv::StoreStatus::ok;
        platform::network_shutdown();
    }
    else
        v.error_ = "The on-demand connection could not start.";
    v.done_.store(true);
    return nullptr;
}
} // namespace ptv
