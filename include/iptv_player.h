/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_PLAYER_H
#define IPTV_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runs one foreground channel session after the launcher has released
 * RmlUi, SDL, and VideoOut. Returns only after every native stream resource
 * has been released so the launcher can safely be recreated. */
int iptv_player_run(const char *url, const char *channel_name);
int iptv_player_run_with_headers(const char *url, const char *channel_name,
                                 const char *user_agent, const char *referrer);

#ifdef __cplusplus
}
#endif

#endif
