/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_CATALOG_H
#define IPTV_CATALOG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iptv {

inline constexpr std::size_t kDefaultMaxPlaylistBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t kDefaultMaxRecordBytes = 16u * 1024u;
inline constexpr std::size_t kDefaultMaxUrlBytes = 4096u;
inline constexpr std::size_t kDefaultMaxFieldBytes = 2048u;
inline constexpr std::size_t kDefaultMaxChannels = 32768u;
inline constexpr std::size_t kDefaultMaxAlternateUrls = 3u;
inline constexpr std::size_t kDefaultMaxAlternateGroups = 4u;
inline constexpr std::size_t kDefaultMaxDiagnostics = 128u;

struct ParseLimits {
    std::size_t max_playlist_bytes = kDefaultMaxPlaylistBytes;
    std::size_t max_record_bytes = kDefaultMaxRecordBytes;
    std::size_t max_url_bytes = kDefaultMaxUrlBytes;
    std::size_t max_field_bytes = kDefaultMaxFieldBytes;
    std::size_t max_channels = kDefaultMaxChannels;
    std::size_t max_alternate_urls = kDefaultMaxAlternateUrls;
    std::size_t max_alternate_groups = kDefaultMaxAlternateGroups;
    std::size_t max_diagnostics = kDefaultMaxDiagnostics;
};

enum class ParseIssueCode : std::uint8_t {
    input_too_large,
    overlong_record,
    malformed_extinf,
    attribute_too_long,
    missing_url,
    url_without_extinf,
    url_too_long,
    unsupported_url_scheme,
    unsafe_url,
    malformed_url,
    catalog_full,
};

struct ParseDiagnostic {
    std::size_t line = 0;
    ParseIssueCode code = ParseIssueCode::malformed_extinf;
};

struct ParseReport {
    std::size_t lines_seen = 0;
    std::size_t accepted = 0;
    std::size_t duplicates = 0;
    std::size_t skipped = 0;
    bool input_too_large = false;
    std::vector<ParseDiagnostic> diagnostics;
};

struct Channel {
    std::string id;
    std::uint64_t source_id = 0;
    std::string name;
    std::string url;
    std::vector<std::string> alternate_urls;
    std::string tvg_id;
    std::string tvg_name;
    std::string tvg_logo;
    std::string group_title;
    std::vector<std::string> alternate_group_titles;
    std::string tvg_country;
    std::string tvg_language;
    std::string http_user_agent;
    std::string http_referrer;
    std::uint32_t source_line = 0;
};

struct CatalogState {
    std::uint64_t source_id = 0;
    std::vector<Channel> channels;
};

// Returns a normalized http(s) URL suitable for a playable catalog entry.
bool CanonicalizeStreamUrl(std::string_view raw, std::string* canonical);

CatalogState ParseExtendedM3u(std::string_view input,
                              std::uint64_t source_id,
                              const ParseLimits& limits = ParseLimits{},
                              ParseReport* report = nullptr);

}  // namespace iptv

#endif
