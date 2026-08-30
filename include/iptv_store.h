/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_STORE_H
#define IPTV_STORE_H

#include "iptv_catalog.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace iptv {

inline constexpr std::size_t kDefaultMaxStoreBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kDefaultMaxStoredRecordBytes = 32u * 1024u;
inline constexpr char kDefaultPlaybackHistoryPath[] =
    "/download0/prosperotv-playback-history.sqlite3";

struct StoreLimits {
    std::size_t max_file_bytes = kDefaultMaxStoreBytes;
    std::size_t max_record_bytes = kDefaultMaxStoredRecordBytes;
    std::size_t max_string_bytes = kDefaultMaxFieldBytes;
    std::size_t max_url_bytes = kDefaultMaxUrlBytes;
    std::size_t max_channels = kDefaultMaxChannels;
    std::size_t max_alternate_urls = kDefaultMaxAlternateUrls;
    std::size_t max_alternate_groups = kDefaultMaxAlternateGroups;
};

enum class StoreStatus : std::uint8_t {
    ok,
    invalid_argument,
    not_found,
    too_large,
    io_error,
    corrupt,
    unsupported_version,
};

struct StoreReport {
    StoreStatus status = StoreStatus::ok;
    std::size_t records = 0;
    std::size_t bytes = 0;
    std::uint64_t saved_unix = 0;
};

StoreStatus SaveCatalog(const std::string& path,
                        const CatalogState& catalog,
                        const StoreLimits& limits = StoreLimits{},
                        StoreReport* report = nullptr);

StoreStatus LoadCatalog(const std::string& path,
                        CatalogState* catalog,
                        const StoreLimits& limits = StoreLimits{},
                        StoreReport* report = nullptr);

StoreStatus RecordPlaybackResult(const std::string& path,
                                 std::uint64_t source_id,
                                 const std::string& channel_id,
                                 bool playable,
                                 int result);

StoreStatus LoadPlaybackResults(const std::string& path,
                                std::uint64_t source_id,
                                CatalogState* catalog,
                                const StoreLimits& limits = StoreLimits{});

}  // namespace iptv

#endif
