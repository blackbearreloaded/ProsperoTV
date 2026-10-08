// ProsperoTV - Portal handshake, provider groups, live streams and temporary links.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/portal.hpp"
#include "tv/platform.hpp"
#include "iptv_json.h"
#include "iptv_source_state.h"
#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace ptv
{
namespace
{
using iptv::json::JsonReader;
using iptv::json::ReadArray;
using iptv::json::ReadObject;
std::string encode(std::string_view text)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : text)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            result += static_cast<char>(c);
        else
        {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    return result;
}
template <typename Handler> bool js_object(std::string_view text, Handler handler)
{
    JsonReader reader(text);
    bool found = false;
    return ReadObject(&reader,
                      [&](const std::string &key, JsonReader *value)
                      {
                          if (key != "js")
                              return value->SkipValue();
                          if (found)
                              return false;
                          found = true;
                          return ReadObject(value, handler);
                      }) &&
           found && reader.Finished();
}
bool stream_address(std::string_view command, std::string *url)
{
    while (!command.empty() && command.front() == ' ')
        command.remove_prefix(1);
    if (command.starts_with("ffmpeg "))
        command.remove_prefix(7);
    else if (command.starts_with("ffrt "))
        command.remove_prefix(5);
    while (!command.empty() && command.front() == ' ')
        command.remove_prefix(1);
    // Only the URL returned by create_link is playable, never flags, a pipe,
    // another protocol, or an executable command from a portal response.
    return iptv::CanonicalizeStreamUrl(command, url) &&
           iptv::http::IsSupportedPlaylistUrl(url->c_str());
}
struct Category
{
    std::string title, parent;
};
using Categories = std::unordered_map<std::string, Category>;
std::string category_path(const Categories &categories, const std::string &id)
{
    auto current = id;
    std::string result;
    std::vector<std::string> visited;
    while (!current.empty() && current != "0" && visited.size() < 32)
    {
        if (std::find(visited.begin(), visited.end(), current) != visited.end())
            break;
        visited.push_back(current);
        const auto at = categories.find(current);
        if (at == categories.end())
            break;
        if (!at->second.title.empty())
            result = at->second.title + (result.empty() ? "" : " / " + result);
        current = at->second.parent;
    }
    return result.empty() ? "Live TV" : result;
}
} // namespace

bool portal_endpoint(std::string_view address, std::string *endpoint)
{
    std::string normalized;
    if (!endpoint || address.size() > 1020 || address.find_first_of("?#") != address.npos ||
        !iptv::CanonicalizeStreamUrl(address, &normalized) ||
        !iptv::http::IsSupportedPlaylistUrl(normalized.c_str()))
        return false;
    while (normalized.ends_with('/'))
        normalized.pop_back();
    if (normalized.ends_with("/server/load.php") || normalized.ends_with("/portal.php"))
    {
        *endpoint = std::move(normalized);
        return true;
    }
    if (normalized.ends_with("/c/index.html"))
        normalized.resize(normalized.size() - 13);
    else if (normalized.ends_with("/c"))
        normalized.resize(normalized.size() - 2);
    *endpoint = normalized + "/server/load.php";
    return true;
}
bool portal_mac(std::string_view input, std::string *mac)
{
    if (!mac || input.size() != 17)
        return false;
    std::string result(input);
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        char &c = result[i];
        if (i % 3 == 2)
        {
            if (c != ':' && c != '-')
                return false;
            c = ':';
        }
        else if (c >= 'a' && c <= 'f')
            c = static_cast<char>(c - 'a' + 'A');
        else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return false;
    }
    *mac = std::move(result);
    return true;
}
std::uint64_t portal_source_id(const PortalCredentials &credentials)
{
    return iptv::CustomSourceId("portal\n" + credentials.url + "\n" + credentials.mac);
}
std::string portal_channel_id(std::uint64_t source, std::string_view id)
{
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix),
                  "portal:%016llx:", static_cast<unsigned long long>(source));
    return prefix + std::string(id);
}
PortalClient::PortalClient(PortalCredentials credentials, const iptv::http::RequestControl *control,
                           Fetch fetch)
    : credentials_(std::move(credentials)), fetch_(fetch ? std::move(fetch) : platform::fetch),
      control_(control)
{
    cookie_ = "mac=" + encode(credentials_.mac) + "; stb_lang=en; timezone=UTC";
    const auto server = credentials_.url.rfind("/server/load.php");
    referrer_ = server == credentials_.url.npos
                    ? credentials_.url.substr(0, credentials_.url.rfind('/') + 1)
                    : credentials_.url.substr(0, server) + "/c/";
}
iptv::http::RequestHeaders PortalClient::headers() const
{
    return {"Mozilla/5.0 (QtEmbedded; U; Linux; C) AppleWebKit/533.3 MAG250 stbapp ver: 2 rev: 250 "
            "Safari/533.3",
            referrer_.c_str(), cookie_.c_str(), authorization_.c_str()};
}
std::string PortalClient::address(std::string_view type, std::string_view action,
                                  const Parameters &parameters) const
{
    auto url = credentials_.url + "?type=" + encode(type) + "&action=" + encode(action);
    for (const auto &[key, value] : parameters)
        url += "&" + encode(key) + "=" + encode(value);
    return url + "&JsHttpRequest=1-xml";
}
bool PortalClient::request(std::string_view type, std::string_view action, std::string *reply,
                           const Parameters &parameters)
{
    const auto url = address(type, action, parameters);
    if (url.size() > iptv::http::kMaxUrlBytes)
    {
        error_ = "The portal returned a request that is too long.";
        return false;
    }
    constexpr std::size_t limit = 4u * 1024u * 1024u;
    reply->resize(limit + 1);
    const auto request_headers = headers();
    const auto response =
        fetch_(url.c_str(), reply->data(), reply->size(), limit, control_, &request_headers);
    reply->resize(response.bytes);
    if (response.status == iptv::http::Status::ok)
        return true;
    error_ = response.status == iptv::http::Status::cancelled ? "Portal request cancelled."
             : response.http_status == 401 || response.http_status == 403
                 ? "The portal refused this account. Check its MAC code and subscription."
                 : "The portal did not answer. Check its address and try again.";
    return false;
}
bool PortalClient::sign_in()
{
    std::string endpoint, mac;
    if (!portal_endpoint(credentials_.url, &endpoint) || endpoint != credentials_.url ||
        !portal_mac(credentials_.mac, &mac) || mac != credentials_.mac)
    {
        error_ = "Check the portal address and MAC code.";
        return false;
    }
    authorization_.clear();
    std::string reply, token;
    if (!request("stb", "handshake", &reply, {{"token", ""}}))
        return false;
    if (!js_object(reply, [&](const std::string &key, JsonReader *value)
                   { return key == "token" ? value->String(&token, 1024) : value->SkipValue(); }) ||
        token.empty() || token.find_first_of("\r\n") != token.npos)
    {
        error_ = "The portal did not accept this MAC code.";
        return false;
    }
    authorization_ = "Bearer " + token;
    if (!request("stb", "get_profile", &reply,
                 {{"stb_type", "MAG250"},
                  {"hd", "1"},
                  {"auth_second_step", "0"},
                  {"not_valid_token", "0"}}))
        return false;
    std::string status;
    if (!js_object(reply,
                   [&](const std::string &key, JsonReader *value) {
                       return key == "status" ? value->StringOrScalar(&status) : value->SkipValue();
                   }) ||
        (!status.empty() && status != "0"))
    {
        error_ =
            "The portal refused the profile. Check that this MAC code has an active subscription.";
        return false;
    }
    error_.clear();
    return true;
}
bool PortalClient::load(iptv::Catalog *catalog, iptv::ParseReport *report,
                        std::atomic<unsigned> *progress)
{
    if (!catalog || !sign_in())
        return false;
    Categories categories;
    std::string reply;
    if (!request("itv", "get_genres", &reply))
        return false;
    JsonReader reader(reply);
    bool found = false;
    const bool genres =
        ReadObject(&reader,
                   [&](const std::string &key, JsonReader *value)
                   {
                       if (key != "js")
                           return value->SkipValue();
                       found = true;
                       return ReadArray(
                           value,
                           [&](JsonReader *item)
                           {
                               std::string id;
                               Category category;
                               if (!ReadObject(item,
                                               [&](const std::string &field, JsonReader *data)
                                               {
                                                   if (field == "id")
                                                       return data->StringOrScalar(&id, 128);
                                                   if (field == "title" || field == "name")
                                                       return data->StringOrScalar(&category.title);
                                                   if (field == "parent_id")
                                                       return data->StringOrScalar(&category.parent,
                                                                                   128);
                                                   return data->SkipValue();
                                               }) ||
                                   categories.size() >= 16384)
                                   return false;
                               if (!id.empty() && id != "*")
                                   categories[id] = std::move(category);
                               return true;
                           });
                   }) &&
        found && reader.Finished();
    if (!genres)
    {
        error_ = "The portal's category list could not be read.";
        return false;
    }
    catalog->Clear();
    catalog->source_id = portal_source_id(credentials_);
    if (report)
        *report = {};
    iptv::json::ListSplitter splitter("js");
    bool full = false;
    const auto accept = [&](std::string_view text)
    {
        if (catalog->size() >= iptv::kDefaultMaxChannels)
        {
            full = true;
            return false;
        }
        iptv::Channel channel;
        std::string id, genre;
        JsonReader item(text);
        if (!ReadObject(&item,
                        [&](const std::string &key, JsonReader *value)
                        {
                            if (key == "id")
                                return value->StringOrScalar(&id, 128);
                            if (key == "name")
                                return value->StringOrScalar(&channel.name);
                            if (key == "cmd")
                                return value->StringOrScalar(&channel.portal_command);
                            if (key == "logo")
                                return value->StringOrScalar(&channel.tvg_logo);
                            if (key == "tv_genre_id")
                                return value->StringOrScalar(&genre, 128);
                            if (key == "xmltv_id")
                                return value->StringOrScalar(&channel.tvg_id);
                            return value->SkipValue();
                        }) ||
            !item.Finished())
            return false;
        if (id.empty() || id == "null" || channel.name.empty() || channel.portal_command.empty() ||
            channel.portal_command == "null")
        {
            if (report)
                ++report->skipped;
            return true;
        }
        channel.id = portal_channel_id(catalog->source_id, id);
        channel.source_id = catalog->source_id;
        channel.source_line = static_cast<std::uint32_t>(catalog->size() + 1);
        channel.group_title = category_path(categories, genre);
        if (channel.tvg_id.empty() || channel.tvg_id == "null")
            channel.tvg_id = id;
        if (!stream_address(channel.portal_command, &channel.url))
            channel.url = credentials_.url; // resolved just before playback, never played as a URL
        if (!channel.tvg_logo.empty() && channel.tvg_logo != "null")
        {
            char logo[iptv::http::kMaxUrlBytes + 1]{};
            channel.tvg_logo =
                iptv::http::ResolveRedirectUrl(referrer_.c_str(), channel.tvg_logo.c_str(), logo,
                                               sizeof(logo)) == iptv::http::Status::ok
                    ? logo
                    : "";
        }
        else
            channel.tvg_logo.clear();
        if (catalog->Find(channel.id) != iptv::Catalog::npos)
        {
            if (report)
                ++report->duplicates;
            return true;
        }
        if (!catalog->Add(channel))
            return false;
        if (progress)
            progress->store(static_cast<unsigned>(catalog->size()));
        return true;
    };
    auto feed = [&](const char *data, std::size_t size)
    { return splitter.Feed({data, size}, accept); };
    const iptv::http::ListSink sink{
        [](void *context, const char *data, std::size_t size)
        { return (*static_cast<decltype(feed) *>(context))(data, size); }, &feed};
    const auto request_headers = headers();
    const auto url = address("itv", "get_all_channels");
    const auto fetched = platform::fetch_list(url.c_str(), sink, iptv::http::kMaxListBytes,
                                              control_, &request_headers);
    const bool ok =
        (fetched.status == iptv::http::Status::ok && splitter.complete() && splitter.found()) ||
        (full && fetched.status == iptv::http::Status::stopped);
    if (!ok || catalog->empty())
    {
        catalog->Clear();
        error_ = "The portal's live channel list could not be downloaded.";
        return false;
    }
    if (report)
    {
        report->accepted = catalog->size();
        report->catalog_full = full;
    }
    return true;
}
bool PortalClient::resolve(std::string_view command, std::string *url)
{
    if (!url || command.empty() || command.size() > iptv::kDefaultMaxFieldBytes || !sign_in())
        return false;
    std::string reply, resolved;
    if (!request(
            "itv", "create_link", &reply,
            {{"cmd", std::string(command)}, {"forced_storage", "undefined"}, {"disable_ad", "0"}}))
        return false;
    if (!js_object(reply,
                   [&](const std::string &key, JsonReader *value)
                   {
                       return key == "cmd" ? value->String(&resolved, iptv::http::kMaxUrlBytes)
                                           : value->SkipValue();
                   }) ||
        !stream_address(resolved, url))
    {
        error_ = "The portal did not return a playable HTTP stream.";
        return false;
    }
    return true;
}
PortalResolveJob::~PortalResolveJob()
{
    cancel();
}
bool PortalResolveJob::start(PortalCredentials credentials, std::string command)
{
    cancel();
    credentials_ = std::move(credentials);
    command_ = std::move(command);
    url_.clear();
    error_.clear();
    stop_.store(false);
    done_.store(false);
    thread_ = platform::thread_start(work, this, 2u * 1024u * 1024u, "ptv-portal-link");
    return thread_ != nullptr;
}
void PortalResolveJob::cancel()
{
    if (!thread_)
        return;
    stop_.store(true);
    while (!done_.load())
    {
        platform::network_cancel();
        platform::sleep_ms(5);
    }
    platform::thread_join(thread_);
    thread_ = nullptr;
}
void *PortalResolveJob::work(void *self)
{
    auto &job = *static_cast<PortalResolveJob *>(self);
    const iptv::http::RequestControl control{
        [](void *self) { return static_cast<PortalResolveJob *>(self)->stop_.load(); }, &job};
    if (platform::network_init() == iptv::http::Status::ok)
    {
        PortalClient client(job.credentials_, &control);
        if (!client.resolve(job.command_, &job.url_))
            job.error_ = client.error();
        platform::network_shutdown();
    }
    else
        job.error_ = "The portal connection could not start.";
    if (job.stop_.load())
        job.url_.clear();
    job.done_.store(true);
    return nullptr;
}
} // namespace ptv
