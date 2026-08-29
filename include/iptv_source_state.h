/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_SOURCE_STATE_H
#define IPTV_SOURCE_STATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace iptv {

inline constexpr char kDefaultCustomSourcePath[] =
    "/download0/iptv-custom-source-v1.txt";
inline constexpr char kDefaultActiveSourcePath[] =
    "/download0/iptv-active-source-v1.txt";
inline constexpr std::size_t kMaxCustomSourceUrlBytes = 1020u;

enum class SourceStateStatus : std::uint8_t {
    ok,
    invalid_argument,
    not_found,
    too_large,
    io_error,
    corrupt,
};

SourceStateStatus SaveCustomSourceUrl(
    const std::string& path, std::string_view url);
SourceStateStatus LoadCustomSourceUrl(
    const std::string& path, std::string* url);

SourceStateStatus SaveCustomSourceUrl(std::string_view url);
SourceStateStatus LoadCustomSourceUrl(std::string* url);
SourceStateStatus SaveActiveSource(const std::string& path, bool custom);
SourceStateStatus LoadActiveSource(const std::string& path, bool* custom);
SourceStateStatus SaveActiveSource(bool custom);
SourceStateStatus LoadActiveSource(bool* custom);
std::uint64_t CustomSourceId(std::string_view url);

}  // namespace iptv

#endif
