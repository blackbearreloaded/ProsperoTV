// ProsperoTV - Settings and caches for the console user who launched the app.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace ptv
{
struct ProfilePaths
{
    std::string config, cache, logs;
};
// Call before opening databases, pairing the remote or starting workers.
// Legacy shared settings belong to the first console user to launch this version.
// A failed identity or migration never falls back to shared settings.
bool prepare_profile(const std::string &root, const std::string &legacy_config,
                     std::int32_t user_id, ProfilePaths &paths, std::string &error);
} // namespace ptv
