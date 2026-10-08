// ProsperoTV - The few choices the player makes about the interface itself.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/settings.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ptv
{

namespace
{

constexpr char kName[] = "/prosperotv-interface-v1.txt";

} // namespace

Settings load_settings(const std::string &data_dir)
{
    Settings settings;
    std::FILE *file = std::fopen((data_dir + kName).c_str(), "rb");
    if (file == nullptr)
        return settings;
    char line[96];
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        int value = 0;
        if (std::sscanf(line, "reduced_motion=%d", &value) == 1)
            settings.reduced_motion = value != 0;
        else if (std::sscanf(line, "sounds=%d", &value) == 1)
            settings.sounds = value != 0;
        else if (std::sscanf(line, "volume=%d", &value) == 1)
            settings.volume = std::clamp(value, 0, 100);
        else if (std::sscanf(line, "resolution=%d", &value) == 1)
            settings.resolution = value == Settings::kFullHd ? Settings::kFullHd : Settings::kBest;
        else if (std::sscanf(line, "diagnostics=%d", &value) == 1)
            settings.diagnostics = value != 0;
    }
    std::fclose(file);
    return settings;
}

bool save_settings(const std::string &data_dir, const Settings &settings)
{
    const std::string target = data_dir + kName;
    const std::string temporary = target + ".tmp";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr)
        return false;
    std::fprintf(file, "reduced_motion=%d\nsounds=%d\nresolution=%d\nvolume=%d\ndiagnostics=%d\n",
                 settings.reduced_motion ? 1 : 0, settings.sounds ? 1 : 0, settings.resolution,
                 std::clamp(settings.volume, 0, 100), settings.diagnostics ? 1 : 0);
    const bool written = std::ferror(file) == 0 && std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
    {
        std::remove(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), target.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace ptv
