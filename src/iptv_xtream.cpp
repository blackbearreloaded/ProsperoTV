/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_xtream.h"
#include "iptv_json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace iptv
{
namespace
{

constexpr char kCredentialsMagic[] = "PROSPEROTV-XTREAM-1";
using json::JsonReader;
using json::ListSplitter;
using json::ReadArray;
using json::ReadObject;
constexpr std::size_t kMaxCategories = 4096u;

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

bool SafeCredential(std::string_view value, std::size_t maximum)
{
    if (value.empty() || value.size() > maximum)
        return false;
    for (const unsigned char byte : value)
        if (byte < 0x20u || byte == 0x7fu)
            return false;
    return true;
}

bool EndsWithCi(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
           EqualsCi(value.substr(value.size() - suffix.size()), suffix);
}

bool PercentEncode(std::string_view input, std::string *output)
{
    if (!output)
        return false;
    static constexpr char digits[] = "0123456789ABCDEF";
    output->clear();
    output->reserve(input.size() * 3u);
    for (const unsigned char byte : input)
    {
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~')
        {
            output->push_back(static_cast<char>(byte));
        }
        else
        {
            output->push_back('%');
            output->push_back(digits[byte >> 4u]);
            output->push_back(digits[byte & 0x0fu]);
        }
    }
    return true;
}

XtreamStatus ReplaceFile(const std::string &temporary, const std::string &path)
{
#ifdef _WIN32
    return MoveFileExA(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? XtreamStatus::ok
               : XtreamStatus::io_error;
#else
    return std::rename(temporary.c_str(), path.c_str()) == 0 ? XtreamStatus::ok
                                                             : XtreamStatus::io_error;
#endif
}

template <typename Handler> bool ReadArrayResponse(JsonReader *reader, Handler handler, bool *found)
{
    if (!reader || !found)
        return false;
    *found = false;
    if (reader->Peek() == '[')
    {
        *found = true;
        return ReadArray(reader, handler) && reader->Finished();
    }
    if (reader->Peek() != '{')
        return false;
    const bool valid = ReadObject(reader,
                                  [&](const std::string &key, JsonReader *value)
                                  {
                                      if (key == "data" && value->Peek() == '[')
                                      {
                                          *found = true;
                                          return ReadArray(value, handler);
                                      }
                                      return value->SkipValue();
                                  });
    return valid && reader->Finished();
}

std::string_view CategoryName(const std::unordered_map<std::string, std::string> &categories,
                              const std::string &id)
{
    const auto found = categories.find(id);
    return found == categories.end() || found->second.empty() ? std::string_view("Live TV")
                                                              : std::string_view(found->second);
}

std::string StableXtreamChannelId(std::uint64_t source_id, std::string_view stream_id)
{
    char prefix[32]{};
    std::snprintf(prefix, sizeof(prefix),
                  "xtream:%016llx:", static_cast<unsigned long long>(source_id));
    return std::string(prefix) + std::string(stream_id);
}

// "server/live/user/password/": what every live address of an account starts with.
std::string LiveUrlPrefix(const XtreamCredentials &credentials)
{
    std::string username;
    std::string password;
    PercentEncode(credentials.username, &username);
    PercentEncode(credentials.password, &password);
    return credentials.server_url + "/live/" + username + "/" + password + "/";
}

bool LiveUrlFromPrefix(const std::string &prefix, std::string_view stream_id,
                       std::string_view extension, std::string *url)
{
    if (stream_id.empty() || stream_id.size() > 64u || extension.size() > 12u)
        return false;
    const std::string_view selected_extension = extension.empty() ? "ts" : extension;
    if (!std::all_of(selected_extension.begin(), selected_extension.end(),
                     [](unsigned char value) { return std::isalnum(value) != 0; }))
        return false;
    std::string stream;
    PercentEncode(stream_id, &stream);
    url->assign(prefix);
    url->append(stream);
    url->push_back('.');
    url->append(selected_extension);
    return url->size() <= kDefaultMaxUrlBytes;
}

} // namespace

bool NormalizeXtreamServerUrl(std::string_view input, std::string *normalized)
{
    if (!normalized || input.empty() || input.size() > kMaxXtreamServerBytes ||
        input.find('?') != std::string_view::npos || input.find('#') != std::string_view::npos)
        return false;
    std::string canonical;
    if (!CanonicalizeStreamUrl(input, &canonical))
        return false;
    while (!canonical.empty() && canonical.back() == '/')
        canonical.pop_back();
    constexpr std::string_view endpoint = "/player_api.php";
    if (EndsWithCi(canonical, endpoint))
        canonical.resize(canonical.size() - endpoint.size());
    while (!canonical.empty() && canonical.back() == '/')
        canonical.pop_back();
    if (canonical.size() < 8u)
        return false;
    *normalized = std::move(canonical);
    return true;
}

bool ValidateXtreamCredentials(const XtreamCredentials &credentials)
{
    std::string normalized;
    return NormalizeXtreamServerUrl(credentials.server_url, &normalized) &&
           normalized == credentials.server_url &&
           SafeCredential(credentials.username, kMaxXtreamCredentialBytes) &&
           SafeCredential(credentials.password, kMaxXtreamCredentialBytes);
}

std::uint64_t XtreamSourceId(const XtreamCredentials &credentials)
{
    std::uint64_t hash = UINT64_C(1469598103934665603);
    const auto add = [&hash](std::string_view value)
    {
        for (const unsigned char byte : value)
        {
            hash ^= byte;
            hash *= UINT64_C(1099511628211);
        }
        hash ^= 0xffu;
        hash *= UINT64_C(1099511628211);
    };
    add(credentials.server_url);
    add(credentials.username);
    add(credentials.password);
    return UINT64_C(0x5854000000000000) | (hash & UINT64_C(0x0000ffffffffffff));
}

bool BuildXtreamApiUrl(const XtreamCredentials &credentials, std::string_view action,
                       std::string *url)
{
    if (!url || !ValidateXtreamCredentials(credentials))
        return false;
    std::string username;
    std::string password;
    std::string encoded_action;
    PercentEncode(credentials.username, &username);
    PercentEncode(credentials.password, &password);
    PercentEncode(action, &encoded_action);
    *url =
        credentials.server_url + "/player_api.php?username=" + username + "&password=" + password;
    if (!action.empty())
        *url += "&action=" + encoded_action;
    return url->size() <= kDefaultMaxUrlBytes;
}

bool BuildXtreamLiveUrl(const XtreamCredentials &credentials, std::string_view stream_id,
                        std::string_view extension, std::string *url)
{
    return url && ValidateXtreamCredentials(credentials) &&
           LiveUrlFromPrefix(LiveUrlPrefix(credentials), stream_id, extension, url);
}

bool BuildXtreamGuideUrl(const XtreamCredentials &credentials, std::string *url)
{
    if (!BuildXtreamApiUrl(credentials, "", url))
        return false;
    url->replace(credentials.server_url.size(), sizeof("/player_api.php") - 1, "/xmltv.php");
    return true;
}

bool BuildXtreamMediaUrl(const XtreamCredentials &credentials, bool episode,
                         std::string_view stream_id, std::string_view extension, std::string *url)
{
    if (!BuildXtreamLiveUrl(credentials, stream_id, extension.empty() ? "mp4" : extension, url))
        return false;
    url->replace(credentials.server_url.size() + 1, 4, episode ? "series" : "movie");
    return url->size() <= kDefaultMaxUrlBytes;
}

bool BuildXtreamSeriesUrl(const XtreamCredentials &credentials, std::string_view series_id,
                          std::string *url)
{
    if (series_id.empty() || series_id.size() > 64 ||
        !BuildXtreamApiUrl(credentials, "get_series_info", url))
        return false;
    std::string encoded;
    PercentEncode(series_id, &encoded);
    *url += "&series_id=" + encoded;
    return url->size() <= kDefaultMaxUrlBytes;
}

XtreamStatus SaveXtreamCredentials(const std::string &path, const XtreamCredentials &credentials)
{
    if (path.empty() || !ValidateXtreamCredentials(credentials))
        return XtreamStatus::invalid_argument;
    const std::string temporary = path + ".tmp";
    std::FILE *output = std::fopen(temporary.c_str(), "wb");
    if (!output)
        return XtreamStatus::io_error;
    const std::array<std::string_view, 4> lines = {kCredentialsMagic, credentials.server_url,
                                                   credentials.username, credentials.password};
    bool written = true;
    for (const std::string_view line : lines)
    {
        written = written && std::fwrite(line.data(), 1, line.size(), output) == line.size();
        written = written && std::fwrite("\n", 1, 1, output) == 1;
    }
    written = written && std::fflush(output) == 0;
    written = std::fclose(output) == 0 && written;
    if (!written)
    {
        std::remove(temporary.c_str());
        return XtreamStatus::io_error;
    }
    const XtreamStatus replaced = ReplaceFile(temporary, path);
    if (replaced != XtreamStatus::ok)
        std::remove(temporary.c_str());
    return replaced;
}

XtreamStatus LoadXtreamCredentials(const std::string &path, XtreamCredentials *credentials)
{
    if (path.empty() || !credentials)
        return XtreamStatus::invalid_argument;
    std::FILE *input = std::fopen(path.c_str(), "rb");
    if (!input)
        return XtreamStatus::not_found;
    constexpr std::size_t capacity =
        sizeof(kCredentialsMagic) + kMaxXtreamServerBytes + 2u * kMaxXtreamCredentialBytes + 8u;
    std::array<char, capacity> file{};
    const std::size_t bytes = std::fread(file.data(), 1, file.size(), input);
    const bool failed = std::ferror(input) != 0 || std::fclose(input) != 0;
    if (failed)
        return XtreamStatus::io_error;
    if (bytes == file.size())
        return XtreamStatus::too_large;
    std::array<std::string, 4> lines;
    std::size_t start = 0;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::size_t newline = std::string_view(file.data(), bytes).find('\n', start);
        if (newline == std::string_view::npos)
            return XtreamStatus::corrupt;
        std::size_t end = newline;
        if (end > start && file[end - 1u] == '\r')
            --end;
        lines[index].assign(file.data() + start, end - start);
        start = newline + 1u;
    }
    if (start != bytes || lines[0] != kCredentialsMagic)
        return XtreamStatus::corrupt;
    XtreamCredentials loaded{lines[1], lines[2], lines[3]};
    if (!ValidateXtreamCredentials(loaded))
        return XtreamStatus::corrupt;
    *credentials = std::move(loaded);
    return XtreamStatus::ok;
}

XtreamStatus SaveXtreamCredentials(const XtreamCredentials &credentials)
{
    return SaveXtreamCredentials(kDefaultXtreamCredentialsPath, credentials);
}

XtreamStatus LoadXtreamCredentials(XtreamCredentials *credentials)
{
    return LoadXtreamCredentials(kDefaultXtreamCredentialsPath, credentials);
}

XtreamStatus ParseXtreamAuth(std::string_view json, XtreamAuth *auth)
{
    if (!auth)
        return XtreamStatus::invalid_argument;
    *auth = {};
    if (json.empty() || json.size() > kMaxXtreamResponseBytes)
        return json.size() > kMaxXtreamResponseBytes ? XtreamStatus::too_large
                                                     : XtreamStatus::malformed_json;
    JsonReader reader(json);
    bool found_user = false;
    std::string authenticated;
    const bool valid =
        ReadObject(&reader,
                   [&](const std::string &key, JsonReader *value)
                   {
                       if (key != "user_info")
                           return value->SkipValue();
                       found_user = true;
                       return ReadObject(value,
                                         [&](const std::string &field, JsonReader *entry)
                                         {
                                             if (field == "auth")
                                                 return entry->StringOrScalar(&authenticated, 16u);
                                             if (field == "status")
                                                 return entry->StringOrScalar(&auth->status, 64u);
                                             if (field == "message")
                                                 return entry->StringOrScalar(&auth->message, 256u);
                                             return entry->SkipValue();
                                         });
                   });
    if (!valid || !reader.Finished() || !found_user)
        return XtreamStatus::malformed_json;
    auth->authenticated = authenticated == "1" || EqualsCi(authenticated, "true");
    if (!auth->authenticated)
        return XtreamStatus::authentication_failed;
    if (!auth->status.empty() && !EqualsCi(auth->status, "active"))
        return XtreamStatus::account_inactive;
    return XtreamStatus::ok;
}

XtreamStatus ParseXtreamCategories(std::string_view json, std::vector<XtreamCategory> *categories)
{
    if (!categories)
        return XtreamStatus::invalid_argument;
    categories->clear();
    if (json.empty() || json.size() > kMaxXtreamResponseBytes)
        return json.size() > kMaxXtreamResponseBytes ? XtreamStatus::too_large
                                                     : XtreamStatus::malformed_json;
    JsonReader reader(json);
    bool found = false;
    const bool valid = ReadArrayResponse(
        &reader,
        [&](JsonReader *entry)
        {
            XtreamCategory category;
            if (!ReadObject(entry,
                            [&](const std::string &key, JsonReader *value)
                            {
                                if (key == "category_id")
                                    return value->StringOrScalar(&category.id, 64u);
                                if (key == "category_name")
                                    return value->StringOrScalar(&category.name);
                                if (key == "parent_id")
                                    return value->StringOrScalar(&category.parent_id, 64u);
                                return value->SkipValue();
                            }))
                return false;
            if (!category.id.empty() && categories->size() < kMaxCategories)
                categories->push_back(std::move(category));
            return true;
        },
        &found);
    if (!valid || !found)
        return XtreamStatus::malformed_json;
    const auto originals = *categories;
    std::unordered_map<std::string, std::size_t> by_id;
    for (std::size_t i = 0; i < originals.size(); ++i)
        by_id.emplace(originals[i].id, i);
    for (auto &category : *categories)
    {
        auto parent = category.parent_id;
        std::vector<std::string> visited{category.id};
        while (!parent.empty() && parent != "0" && visited.size() < 32)
        {
            if (std::find(visited.begin(), visited.end(), parent) != visited.end())
                break;
            const auto found_parent = by_id.find(parent);
            if (found_parent == by_id.end())
                break;
            const auto &ancestor = originals[found_parent->second];
            if (category.name.size() + ancestor.name.size() + 3 > kDefaultMaxFieldBytes)
                break;
            if (!ancestor.name.empty())
                category.name = ancestor.name + " / " + category.name;
            visited.push_back(parent);
            parent = ancestor.parent_id;
        }
    }
    return XtreamStatus::ok;
}

struct XtreamStreamsParser::State
{
    std::string live_prefix;
    std::unordered_map<std::string, std::string> category_names;
    std::uint64_t source_id = 0;
    Catalog *catalog = nullptr;
    ParseReport local_report;
    ParseReport *report = nullptr;
    std::size_t max_channels = kDefaultMaxChannels;
    ListSplitter splitter;
    std::uint32_t source_line = 0;
    std::size_t bytes_seen = 0;
    bool usable = false;
    bool too_large = false;
    bool full = false;

    void LeftOut()
    {
        full = true;
        report->catalog_full = true;
        ++report->skipped;
    }

    // One stream of the list. False: it is not what a provider sends.
    bool Stream(std::string_view element)
    {
        ++source_line;
        ++report->lines_seen;
        std::string stream_id;
        std::string name;
        std::string logo;
        std::string epg_id;
        std::string category_id;
        std::string extension;
        std::string direct_source;
        std::string stream_url;
        std::string archive, archive_days;
        JsonReader entry(element);
        if (!ReadObject(&entry,
                        [&](const std::string &key, JsonReader *value)
                        {
                            if (key == "stream_id")
                                return value->StringOrScalar(&stream_id, 64u);
                            if (key == "tv_archive")
                                return value->StringOrScalar(&archive, 12u);
                            if (key == "tv_archive_duration")
                                return value->StringOrScalar(&archive_days, 12u);
                            if (key == "name")
                                return value->StringOrScalar(&name);
                            if (key == "stream_icon")
                                return value->StringOrScalar(&logo, kDefaultMaxUrlBytes);
                            if (key == "epg_channel_id")
                                return value->StringOrScalar(&epg_id);
                            if (key == "category_id")
                                return value->StringOrScalar(&category_id, 64u);
                            if (key == "container_extension")
                                return value->StringOrScalar(&extension, 12u);
                            if (key == "direct_source")
                                return value->StringOrScalar(&direct_source, kDefaultMaxUrlBytes);
                            if (key == "stream_url")
                                return value->StringOrScalar(&stream_url, kDefaultMaxUrlBytes);
                            return value->SkipValue();
                        }) ||
            !entry.Finished())
            return false;

        std::string generated_url;
        const std::string id = StableXtreamChannelId(source_id, stream_id);
        if (stream_id.empty() || catalog->Find(id) != Catalog::npos ||
            !LiveUrlFromPrefix(live_prefix, stream_id, extension, &generated_url))
        {
            ++report->skipped;
            return true;
        }
        if (catalog->size() >= max_channels)
        {
            LeftOut();
            return true;
        }
        if (name.empty())
            name = "Channel " + stream_id;
        std::string logo_url;
        std::string direct_url;
        ChannelView channel;
        channel.id = id;
        channel.source_id = source_id;
        channel.name = name;
        channel.tvg_name = name;
        channel.tvg_id = epg_id;
        channel.group_title = CategoryName(category_names, category_id);
        channel.source_line = source_line;
        if (archive == "1")
        {
            channel.catchup = "xc";
            // Keep the generated live path even when playback uses a direct CDN URL.
            channel.catchup_source = generated_url;
            channel.catchup_days = archive_days;
        }
        if (CanonicalizeStreamUrl(logo, &logo_url))
            channel.tvg_logo = logo_url;
        if (direct_source.empty())
            direct_source = std::move(stream_url);
        const bool direct =
            CanonicalizeStreamUrl(direct_source, &direct_url) && direct_url != generated_url;
        channel.url = direct ? direct_url : generated_url;
        const std::size_t index = catalog->size();
        if (!catalog->Add(channel) || (direct && !catalog->AddAlternateUrl(index, generated_url)))
        {
            LeftOut();
            return true;
        }
        ++report->accepted;
        return true;
    }
};

XtreamStreamsParser::XtreamStreamsParser(const XtreamCredentials &credentials,
                                         const std::vector<XtreamCategory> &categories,
                                         std::uint64_t source_id, Catalog *catalog,
                                         ParseReport *report, std::size_t max_channels)
    : state_(new State)
{
    State &state = *state_;
    state.usable = catalog != nullptr && source_id != 0 && ValidateXtreamCredentials(credentials);
    state.source_id = source_id;
    state.catalog = catalog;
    state.report = report == nullptr ? &state.local_report : report;
    *state.report = {};
    state.max_channels = max_channels;
    if (!state.usable)
        return;
    state.live_prefix = LiveUrlPrefix(credentials);
    state.category_names.reserve(categories.size());
    for (const XtreamCategory &category : categories)
        if (!category.id.empty())
            state.category_names.emplace(category.id, category.name);
    catalog->Clear();
    catalog->source_id = source_id;
}

XtreamStreamsParser::~XtreamStreamsParser() = default;

bool XtreamStreamsParser::Feed(std::string_view bytes)
{
    State &state = *state_;
    if (!state.usable || state.too_large)
        return false;
    state.bytes_seen += bytes.size();
    if (state.bytes_seen > kMaxXtreamResponseBytes)
    {
        state.too_large = true;
        return false;
    }
    return state.splitter.Feed(bytes, [&state](std::string_view element)
                               { return state.Stream(element); });
}

XtreamStatus XtreamStreamsParser::Finish()
{
    State &state = *state_;
    if (!state.usable)
        return XtreamStatus::invalid_argument;
    // A download that stopped because the catalog was full ends in the middle
    // of the list: what it has is the first max_channels streams.
    const bool whole = state.splitter.complete() && state.splitter.found();
    if (state.too_large || (!whole && !state.full))
    {
        state.catalog->Clear();
        return state.too_large ? XtreamStatus::too_large : XtreamStatus::malformed_json;
    }
    return state.catalog->empty() ? XtreamStatus::no_channels : XtreamStatus::ok;
}

bool XtreamStreamsParser::full() const
{
    return state_->full;
}

XtreamStatus ParseXtreamLiveStreams(std::string_view json, const XtreamCredentials &credentials,
                                    const std::vector<XtreamCategory> &categories,
                                    std::uint64_t source_id, Catalog *catalog, ParseReport *report,
                                    std::size_t max_channels)
{
    if (!catalog || !ValidateXtreamCredentials(credentials) || source_id == 0)
        return XtreamStatus::invalid_argument;
    XtreamStreamsParser parser(credentials, categories, source_id, catalog, report, max_channels);
    if (json.empty())
        return XtreamStatus::malformed_json;
    // A list that is whole in memory is read to its end, so that the report
    // counts every stream it left out.
    (void)parser.Feed(json);
    return parser.Finish();
}

const char *XtreamStatusDescription(XtreamStatus status)
{
    switch (status)
    {
    case XtreamStatus::ok:
        return "ready";
    case XtreamStatus::invalid_argument:
        return "invalid Xtream server or credentials";
    case XtreamStatus::not_found:
        return "Xtream credentials are not configured";
    case XtreamStatus::too_large:
        return "Xtream response exceeds the supported size";
    case XtreamStatus::io_error:
        return "Xtream credentials could not be stored";
    case XtreamStatus::corrupt:
        return "saved Xtream credentials are invalid";
    case XtreamStatus::malformed_json:
        return "provider returned malformed Xtream data";
    case XtreamStatus::authentication_failed:
        return "Xtream username or password was rejected";
    case XtreamStatus::account_inactive:
        return "Xtream account is expired or inactive";
    case XtreamStatus::no_channels:
        return "Xtream provider returned no live channels";
    }
    return "Xtream request failed";
}

} // namespace iptv
