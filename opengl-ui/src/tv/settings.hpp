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

    int picture_mode = 0;       // Fit, stretch, 4:3, 16:9, fill, 125%, 150%.
    int subtitle_size = 1;      // Small, medium, large.
    int subtitle_color = 0;     // White, yellow, cyan.
    int interface_language = 0; // 0 follows console, 1..9 en/es/fr/de/it/pt/nl/ar/he.
    int theme = 0;              // 0 Dusk, 1 Ocean, 2 Forest
    bool larger_text = false;
    bool high_contrast = false;
    int screensaver_minutes = 5; // 0 disables; supported delays 1, 5, 10
    bool reduced_motion = false;
    bool sounds = true;
    bool hide_failed = false;
    bool resume_last = false;
    bool live_preview = true;
    int volume = 100;
    bool audio_normalization = false;
    bool audio_surround = false;
    int resolution = kBest;
    // The diagnostic log (tv/diag.hpp). Off unless the viewer turns it on.
    bool diagnostics = false;
    // The playback stats line stays on screen instead of going after five seconds.
    bool stats_line = false;

    bool operator==(const Settings &) const = default;
};

// Reads the settings file of a data folder; anything missing keeps its default.
Settings load_settings(const std::string &data_dir);
// Writes it through a temporary file. False when the folder refused it.
bool save_settings(const std::string &data_dir, const Settings &settings);

} // namespace ptv
