// ProsperoTV - Where the diagnostic log goes on the console: logs/debug-trace.txt.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace tv::diag
{

// Takes the lines of ptv::diag from now on. Call once, after the app's folders
// are settled and it may start threads. Nothing is written while the log is off.
void start(const std::string &logs_dir);
// Writes what is waiting, now: before the player takes over, and after it.
void flush();

} // namespace tv::diag
