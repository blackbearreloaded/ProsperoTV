/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

// PS5 Radio - SQLite compatibility boundary.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

extern "C" int fchown(int descriptor, uid_t owner, gid_t group)
{
    (void)descriptor;
    (void)owner;
    (void)group;
    return 0;
}

extern "C" struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    (void)timer;
    (void)result;
    return nullptr;
}

extern "C" int lstat(const char *path, struct stat *status)
{
    // The title sandbox rejects lstat() on /download0 with EPERM even though
    // stat() is supported. SQLite uses lstat() only to reject symlinked path
    // components here, and PS5 Radio always supplies its fixed sandbox path.
    return stat(path, status);
}
