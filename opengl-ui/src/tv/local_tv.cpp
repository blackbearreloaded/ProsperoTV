// ProsperoTV - HDHomeRun lineup and Tvheadend playlist readers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/local_tv.hpp"
#include "tv/platform.hpp"
#include "iptv_json.h"
#include "iptv_source_state.h"

namespace ptv
{
bool local_tv_address(std::string_view input, std::string *address)
{
    std::string normalized;
    if (!address || input.size() > 1020 || input.find_first_of("?#") != input.npos ||
        !iptv::CanonicalizeStreamUrl(input, &normalized))
        return false;
    while (normalized.ends_with('/'))
        normalized.pop_back();
    for (const std::string_view endpoint : {"/lineup.json", "/playlist/channels", "/playlist"})
        if (normalized.ends_with(endpoint))
        {
            normalized.resize(normalized.size() - endpoint.size());
            break;
        }
    *address = std::move(normalized);
    return true;
}

std::uint64_t local_tv_source_id(const SavedSource &source)
{
    return iptv::CustomSourceId("local-tv:" + std::to_string(source.kind) + ":" + source.url + ":" +
                                source.username);
}

std::string local_tv_authorization(const SavedSource &source)
{
    if (source.kind != 5 || source.username.empty())
        return {};
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto credentials = source.username + ":" + source.password;
    std::string result = "Basic ";
    for (std::size_t i = 0; i < credentials.size(); i += 3)
    {
        const unsigned a = static_cast<unsigned char>(credentials[i]);
        const unsigned b =
            i + 1 < credentials.size() ? static_cast<unsigned char>(credentials[i + 1]) : 0;
        const unsigned c =
            i + 2 < credentials.size() ? static_cast<unsigned char>(credentials[i + 2]) : 0;
        result += alphabet[a >> 2];
        result += alphabet[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < credentials.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < credentials.size() ? alphabet[c & 63] : '=';
    }
    return result;
}

bool parse_hdhomerun(std::string_view json, std::uint64_t source, iptv::Catalog *catalog,
                     iptv::ParseReport *report)
{
    if (!catalog || !report || json.size() > 4u * 1024u * 1024u)
        return false;
    iptv::Catalog next;
    next.source_id = source;
    *report = {};
    iptv::json::JsonReader reader(json);
    const bool valid = iptv::json::ReadArray(
        &reader,
        [&](iptv::json::JsonReader *entry)
        {
            iptv::Channel channel;
            std::string number, drm, tags, raw_url;
            if (!iptv::json::ReadObject(entry,
                                        [&](const std::string &key, iptv::json::JsonReader *value)
                                        {
                                            if (key == "GuideNumber")
                                                return value->StringOrScalar(&number, 64);
                                            if (key == "GuideName")
                                                return value->String(&channel.name);
                                            if (key == "URL")
                                                return value->String(&raw_url, 4096);
                                            if (key == "DRM")
                                                return value->StringOrScalar(&drm, 16);
                                            if (key == "Tags")
                                                return value->String(&tags, 256);
                                            return value->SkipValue();
                                        }))
                return false;
            ++report->lines_seen;
            bool protected_channel = drm == "1" || drm == "true";
            std::string_view remaining(tags);
            while (!remaining.empty())
            {
                const auto comma = remaining.find(',');
                protected_channel |= remaining.substr(0, comma) == "drm";
                if (comma == remaining.npos)
                    break;
                remaining.remove_prefix(comma + 1);
            }
            if (protected_channel || channel.name.empty() || number.empty() ||
                !iptv::CanonicalizeStreamUrl(raw_url, &channel.url))
            {
                ++report->skipped;
                return true;
            }
            channel.id = "hdhomerun:" + std::to_string(source) + ":" + number;
            channel.source_id = source;
            channel.tvg_id = number;
            channel.tvg_name = channel.name;
            channel.group_title = "Local TV / HDHomeRun";
            if (next.Find(channel.id) != iptv::Catalog::npos)
            {
                ++report->duplicates;
                return true;
            }
            if (!next.Add(channel))
                return false;
            ++report->accepted;
            return true;
        });
    if (!valid || !reader.Finished())
        return false;
    *catalog = std::move(next);
    return true;
}

iptv::http::FetchResult load_local_tv(const SavedSource &source, iptv::Catalog *catalog,
                                      iptv::ParseReport *report,
                                      const iptv::http::RequestControl *control)
{
    if (source.kind == 4)
    {
        auto body = iptv::http::AllocateListBuffer(4u * 1024u * 1024u);
        if (!body.data())
            return {iptv::http::Status::response_too_large};
        auto result = platform::fetch((source.url + "/lineup.json").c_str(), body.data(),
                                      body.size(), body.max_bytes, control);
        if (result.status == iptv::http::Status::ok &&
            !parse_hdhomerun({body.data(), result.bytes}, local_tv_source_id(source), catalog,
                             report))
            result.status = iptv::http::Status::read_failed;
        return result;
    }
    const auto authorization = local_tv_authorization(source);
    iptv::http::RequestHeaders headers{};
    headers.authorization = authorization.c_str();
    headers.credential_origin = source.url.c_str();
    iptv::M3uParser parser(catalog, local_tv_source_id(source), {}, report);
    const iptv::http::ListSink sink{
        [](void *context, const char *data, std::size_t size)
        { return static_cast<iptv::M3uParser *>(context)->Feed({data, size}); }, &parser};
    auto result = platform::fetch_list((source.url + "/playlist/channels").c_str(), sink,
                                       iptv::http::kMaxListBytes, control, &headers);
    if (result.status == iptv::http::Status::ok)
    {
        parser.Finish();
        if (catalog->guide_urls.empty())
            catalog->guide_urls.push_back(source.url + "/xmltv/channels");
    }
    else
        catalog->Clear();
    return result;
}
} // namespace ptv
