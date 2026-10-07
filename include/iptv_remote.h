/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IPTV_REMOTE_H
#define IPTV_REMOTE_H
#include "iptv_input.h"
#include "iptv_ime.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Main/input thread only. Nonblocking, bounded work on each poll. */
    bool iptv_remote_start(unsigned short port);
    void iptv_remote_stop(void);
    void iptv_remote_poll(void);
    bool iptv_remote_next(iptv_input_event_t *event);
    bool iptv_remote_search(char text[IPTV_IME_MAX_TEXT_BYTES]);
    void iptv_remote_enable_search(bool enabled);
    const char *iptv_remote_hint(void);
#ifdef __cplusplus
}
#endif
#endif
