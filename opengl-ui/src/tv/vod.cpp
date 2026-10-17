// ProsperoTV - Xtream on-demand catalogues and per-series episode lists.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/i18n.hpp"
#include "tv/vod.hpp"
#include "tv/platform.hpp"
#include "tv/catalog_index.hpp"
#include "iptv_json.h"
#include "iptv_store.h"
#include "iptv_source_state.h"
#include <algorithm>
#include <limits>
#include <charconv>
#include <cstdio>
#include <cstdlib>
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
// When the provider added a title, in Unix seconds, as a number or a string of one. Anything
// else (words, a negative or far-off time, a value too long) is no date, never a failed list.
bool date_field(JsonReader *value, std::string &text)
{
    if (value->Peek() == '{' || value->Peek() == '[')
        return value->SkipValue();
    return value->OptionalStringOrScalar(&text, 24);
}
std::uint32_t added_time(std::string_view text)
{
    if (const auto dot = text.find('.');
        dot != std::string_view::npos &&
        text.find_first_not_of("0123456789", dot + 1) == std::string_view::npos)
        text = text.substr(0, dot);
    return integer(text);
}
bool fetch(const std::string &url, std::string *reply, const iptv::http::RequestControl *control,
           const iptv::http::RequestHeaders *headers)
{
    constexpr std::size_t limit = 16u * 1024u * 1024u;
    auto buffer = std::make_unique<char[]>(limit + 1);
    const auto result =
        platform::fetch(url.c_str(), buffer.get(), limit + 1, limit, control, headers);
    if (result.status != iptv::http::Status::ok)
        return false;
    reply->assign(buffer.get(), result.bytes);
    return true;
}
bool detail_field(const std::string &key, JsonReader *value, VodDetails &details,
                  unsigned depth = 0)
{
    if (depth > 8)
        return false;
    if (key == "info" && value->Peek() == '{')
        return ReadObject(value, [&](const std::string &nested, JsonReader *item)
                          { return detail_field(nested, item, details, depth + 1); });
    std::string *target =
        key == "plot" || key == "description" ? &details.plot
        : key == "cast" || key == "actors"    ? &details.cast
        : key == "genre"                      ? &details.genre
        : key == "year" || key == "releaseDate" || key == "releasedate" || key == "release_date"
            ? &details.year
        : key == "rating" || key == "rating_5based" ? &details.rating
        : key == "movie_image" || key == "cover_big" || key == "cover" || key == "stream_icon"
            ? &details.poster
            : nullptr;
    if (!target)
        return value->SkipValue();
    std::string text;
    if (value->Peek() == '[')
    {
        if (!ReadArray(value,
                       [&](JsonReader *entry)
                       {
                           std::string part;
                           if (entry->Peek() == '{' || entry->Peek() == '[')
                               return entry->SkipValue();
                           if (!entry->StringOrScalar(&part, 256))
                               return false;
                           if (part != "null" && text.size() + part.size() + 2 <= 1024)
                           {
                               if (!text.empty())
                                   text += ", ";
                               text += part;
                           }
                           return true;
                       }))
            return false;
    }
    else
    {
        // Longer text than is kept is cut short, not refused: one show with a long cast list
        // must not stop the whole list loading. A poster address that long is no address.
        const std::size_t keep = target == &details.plot || target == &details.poster ? 4096
                                 : target == &details.cast                            ? 1024
                                                                                      : 256;
        if (!value->OptionalStringOrScalar(
                &text, target == &details.poster ? keep : std::numeric_limits<std::size_t>::max()))
            return false;
        if (text.size() > keep)
        {
            text.resize(keep);
            // At a whole character, and for the cast at a whole name.
            while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0u) == 0x80u)
                text.pop_back();
            if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xc0u)
                text.pop_back();
            if (const auto comma = text.rfind(',');
                target == &details.cast && comma != 0 && comma != std::string::npos)
                text.resize(comma);
        }
    }
    if (text == "null")
        text.clear();
    if (target == &details.year)
    {
        if (text.size() < 4 || integer(std::string_view(text).substr(0, 4)) < 1880 ||
            integer(std::string_view(text).substr(0, 4)) > 2200)
            text.clear();
        else
            text.resize(4);
    }
    if (target == &details.rating &&
        (text.size() > 16 || text.find_first_not_of("0123456789.") != std::string::npos))
        text.clear();
    if (target == &details.rating && !text.empty())
    {
        char *end = nullptr;
        const double rating = std::strtod(text.c_str(), &end);
        const bool five_based = key == "rating_5based";
        if (!end || *end != '\0' || rating < 0 || rating > (five_based ? 5 : 10))
            text.clear();
        else
        {
            if (five_based && !details.rating.empty())
                return true;
            char normalized[24];
            std::snprintf(normalized, sizeof(normalized), "%.2f", rating * (five_based ? 2 : 1));
            text = normalized;
            while (!text.empty() && text.back() == '0')
                text.pop_back();
            if (!text.empty() && text.back() == '.')
                text.pop_back();
        }
    }
    if (target == &details.poster)
    {
        std::string canonical;
        if (iptv::CanonicalizeStreamUrl(text, &canonical))
            text = std::move(canonical);
        else
            text.clear();
    }
    if (!text.empty())
        *target = std::move(text);
    return true;
}
std::string unescape(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto digit = [](char c)
        {
            return c >= '0' && c <= '9'   ? c - '0'
                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                   : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                          : -1;
        };
        if (text[i] == '%' && i + 2 < text.size() && digit(text[i + 1]) >= 0 &&
            digit(text[i + 2]) >= 0)
        {
            out += static_cast<char>(digit(text[i + 1]) * 16 + digit(text[i + 2]));
            i += 2;
        }
        else
            out += text[i];
    }
    return out;
}
// <server>/movie/<username>/<password>/<id>.<extension>: how an Xtream server's playlist
// names a film. The details come from the same server, with the same account.
bool xtream_movie_address(std::string_view url, iptv::XtreamCredentials *account, std::string *id)
{
    if (const auto query = url.find_first_of("?#"); query != std::string_view::npos)
        url = url.substr(0, query);
    const auto scheme = url.find("://");
    const auto movie = url.rfind("/movie/");
    if (scheme == std::string_view::npos || movie == std::string_view::npos || movie <= scheme + 3)
        return false;
    auto rest = url.substr(movie + 7);
    const auto user_end = rest.find('/');
    if (user_end == std::string_view::npos)
        return false;
    const auto password_end = rest.find('/', user_end + 1);
    if (password_end == std::string_view::npos)
        return false;
    auto file = rest.substr(password_end + 1);
    file = file.substr(0, file.find('.'));
    if (file.empty() || file.size() > 20 ||
        file.find_first_not_of("0123456789") != std::string_view::npos)
        return false;
    account->server_url = std::string(url.substr(0, movie));
    account->username = unescape(rest.substr(0, user_end));
    account->password = unescape(rest.substr(user_end + 1, password_end - user_end - 1));
    *id = std::string(file);
    return iptv::ValidateXtreamCredentials(*account);
}
void fill_missing(VodDetails &value, const VodDetails &fallback)
{
    if (value.plot.empty())
        value.plot = fallback.plot;
    if (value.cast.empty())
        value.cast = fallback.cast;
    if (value.genre.empty())
        value.genre = fallback.genre;
    if (value.year.empty())
        value.year = fallback.year;
    if (value.rating.empty())
        value.rating = fallback.rating;
    if (!fallback.poster.empty() && (value.poster.empty() || fallback.complete))
        value.poster = fallback.poster;
    value.complete = value.complete || fallback.complete;
}
std::size_t detail_bytes(const VodDetails &value)
{
    return value.plot.size() + value.cast.size() + value.genre.size() + value.year.size() +
           value.rating.size() + value.poster.size();
}
struct Parser
{
    const iptv::XtreamCredentials &account;
    VodKind kind;
    iptv::Catalog &catalog;
    std::unordered_map<std::string, iptv::XtreamCategory> categories;
    std::unordered_set<std::string> ids;
    std::size_t text_bytes = 0;
    bool full = false;
    VodMetadata *metadata = nullptr;
    std::size_t metadata_bytes = 0;
    VodDetails series_details{};
    bool item(JsonReader *reader, std::string_view season = {})
    {
        std::string id, name, logo, category, extension, episode, season_number(season), adult;
        std::string added, modified;
        VodDetails details;
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
                                return value->OptionalStringOrScalar(&logo,
                                                                     iptv::kDefaultMaxUrlBytes);
                            if (key == "is_adult")
                                return value->StringOrScalar(&adult, 8);
                            if (key == "category_id")
                                return value->StringOrScalar(&category, 64);
                            if (key == "container_extension")
                                return value->StringOrScalar(&extension, 12);
                            if (key == "episode_num")
                                return value->StringOrScalar(&episode, 12);
                            if (key == "season")
                                return value->StringOrScalar(&season_number, 12);
                            if (key == "added" || key == "last_modified")
                                return date_field(value, key == "added" ? added : modified);
                            return detail_field(key, value, details);
                        }))
            return false;
        if (id.empty() || id == "null" || name.empty() || name == "null")
            return true;
        if (extension == "null")
            extension.clear();
        iptv::Channel item;
        item.source_id = catalog.source_id;
        item.media_kind =
            kind == VodKind::movies ? iptv::MediaKind::movie : iptv::MediaKind::episode;
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
        item.group_title = group == categories.end() ? "Other" : group->second.name;
        item.adult =
            adult == "1" || adult == "true" || (group != categories.end() && group->second.adult);
        if (kind == VodKind::episodes)
        {
            const auto season_id = std::min(999u, integer(season_number));
            const auto episode_id = std::min(99999u, integer(episode));
            item.group_title = "Season " + std::to_string(season_id);
            item.source_line = season_id * 100000u + episode_id;
            if (episode_id)
                item.name = "Episode " + std::to_string(episode_id) + " · " + item.name;
        }
        else
        {
            // A show's list entry changes when episodes are added to it; a film has only `added`.
            const auto changed = kind == VodKind::series ? added_time(modified) : 0u;
            item.source_line = changed ? changed : added_time(added);
        }
        const auto bytes = item.id.size() + item.name.size() + item.url.size() +
                           item.tvg_logo.size() + item.group_title.size();
        if (catalog.size() >= kLimit || bytes > 96u * 1024u * 1024u - text_bytes)
        {
            full = true;
            return false;
        }
        text_bytes += bytes;
        fill_missing(details, series_details);
        if (details.poster.empty())
            details.poster = item.tvg_logo;
        if (metadata)
        {
            const auto size = detail_bytes(details) + item.id.size();
            if (size <= 32u * 1024u * 1024u - metadata_bytes)
            {
                metadata_bytes += size;
                metadata->emplace(item.id, std::move(details));
            }
        }
        return catalog.Add(item);
    }
};
} // namespace

bool load_vod(const iptv::XtreamCredentials &account, VodKind kind, std::string_view series,
              iptv::Catalog *catalog, const iptv::http::RequestControl *control, std::string *error,
              VodMetadata *metadata, const iptv::http::RequestHeaders *headers)
{
    if (metadata)
        metadata->clear();
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
    parser.metadata = metadata;
    std::string url, reply;
    bool ok = false;
    if (kind == VodKind::episodes)
    {
        if (iptv::BuildXtreamSeriesUrl(account, series, &url) &&
            fetch(url, &reply, control, headers))
        {
            JsonReader reader(reply);
            bool found = false;
            ok = ReadObject(
                     &reader,
                     [&](const std::string &key, JsonReader *value)
                     {
                         if (key == "info" && value->Peek() == '{')
                             return ReadObject(
                                 value, [&](const std::string &field, JsonReader *part)
                                 { return detail_field(field, part, parser.series_details); });
                         if (key != "episodes")
                             return value->SkipValue();
                         found = true;
                         if (value->Peek() == '[')
                         {
                             unsigned season = 0;
                             return ReadArray(value,
                                              [&](JsonReader *episode)
                                              {
                                                  const auto number = std::to_string(season++);
                                                  if (episode->Peek() == '[')
                                                      return ReadArray(
                                                          episode, [&](JsonReader *item)
                                                          { return parser.item(item, number); });
                                                  return parser.item(episode);
                                              });
                         }
                         return ReadObject(value,
                                           [&](const std::string &season, JsonReader *episodes) {
                                               return ReadArray(
                                                   episodes, [&](JsonReader *episode)
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
        if (fetch(url, &reply, control, headers) &&
            iptv::ParseXtreamCategories(reply, &categories) == iptv::XtreamStatus::ok)
        {
            for (const auto &category : categories)
                parser.categories.emplace(category.id, category);
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
            const auto response = platform::fetch_list(url.c_str(), sink, iptv::http::kMaxListBytes,
                                                       control, headers);
            ok = response.status == iptv::http::Status::ok && splitter.complete() &&
                 splitter.found();
        }
    }
    if (ok && metadata && kind == VodKind::episodes)
    {
        std::size_t bytes = 0;
        for (auto it = metadata->begin(); it != metadata->end();)
        {
            fill_missing(it->second, parser.series_details);
            const auto size = it->first.size() + detail_bytes(it->second);
            if (size > 32u * 1024u * 1024u - bytes)
                it = metadata->erase(it);
            else
            {
                bytes += size;
                ++it;
            }
        }
    }
    if (control && control->cancelled && control->cancelled(control->context))
        ok = false;
    if (!ok)
    {
        catalog->Clear();
        if (metadata)
            metadata->clear();
        *error = parser.full ? tr("This on-demand list is too large to load.")
                             : tr("The provider's on-demand list could not be downloaded.");
    }
    return ok;
}

std::uint32_t VodLibrary::added(unsigned index) const
{
    // A playlist row's source_line is where it was in the file, and an episode's its number.
    return playlist_ || kind_ == VodKind::episodes || index >= catalog_.size()
               ? 0
               : catalog_[index].source_line;
}
const VodDetails &VodLibrary::details(std::string_view id) const
{
    static const VodDetails empty;
    const auto found = metadata_.find(std::string(id));
    return found == metadata_.end() ? empty : found->second;
}
void VodLibrary::request_details(unsigned index)
{
    if (kind_ != VodKind::movies || index >= catalog_.size())
        return;
    const auto item = catalog_[index];
    if (details(item.id).complete)
        return;
    // Kept until nothing else downloads: asked for during a list update or the guide's, the
    // details used to be dropped and the panel said the provider gave none.
    iptv::XtreamCredentials account = account_;
    std::string provider_id(item.tvg_id);
    if (playlist_ && !xtream_movie_address(item.url, &account, &provider_id))
        return; // a playlist from anywhere else has no details to ask for
    wanted_id_ = item.id;
    wanted_provider_id_ = std::move(provider_id);
    wanted_account_ = std::move(account);
    status_ = tr("Loading title details...");
    ++revision_;
}
bool VodLibrary::details_loading(std::string_view id) const
{
    return !id.empty() && (id == wanted_id_ || (thread_ && id == detail_id_));
}

VodLibrary::~VodLibrary()
{
    stop();
}
void VodLibrary::configure(iptv::XtreamCredentials account, std::string cache,
                           RefreshSchedule schedule, std::string user_agent, std::string referrer,
                           std::string extra_headers)
{
    schedule_ = schedule;
    if (!playlist_ && account.server_url == account_.server_url &&
        account.username == account_.username && account.password == account_.password &&
        cache == cache_ && user_agent == user_agent_ && referrer == referrer_ &&
        extra_headers == extra_headers_)
        return;
    stop();
    playlist_ = nullptr;
    account_ = std::move(account);
    user_agent_ = std::move(user_agent);
    referrer_ = std::move(referrer);
    extra_headers_ = std::move(extra_headers);
    cache_ = std::move(cache);
    key_.clear();
    catalog_.Clear();
    metadata_.clear();
    detail_id_.clear();
    east_asian_ = korean_ = false;
    loaded_ = false;
    status_ =
        available()
            ? tr("Choose Movies or TV shows.")
            : tr("Choose a playlist or Xtream account in Sources to browse on-demand titles.");
    ++revision_;
}
void VodLibrary::configure_like(const VodLibrary &source, bool reload)
{
    if (reload)
    {
        stop();
        key_.clear(); // Another browser may have refreshed the shared disk cache.
    }
    configure(source.account_, source.cache_, source.schedule_, source.user_agent_,
              source.referrer_, source.extra_headers_);
    if (source.playlist_)
        configure_playlist(source.playlist_);
}
std::size_t VodLibrary::find_series(std::string_view series, std::uint64_t source) const
{
    if (!loaded_ || kind_ != VodKind::series || series.empty() || catalog_.source_id != source)
        return iptv::Catalog::npos;
    if (!playlist_)
    {
        const auto at = catalog_.Find(identifier(source, VodKind::series, series));
        return at != iptv::Catalog::npos && catalog_[at].source_id == source &&
                       catalog_[at].tvg_id == series
                   ? at
                   : iptv::Catalog::npos;
    }
    // Playlist show rows keep the representative episode ID, not an Xtream ID.
    for (unsigned at = 0; at < catalog_.size(); ++at)
        if (catalog_[at].source_id == source && catalog_[at].tvg_id == series)
            return at;
    return iptv::Catalog::npos;
}
void VodLibrary::configure_playlist(const iptv::Catalog *playlist)
{
    stop();
    playlist_ = playlist;
    account_ = {};
    key_.clear();
    catalog_.Clear();
    metadata_.clear();
    detail_id_.clear();
    loaded_ = false;
    east_asian_ = korean_ = false;
    status_ = tr("Choose Movies or TV shows.");
    ++revision_;
}
void VodLibrary::playlist_updated()
{
    if (!playlist_ || key_.empty())
        return;
    const auto kind = kind_;
    const auto series = series_;
    key_.clear();
    select(kind, series);
}

// Providers commonly name episodes "Show S01E02"; unidentified episodes remain
// individually selectable instead of guessing a show from a broad category.
static std::string playlist_show(std::string_view name)
{
    for (std::size_t i = 1; i + 3 < name.size(); ++i)
    {
        if ((name[i] != 'S' && name[i] != 's') ||
            (name[i - 1] != ' ' && name[i - 1] != '.' && name[i - 1] != '-'))
            continue;
        auto end = i + 1;
        while (end < name.size() && name[end] >= '0' && name[end] <= '9')
            ++end;
        if (end == i + 1 || end + 1 >= name.size() || (name[end] != 'E' && name[end] != 'e') ||
            name[end + 1] < '0' || name[end + 1] > '9')
            continue;
        auto cut = i;
        while (cut && (name[cut - 1] == ' ' || name[cut - 1] == '.' || name[cut - 1] == '-'))
            --cut;
        if (cut)
            return std::string(name.substr(0, cut));
    }
    return std::string(name);
}
void VodLibrary::playlist_counts(const iptv::Catalog &playlist, unsigned *movies, unsigned *shows)
{
    unsigned films = 0;
    std::unordered_set<std::string> series;
    for (std::size_t i = 0; i < playlist.size(); ++i)
    {
        const auto entry = playlist[i];
        if (entry.media_kind == iptv::MediaKind::movie)
            ++films;
        else if (entry.media_kind == iptv::MediaKind::episode)
            series.insert(std::string(entry.group_title) + "\n" + playlist_show(entry.name));
    }
    *movies = films;
    *shows = static_cast<unsigned>(series.size());
}
// The provider ID never becomes a path component.
static std::string vod_file(const std::string &cache, VodKind kind, const std::string &series)
{
    return cache + ".vod-" + std::to_string(static_cast<int>(kind)) + "-" +
           std::to_string(iptv::CustomSourceId(series)) + ".sqlite3";
}
std::string VodLibrary::file() const
{
    return vod_file(cache_, kind_, series_);
}
std::string VodLibrary::cache_file(VodKind kind) const
{
    return vod_file(cache_, kind, {});
}
void VodLibrary::note_scripts()
{
    east_asian_ = korean_ = false;
    for (std::size_t i = 0; i < catalog_.size() && (!east_asian_ || !korean_); ++i)
    {
        ptv::note_scripts(catalog_[i].name, &east_asian_, &korean_);
        ptv::note_scripts(catalog_[i].group_title, &east_asian_, &korean_);
        const auto &detail = details(catalog_[i].id);
        ptv::note_scripts(detail.plot, &east_asian_, &korean_);
        ptv::note_scripts(detail.cast, &east_asian_, &korean_);
        ptv::note_scripts(detail.genre, &east_asian_, &korean_);
    }
}
void VodLibrary::select(VodKind kind, std::string series, bool force)
{
    if (!available())
        return;
    const auto key = std::to_string(static_cast<int>(kind)) + ":" + series;
    if (playlist_)
    {
        if (key == key_ && !force)
            return;
        kind_ = kind;
        series_ = std::move(series);
        key_ = key;
        catalog_.Clear();
        metadata_.clear();
        catalog_.source_id = playlist_->source_id;
        std::unordered_set<std::string> shows;
        for (std::size_t i = 0; i < playlist_->size(); ++i)
        {
            const auto entry = (*playlist_)[i];
            if (entry.media_kind !=
                (kind == VodKind::movies ? iptv::MediaKind::movie : iptv::MediaKind::episode))
                continue;
            auto item = entry.Copy();
            if (kind != VodKind::movies)
            {
                const auto show = playlist_show(entry.name);
                // Keep identically named shows from separate provider categories apart.
                const auto show_id = std::string(entry.group_title) + "\n" + show;
                if (kind == VodKind::episodes && show_id != series_)
                    continue;
                if (kind == VodKind::series)
                {
                    if (!shows.insert(show_id).second)
                        continue;
                    item.tvg_id = show_id;
                    item.name = show;
                }
            }
            catalog_.Add(item);
        }
        loaded_ = true;
        note_scripts();
        status_ = catalog_.empty() ? tr("The playlist has no titles in this list.")
                                   : tr("On-demand titles from the playlist");
        ++revision_;
        return;
    }
    if (key == key_ && busy())
        return;
    if (key != key_)
    {
        stop();
        kind_ = kind;
        series_ = std::move(series);
        key_ = key;
        iptv::StoreReport report;
        loaded_ = iptv::LoadCatalog(file(), &catalog_, {.allow_empty = true}, &report) ==
                      iptv::StoreStatus::ok &&
                  report.catalog_version >= 5 &&
                  catalog_.source_id == iptv::XtreamSourceId(account_);
        if (!loaded_)
            catalog_.Clear();
        metadata_ = loaded_ ? load_vod_metadata(file(), catalog_) : VodMetadata{};
        saved_ = loaded_ ? report.saved_unix : 0;
        note_scripts();
        ++revision_;
    }
    if (force)
    {
        detail_id_.clear();
        detail_provider_id_.clear();
    }
    if (force || !loaded_ || refresh_due(schedule_, saved_, platform::unix_time()))
        requested_ = true;
    status_ = requested_
                  ? (loaded_ ? tr("Updating the saved list…") : tr("Loading the provider's list…"))
              : catalog_.empty() ? tr("The provider has no titles in this list.")
                                 : tr("Saved on-demand list");
}
void VodLibrary::poll(bool can_download)
{
    if (thread_ && done_.load())
    {
        platform::thread_join(thread_);
        thread_ = nullptr;
        if (ok_)
        {
            metadata_ = std::move(pending_metadata_);
            if (detail_id_.empty())
                catalog_ = std::move(pending_);
            loaded_ = true;
            note_scripts();
            if (detail_id_.empty())
                saved_ = platform::unix_time();
            status_ = catalog_.empty() ? tr("The provider has no titles in this list.")
                      : saved_ok_      ? tr("On-demand list up to date")
                                       : tr("List loaded, but it could not be saved.");
        }
        else
            status_ = error_ + (loaded_ ? tr(" Showing the saved titles.") : "");
        pending_.Clear();
        pending_metadata_.clear();
        detail_id_.clear();
        detail_provider_id_.clear();
        ++revision_;
    }
    if (thread_)
        return;
    // One title's details are a small answer: they go ahead of a list and do not wait for
    // the guide.
    if (!wanted_id_.empty())
    {
        if (details(wanted_id_).complete)
            wanted_id_.clear();
        else
        {
            detail_id_ = std::move(wanted_id_);
            detail_provider_id_ = std::move(wanted_provider_id_);
            detail_account_ = std::move(wanted_account_);
            wanted_id_.clear();
            start();
            return;
        }
    }
    if (!can_download || !requested_)
        return;
    requested_ = false;
    start();
}
void VodLibrary::start()
{
    ok_ = saved_ok_ = false;
    stop_.store(false);
    done_.store(false);
    thread_ = platform::thread_start(work, this, 2u * 1024u * 1024u, "ptv-vod");
    if (!thread_)
    {
        detail_id_.clear();
        detail_provider_id_.clear();
        status_ = tr("The on-demand download could not start.");
        ++revision_;
    }
}
void VodLibrary::stop()
{
    requested_ = false;
    wanted_id_.clear();
    if (!thread_)
    {
        detail_id_.clear();
        detail_provider_id_.clear();
        return;
    }
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
        iptv::http::RequestHeaders headers{v.user_agent_.c_str(), v.referrer_.c_str()};
        headers.extra_headers = v.extra_headers_.c_str();
        headers.credential_origin = v.account_.server_url.c_str();
        if (!v.detail_id_.empty())
        {
            v.pending_metadata_ = v.metadata_;
            std::string url, reply;
            iptv::BuildXtreamApiUrl(v.detail_account_, "get_vod_info", &url);
            headers.credential_origin = v.detail_account_.server_url.c_str();
            url += "&vod_id=";
            constexpr char hex[] = "0123456789ABCDEF";
            for (const unsigned char c : v.detail_provider_id_)
            {
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '-' || c == '_')
                    url += c;
                else
                {
                    url += '%';
                    url += hex[c >> 4];
                    url += hex[c & 15];
                }
            }
            VodDetails detail = v.details(v.detail_id_);
            if (fetch(url, &reply, &control, &headers))
            {
                JsonReader reader(reply);
                bool found = false;
                v.ok_ = ReadObject(
                            &reader,
                            [&](const std::string &key, JsonReader *value)
                            {
                                if ((key == "info" || key == "movie_data") && value->Peek() == '{')
                                {
                                    found = true;
                                    return ReadObject(
                                        value, [&](const std::string &field, JsonReader *part)
                                        { return detail_field(field, part, detail); });
                                }
                                return value->SkipValue();
                            }) &&
                        reader.Finished() && found && !v.stop_.load();
            }
            if (v.ok_)
            {
                detail.complete = true;
                v.pending_metadata_[v.detail_id_] = std::move(detail);
                std::size_t bytes = 0;
                for (const auto &[id, value] : v.pending_metadata_)
                    bytes += id.size() + detail_bytes(value);
                if (bytes > 32u * 1024u * 1024u)
                {
                    v.ok_ = false;
                    v.error_ = tr("The provider metadata cache is full.");
                }
                else // a playlist's titles are rebuilt from it: their details stay in memory
                    v.saved_ok_ = v.playlist_ || save_vod_metadata(v.file(), v.pending_metadata_);
            }
            else
                v.error_ = tr("Title details could not be downloaded.");
        }
        else
        {
            v.ok_ = load_vod(v.account_, v.kind_, v.series_, &v.pending_, &control, &v.error_,
                             &v.pending_metadata_, &headers);
            if (v.ok_)
            {
                std::size_t bytes = 0;
                for (auto it = v.pending_metadata_.begin(); it != v.pending_metadata_.end();)
                {
                    fill_missing(it->second, v.details(it->first));
                    const auto size = it->first.size() + detail_bytes(it->second);
                    if (size > 32u * 1024u * 1024u - bytes)
                        it = v.pending_metadata_.erase(it);
                    else
                    {
                        bytes += size;
                        ++it;
                    }
                }
                v.saved_ok_ = iptv::SaveCatalog(v.file(), v.pending_, {.allow_empty = true}) ==
                                  iptv::StoreStatus::ok &&
                              save_vod_metadata(v.file(), v.pending_metadata_);
            }
        }
        platform::network_shutdown();
    }
    else
        v.error_ = "The on-demand connection could not start.";
    v.done_.store(true);
    return nullptr;
}
} // namespace ptv
