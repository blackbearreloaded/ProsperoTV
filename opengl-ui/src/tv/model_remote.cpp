// ProsperoTV - Source management for the paired browser.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/model.hpp"
#include "tv/local_tv.hpp"
#include "iptv_json.h"
#include "iptv_ime.h"
#include <charconv>
#include <map>

namespace ptv
{
namespace
{
std::string quoted(std::string_view value)
{
    std::string result = "\"";
    for (const unsigned char c : value)
    {
        if (c == '"' || c == '\\')
            result += '\\';
        if (c < 32)
            result += ' ';
        else
            result += static_cast<char>(c);
    }
    return result + '"';
}

bool integer(std::string_view text, std::int64_t &value)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           value >= 0;
}
} // namespace

int Model::remote_sources(std::string_view input, std::string &output)
{
    if (input.empty())
    {
        output = "[";
        for (const auto &source : sources_)
        {
            if (output.size() > 1)
                output += ',';
            output += "{\"id\":" + std::to_string(source.id) +
                      ",\"kind\":" + std::to_string(source.kind) +
                      ",\"name\":" + quoted(source.name) + ",\"url\":" + quoted(source.url) +
                      ",\"username\":" + quoted(source.username) +
                      ",\"mac\":" + quoted(source.mac) +
                      ",\"schedule\":" + std::to_string(static_cast<int>(source.schedule)) +
                      ",\"selected\":" + (source.id == selected_source_id_ ? "true" : "false") +
                      ",\"passwordSet\":" + (source.password.empty() ? "false" : "true") + "}";
            if (output.size() >= 120u * 1024u)
            {
                output = "Too many source details to send. Manage sources on the TV.";
                return 413;
            }
        }
        output += ']';
        return 200;
    }
    if (refreshing() || account_step_ != AccountStep::none)
    {
        output = "Wait for the source update or TV source form to finish.";
        return 409;
    }
    output = "Check the source details and try again.";
    if (input.size() > 12u * 1024u)
        return 400;
    iptv::json::JsonReader json(input);
    std::map<std::string, std::string> fields;
    if (!json.Consume('{') || json.Consume('}'))
        return 400;
    do
    {
        std::string key, value;
        if (!json.String(&key, 32) || !json.Consume(':') || !json.StringOrScalar(&value, 4096) ||
            fields.contains(key) || fields.size() >= 12)
            return 400;
        fields.emplace(std::move(key), std::move(value));
    } while (json.Consume(','));
    if (!json.Consume('}') || !json.Finished())
        return 400;
    for (const auto &[key, value] : fields)
        if (key != "operation" && key != "id" && key != "kind" && key != "name" && key != "url" &&
            key != "username" && key != "password" && key != "mac" && key != "schedule")
            return 400;
    std::int64_t id = 0;
    if (!integer(fields["id"], id))
        return 400;
    const auto *previous = saved_source(id);
    if (id != 0 && previous == nullptr)
        return 404;
    const auto &operation = fields["operation"];
    if (operation == "select")
    {
        if (!previous || (previous->kind != 0 && previous->url.empty()))
            return 400;
        iptv_ime_cancel();
        use_saved_source(id);
        output = selected_source_id_ == id ? "Source selected" : "The source could not be selected";
        return selected_source_id_ == id ? 200 : 500;
    }
    if (operation == "remove")
    {
        if (id <= 4)
            return 400;
        iptv_ime_cancel();
        const bool removed = remove_source(id);
        output = removed ? "Source removed" : "The source could not be removed";
        return removed ? 200 : 500;
    }
    if (operation != "save" || id == 1)
        return 400;
    SavedSource source = previous ? *previous : SavedSource{};
    std::int64_t kind = 0, schedule = 0;
    if (!integer(fields["kind"], kind) || kind < 1 || kind > 5 ||
        !integer(fields["schedule"], schedule) || schedule > 2 ||
        (previous && previous->kind != kind))
        return 400;
    source.id = id;
    source.kind = static_cast<int>(kind);
    source.schedule = static_cast<RefreshSchedule>(schedule);
    source.name = fields["name"];
    source.url = fields["url"];
    source.username = fields["username"];
    source.mac = fields["mac"];
    if (fields.contains("password"))
        source.password = fields["password"];
    std::string normalized;
    if (kind >= 4)
    {
        if (!local_tv_address(source.url, &normalized))
            return 400;
    }
    else if (kind == 3)
    {
        if (!portal_endpoint(source.url, &normalized) || !portal_mac(source.mac, &source.mac))
            return 400;
    }
    else if (kind == 2)
    {
        if (!iptv::NormalizeXtreamServerUrl(source.url, &normalized))
            return 400;
    }
    else if (!iptv::CanonicalizeStreamUrl(source.url, &normalized))
        return 400;
    source.url = std::move(normalized);
    if (!valid_source(source))
        return 400;
    iptv_ime_cancel();
    const bool saved = commit_source(std::move(source));
    ++revision_;
    output = saved ? "Source saved on the TV" : "The source could not be saved";
    return saved ? 200 : 500;
}
} // namespace ptv
