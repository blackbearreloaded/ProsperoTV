// ProsperoTV - Where the app keeps its files, and the request for filesystem access.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// With filesystem access (elevation/elevation.hpp) the app uses real console
// paths:
//
//   the app's folder   where it is installed, normally /data/homebrew/<TITLE>
//   /data/prosperotv   config/  settings, sources, favorites, recent channels
//                      cache/   the downloaded channel lists
//                      logs/    app.log, the player's receipts
//
// Without it (no resident Lapy service and no payload loader, or the request
// was refused) everything stays where the released app keeps it: /app0 and the
// title's own storage, /download0.

#pragma once

#include <string>

namespace tv::storage
{

// First thing in main, while the process still has one thread: reads what an
// earlier version kept in the title's storage, asks for filesystem access,
// settles every path, makes the folders, writes the carried files where the
// new place has none, and moves the log.
void initialize();
// Select the launching console user's folders before any worker starts.
bool select_profile(int user_id, std::string &error);

// Whether filesystem access was granted, and what the request answered
// (elevation::Status as a number; -1 when it was not asked).
bool elevated();
int status();
// How it was granted: "existing", "resident", "helper" or "none".
const char *route();

const std::string &app_dir();
const std::string &config_dir();
const std::string &cache_dir();
const std::string &logs_dir();
// A file of the app's own folder.
std::string app_file(const std::string &relative);

} // namespace tv::storage
