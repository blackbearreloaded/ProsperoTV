// ProsperoTV - The few choices the player makes about the interface itself.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace ptv
{

struct Settings
{
    // How large the menus are drawn. The video keeps its own size either way.
    enum Resolution : int
    {
        kBest = 0,   // 2160p where the console offers it, else 1080p
        kFullHd = 1, // always 1080p
    };

    bool reduced_motion = false;
    bool sounds = true;
    int volume = 100;
    int resolution = kBest;
    // The diagnostic log (tv/diag.hpp). Off unless the viewer turns it on.
    bool diagnostics = false;

    bool operator==(const Settings &) const = default;
};

// Reads the settings file of a data folder; anything missing keeps its default.
Settings load_settings(const std::string &data_dir);
// Writes it through a temporary file. False when the folder refused it.
bool save_settings(const std::string &data_dir, const Settings &settings);

} // namespace ptv
