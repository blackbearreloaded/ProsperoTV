/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_catalog.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_map>

namespace iptv
{
namespace
{

constexpr std::size_t kHardMaxPlaylistBytes = 16u * 1024u * 1024u;
constexpr std::size_t kHardMaxRecordBytes = 64u * 1024u;
constexpr std::size_t kHardMaxUrlBytes = 8192u;
constexpr std::size_t kHardMaxFieldBytes = 4096u;
constexpr std::size_t kHardMaxChannels = 65536u;
constexpr std::size_t kHardMaxAlternateUrls = 8u;
constexpr std::size_t kHardMaxAlternateGroups = 8u;
constexpr std::size_t kHardMaxDiagnostics = 512u;

struct EffectiveLimits
{
    std::size_t max_playlist_bytes;
    std::size_t max_record_bytes;
    std::size_t max_url_bytes;
    std::size_t max_field_bytes;
    std::size_t max_channels;
    std::size_t max_alternate_urls;
    std::size_t max_alternate_groups;
    std::size_t max_diagnostics;
};

EffectiveLimits ClampLimits(const ParseLimits &limits)
{
    return {
        std::min(limits.max_playlist_bytes, kHardMaxPlaylistBytes),
        std::min(limits.max_record_bytes, kHardMaxRecordBytes),
        std::min(limits.max_url_bytes, kHardMaxUrlBytes),
        std::min(limits.max_field_bytes, kHardMaxFieldBytes),
        std::min(limits.max_channels, kHardMaxChannels),
        std::min(limits.max_alternate_urls, kHardMaxAlternateUrls),
        std::min(limits.max_alternate_groups, kHardMaxAlternateGroups),
        std::min(limits.max_diagnostics, kHardMaxDiagnostics),
    };
}

bool IsAsciiSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\v' ||
           value == '\f';
}

bool IsControl(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    return byte < 0x20u || byte == 0x7fu;
}

std::string_view Trim(std::string_view value)
{
    while (!value.empty() && IsAsciiSpace(value.front()))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && IsAsciiSpace(value.back()))
    {
        value.remove_suffix(1);
    }
    return value;
}

char LowerAscii(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    return byte < 128u ? static_cast<char>(std::tolower(byte)) : value;
}

bool EqualInsensitive(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        if (LowerAscii(left[i]) != LowerAscii(right[i]))
        {
            return false;
        }
    }
    return true;
}

bool StartsWithInsensitive(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() &&
           EqualInsensitive(value.substr(0, prefix.size()), prefix);
}

std::string LowerTrimmed(std::string_view value)
{
    value = Trim(value);
    std::string result;
    result.reserve(value.size());
    for (char byte : value)
    {
        result.push_back(LowerAscii(byte));
    }
    return result;
}

std::string Hex(std::uint64_t value)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t i = result.size(); i > 0; --i)
    {
        result[i - 1] = digits[value & 0x0fu];
        value >>= 4;
    }
    return result;
}

std::uint64_t Fnv1a(std::string_view value, std::uint64_t hash = 1469598103934665603ull)
{
    for (unsigned char byte : value)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string StableId(std::uint64_t source_id, std::string_view identity)
{
    std::string seed = "source:" + Hex(source_id) + "|";
    seed.append(identity.data(), identity.size());
    return "ch-" + Hex(Fnv1a(seed));
}

enum class UrlError : std::uint8_t
{
    none,
    too_long,
    unsupported_scheme,
    unsafe,
    malformed,
};

bool IsSchemeChar(char value, bool first)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    if (first)
    {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
    }
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || value == '+' || value == '-' || value == '.';
}

bool IsDigits(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }
    for (char byte : value)
    {
        if (byte < '0' || byte > '9')
        {
            return false;
        }
    }
    return true;
}

UrlError CanonicalizeUrl(std::string_view raw, std::string *canonical)
{
    if (canonical == nullptr)
    {
        return UrlError::malformed;
    }
    canonical->clear();
    raw = Trim(raw);
    if (raw.empty())
    {
        return UrlError::malformed;
    }
    if (raw.size() > kHardMaxUrlBytes)
    {
        return UrlError::too_long;
    }
    for (char byte : raw)
    {
        if (IsControl(byte) || IsAsciiSpace(byte) || byte == '\\')
        {
            return UrlError::unsafe;
        }
    }

    const std::size_t colon = raw.find(':');
    if (colon == std::string_view::npos || colon == 0)
    {
        return UrlError::malformed;
    }
    for (std::size_t i = 0; i < colon; ++i)
    {
        if (!IsSchemeChar(raw[i], i == 0))
        {
            return UrlError::malformed;
        }
    }
    const std::string scheme = LowerTrimmed(raw.substr(0, colon));
    if (scheme != "http" && scheme != "https")
    {
        return UrlError::unsupported_scheme;
    }
    if (raw.substr(colon, 3) != "://")
    {
        return UrlError::malformed;
    }

    const std::size_t authority_start = colon + 3;
    std::size_t authority_end = raw.find_first_of("/?#", authority_start);
    if (authority_end == std::string_view::npos)
    {
        authority_end = raw.size();
    }
    const std::string_view authority = raw.substr(authority_start, authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos)
    {
        return UrlError::unsafe;
    }

    if (authority.front() == '[')
    {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1)
        {
            return UrlError::malformed;
        }
        if (close + 1 < authority.size() &&
            (authority[close + 1] != ':' || !IsDigits(authority.substr(close + 2))))
        {
            return UrlError::malformed;
        }
    }
    else
    {
        const std::size_t port_separator = authority.find(':');
        const std::string_view host = authority.substr(0, port_separator);
        if (host.empty() || host.find('[') != std::string_view::npos ||
            host.find(']') != std::string_view::npos)
        {
            return UrlError::malformed;
        }
        if (port_separator != std::string_view::npos &&
            !IsDigits(authority.substr(port_separator + 1)))
        {
            return UrlError::malformed;
        }
    }

    const std::size_t fragment = raw.find('#', authority_end);
    const std::size_t content_end = fragment == std::string_view::npos ? raw.size() : fragment;
    canonical->reserve(content_end);
    canonical->append(scheme);
    canonical->append("://");
    for (std::size_t i = authority_start; i < authority_end; ++i)
    {
        canonical->push_back(LowerAscii(raw[i]));
    }
    canonical->append(raw.substr(authority_end, content_end - authority_end));
    if (canonical->size() > kHardMaxUrlBytes)
    {
        canonical->clear();
        return UrlError::too_long;
    }
    return UrlError::none;
}

} // namespace

bool CanonicalizeStreamUrl(std::string_view raw, std::string *canonical)
{
    return CanonicalizeUrl(raw, canonical) == UrlError::none;
}

namespace
{

struct EntryMetadata
{
    std::string title;
    std::string tvg_id;
    std::string tvg_name;
    std::string tvg_logo;
    std::string group_title;
    std::string tvg_country;
    std::string tvg_language;
    std::string http_user_agent;
    std::string http_referrer;
};

struct PendingEntry
{
    EntryMetadata metadata;
    std::uint32_t line = 0;
};

void AddDiagnostic(ParseReport *report, const EffectiveLimits &limits, std::size_t line,
                   ParseIssueCode code)
{
    ++report->skipped;
    if (report->diagnostics.size() < limits.max_diagnostics)
    {
        report->diagnostics.push_back({line, code});
    }
}

bool ValidDuration(std::string_view value)
{
    value = Trim(value);
    if (value.empty())
    {
        return false;
    }
    std::size_t i = (value.front() == '-' || value.front() == '+') ? 1 : 0;
    bool digit = false;
    bool dot = false;
    for (; i < value.size(); ++i)
    {
        const char byte = value[i];
        if (byte >= '0' && byte <= '9')
        {
            digit = true;
        }
        else if (byte == '.' && !dot)
        {
            dot = true;
        }
        else
        {
            return false;
        }
    }
    return digit;
}

bool IsAttributeKeyChar(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || value == '-' || value == '_';
}

void SetAttribute(std::string_view key, std::string &&value, EntryMetadata *metadata)
{
    if (key == "tvg-id")
    {
        metadata->tvg_id = std::move(value);
    }
    else if (key == "tvg-name")
    {
        metadata->tvg_name = std::move(value);
    }
    else if (key == "tvg-logo")
    {
        metadata->tvg_logo = std::move(value);
    }
    else if (key == "group-title")
    {
        metadata->group_title = std::move(value);
    }
    else if (key == "tvg-country")
    {
        metadata->tvg_country = std::move(value);
    }
    else if (key == "tvg-language")
    {
        metadata->tvg_language = std::move(value);
    }
}

bool SetHttpOption(std::string_view line, std::size_t max_field_bytes, EntryMetadata *metadata)
{
    static constexpr std::string_view prefix = "#EXTVLCOPT:";
    if (!StartsWithInsensitive(line, prefix))
        return false;
    line.remove_prefix(prefix.size());
    const std::size_t separator = line.find('=');
    if (separator == std::string_view::npos)
        return true;
    const std::string key = LowerTrimmed(line.substr(0, separator));
    const std::string_view value = Trim(line.substr(separator + 1));
    if (value.size() > max_field_bytes)
        return true;
    for (char byte : value)
    {
        if (IsControl(byte) && byte != '\t')
            return true;
    }
    if (key == "http-user-agent")
    {
        metadata->http_user_agent.assign(value);
    }
    else if (key == "http-referrer" || key == "http-referer")
    {
        metadata->http_referrer.assign(value);
    }
    return true;
}

bool ParseExtinf(std::string_view line, std::size_t max_field_bytes, EntryMetadata *metadata,
                 ParseIssueCode *error)
{
    const std::string_view body = line.substr(8); // strlen("#EXTINF:")
    bool quoted = false;
    bool escaped = false;
    std::size_t comma = std::string_view::npos;
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        const char byte = body[i];
        if (escaped)
        {
            escaped = false;
        }
        else if (byte == '\\' && quoted)
        {
            escaped = true;
        }
        else if (byte == '"')
        {
            quoted = !quoted;
        }
        else if (byte == ',' && !quoted)
        {
            comma = i;
            break;
        }
    }
    if (quoted || comma == std::string_view::npos)
    {
        *error = ParseIssueCode::malformed_extinf;
        return false;
    }

    const std::string_view prefix = Trim(body.substr(0, comma));
    std::size_t duration_end = 0;
    while (duration_end < prefix.size() && !IsAsciiSpace(prefix[duration_end]))
    {
        ++duration_end;
    }
    if (!ValidDuration(prefix.substr(0, duration_end)))
    {
        *error = ParseIssueCode::malformed_extinf;
        return false;
    }

    EntryMetadata parsed;
    const std::string_view title = Trim(body.substr(comma + 1));
    if (title.size() > max_field_bytes)
    {
        *error = ParseIssueCode::attribute_too_long;
        return false;
    }
    parsed.title.assign(title.data(), title.size());

    std::size_t cursor = duration_end;
    const std::string_view attributes = prefix.substr(cursor);
    std::size_t i = 0;
    while (i < attributes.size())
    {
        while (i < attributes.size() && IsAsciiSpace(attributes[i]))
        {
            ++i;
        }
        if (i == attributes.size())
        {
            break;
        }
        const std::size_t key_start = i;
        while (i < attributes.size() && IsAttributeKeyChar(attributes[i]))
        {
            ++i;
        }
        if (key_start == i)
        {
            *error = ParseIssueCode::malformed_extinf;
            return false;
        }
        std::string key;
        key.reserve(i - key_start);
        for (std::size_t k = key_start; k < i; ++k)
        {
            key.push_back(LowerAscii(attributes[k]));
        }
        while (i < attributes.size() && IsAsciiSpace(attributes[i]))
        {
            ++i;
        }
        if (i == attributes.size() || attributes[i] != '=')
        {
            *error = ParseIssueCode::malformed_extinf;
            return false;
        }
        ++i;
        while (i < attributes.size() && IsAsciiSpace(attributes[i]))
        {
            ++i;
        }

        std::string value;
        if (i < attributes.size() && attributes[i] == '"')
        {
            ++i;
            bool closed = false;
            while (i < attributes.size())
            {
                const char byte = attributes[i++];
                if (byte == '"')
                {
                    closed = true;
                    break;
                }
                if (byte == '\\' && i < attributes.size() &&
                    (attributes[i] == '\\' || attributes[i] == '"'))
                {
                    value.push_back(attributes[i++]);
                }
                else
                {
                    value.push_back(byte);
                }
                if (value.size() > max_field_bytes)
                {
                    *error = ParseIssueCode::attribute_too_long;
                    return false;
                }
            }
            if (!closed)
            {
                *error = ParseIssueCode::malformed_extinf;
                return false;
            }
        }
        else
        {
            const std::size_t value_start = i;
            while (i < attributes.size() && !IsAsciiSpace(attributes[i]))
            {
                ++i;
            }
            if (i == value_start || i - value_start > max_field_bytes)
            {
                *error = i - value_start > max_field_bytes ? ParseIssueCode::attribute_too_long
                                                           : ParseIssueCode::malformed_extinf;
                return false;
            }
            value.assign(attributes.substr(value_start, i - value_start));
        }
        SetAttribute(key, std::move(value), &parsed);
    }
    *metadata = std::move(parsed);
    return true;
}

bool UrlIssue(std::string_view raw, std::size_t max_url_bytes, std::string *canonical,
              ParseIssueCode *issue)
{
    if (Trim(raw).size() > max_url_bytes)
    {
        *issue = ParseIssueCode::url_too_long;
        return false;
    }
    switch (CanonicalizeUrl(raw, canonical))
    {
    case UrlError::none:
        return true;
    case UrlError::too_long:
        *issue = ParseIssueCode::url_too_long;
        return false;
    case UrlError::unsupported_scheme:
        *issue = ParseIssueCode::unsupported_url_scheme;
        return false;
    case UrlError::unsafe:
        *issue = ParseIssueCode::unsafe_url;
        return false;
    case UrlError::malformed:
        *issue = ParseIssueCode::malformed_url;
        return false;
    }
    *issue = ParseIssueCode::malformed_url;
    return false;
}

bool Contains(std::vector<std::string> const &values, std::string_view value)
{
    return std::any_of(values.begin(), values.end(),
                       [value](const std::string &item) { return item == value; });
}

void AddBounded(std::vector<std::string> *values, std::string_view value, std::size_t limit)
{
    if (value.empty() || values->size() >= limit || Contains(*values, value))
    {
        return;
    }
    values->emplace_back(value);
}

void MergeChannel(Channel *existing, const EntryMetadata &metadata, std::string_view canonical_url,
                  std::size_t max_alternate_urls, std::size_t max_alternate_groups)
{
    if (existing->url != canonical_url && !Contains(existing->alternate_urls, canonical_url))
    {
        AddBounded(&existing->alternate_urls, canonical_url, max_alternate_urls);
    }
    if (existing->name.empty() && !metadata.title.empty())
    {
        existing->name = metadata.title;
    }
    if (existing->tvg_id.empty())
        existing->tvg_id = metadata.tvg_id;
    if (existing->tvg_name.empty())
        existing->tvg_name = metadata.tvg_name;
    if (existing->tvg_logo.empty())
        existing->tvg_logo = metadata.tvg_logo;
    if (existing->tvg_country.empty())
        existing->tvg_country = metadata.tvg_country;
    if (existing->tvg_language.empty())
        existing->tvg_language = metadata.tvg_language;
    if (existing->http_user_agent.empty())
        existing->http_user_agent = metadata.http_user_agent;
    if (existing->http_referrer.empty())
        existing->http_referrer = metadata.http_referrer;
    if (!metadata.group_title.empty())
    {
        if (existing->group_title.empty())
        {
            existing->group_title = metadata.group_title;
        }
        else if (existing->group_title != metadata.group_title)
        {
            AddBounded(&existing->alternate_group_titles, metadata.group_title,
                       max_alternate_groups);
        }
    }
}

} // namespace

CatalogState ParseExtendedM3u(std::string_view input, std::uint64_t source_id,
                              const ParseLimits &limits, ParseReport *report)
{
    ParseReport local_report;
    report = report == nullptr ? &local_report : report;
    *report = ParseReport{};
    const EffectiveLimits bounded = ClampLimits(limits);
    CatalogState catalog;
    catalog.source_id = source_id;
    if (input.size() > bounded.max_playlist_bytes)
    {
        report->input_too_large = true;
        AddDiagnostic(report, bounded, 0, ParseIssueCode::input_too_large);
        return catalog;
    }

    const std::size_t expected_channels = std::min(bounded.max_channels, input.size() / 160u + 1u);
    catalog.channels.reserve(expected_channels);
    std::unordered_map<std::string, std::size_t> channels_by_tvg_id;
    std::unordered_map<std::string, std::size_t> channels_by_url;
    channels_by_tvg_id.reserve(expected_channels);
    channels_by_url.reserve(expected_channels);

    bool pending = false;
    PendingEntry entry;
    std::size_t position = 0;
    std::size_t line_number = 0;
    while (position < input.size())
    {
        const std::size_t line_start = position;
        const std::size_t newline = input.find('\n', position);
        const std::size_t line_end = newline == std::string_view::npos ? input.size() : newline;
        position = newline == std::string_view::npos ? input.size() : newline + 1;
        ++line_number;
        ++report->lines_seen;

        std::string_view line = input.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if (line_number == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xefu &&
            static_cast<unsigned char>(line[1]) == 0xbbu &&
            static_cast<unsigned char>(line[2]) == 0xbfu)
        {
            line.remove_prefix(3);
        }
        if (line.size() > bounded.max_record_bytes)
        {
            pending = false;
            AddDiagnostic(report, bounded, line_number, ParseIssueCode::overlong_record);
            continue;
        }
        line = Trim(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '#')
        {
            if (StartsWithInsensitive(line, "#EXTINF:"))
            {
                if (pending)
                {
                    AddDiagnostic(report, bounded, entry.line, ParseIssueCode::missing_url);
                    pending = false;
                }
                ParseIssueCode error = ParseIssueCode::malformed_extinf;
                EntryMetadata parsed;
                if (ParseExtinf(line, bounded.max_field_bytes, &parsed, &error))
                {
                    entry.metadata = std::move(parsed);
                    entry.line = static_cast<std::uint32_t>(std::min<std::size_t>(
                        line_number, std::numeric_limits<std::uint32_t>::max()));
                    pending = true;
                }
                else
                {
                    AddDiagnostic(report, bounded, line_number, error);
                }
            }
            else if (pending)
            {
                (void)SetHttpOption(line, bounded.max_field_bytes, &entry.metadata);
            }
            continue;
        }
        if (!pending)
        {
            AddDiagnostic(report, bounded, line_number, ParseIssueCode::url_without_extinf);
            continue;
        }

        std::string canonical_url;
        ParseIssueCode url_issue = ParseIssueCode::malformed_url;
        if (!UrlIssue(line, bounded.max_url_bytes, &canonical_url, &url_issue))
        {
            AddDiagnostic(report, bounded, line_number, url_issue);
            pending = false;
            continue;
        }

        Channel channel;
        channel.source_id = source_id;
        channel.url = canonical_url;
        channel.tvg_id = entry.metadata.tvg_id;
        channel.tvg_name = entry.metadata.tvg_name;
        channel.tvg_logo = entry.metadata.tvg_logo;
        channel.group_title = entry.metadata.group_title;
        channel.tvg_country = entry.metadata.tvg_country;
        channel.tvg_language = entry.metadata.tvg_language;
        channel.http_user_agent = entry.metadata.http_user_agent;
        channel.http_referrer = entry.metadata.http_referrer;
        channel.name = entry.metadata.title.empty() ? channel.tvg_name : entry.metadata.title;
        if (channel.name.empty())
        {
            channel.name = channel.url;
        }
        channel.source_line = entry.line;

        const std::string normalized_tvg_id = LowerTrimmed(channel.tvg_id);
        std::string identity =
            normalized_tvg_id.empty() ? "url:" + channel.url : "tvg:" + normalized_tvg_id;
        channel.id = StableId(source_id, identity);

        std::size_t existing_index = catalog.channels.size();
        if (!normalized_tvg_id.empty())
        {
            const auto by_id = channels_by_tvg_id.find(normalized_tvg_id);
            if (by_id != channels_by_tvg_id.end())
                existing_index = by_id->second;
        }
        const auto by_url = channels_by_url.find(channel.url);
        if (by_url != channels_by_url.end())
            existing_index = std::min(existing_index, by_url->second);

        if (existing_index < catalog.channels.size())
        {
            Channel &existing = catalog.channels[existing_index];
            MergeChannel(&existing, entry.metadata, channel.url, bounded.max_alternate_urls,
                         bounded.max_alternate_groups);
            if (!normalized_tvg_id.empty())
                channels_by_tvg_id.emplace(normalized_tvg_id, existing_index);
            channels_by_url.emplace(channel.url, existing_index);
            ++report->duplicates;
        }
        else
        {
            if (catalog.channels.size() >= bounded.max_channels)
            {
                AddDiagnostic(report, bounded, line_number, ParseIssueCode::catalog_full);
                pending = false;
                continue;
            }
            const std::size_t inserted = catalog.channels.size();
            catalog.channels.push_back(std::move(channel));
            if (!normalized_tvg_id.empty())
                channels_by_tvg_id.emplace(normalized_tvg_id, inserted);
            channels_by_url.emplace(catalog.channels.back().url, inserted);
            ++report->accepted;
        }
        pending = false;
    }
    if (pending)
    {
        AddDiagnostic(report, bounded, entry.line, ParseIssueCode::missing_url);
    }
    return catalog;
}

} // namespace iptv
