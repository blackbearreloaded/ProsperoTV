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
    void iptv_remote_set_pairing_store(const char *path);
    bool iptv_remote_begin_pairing(void);
    void iptv_remote_cancel_pairing(void);
    const char *iptv_remote_url(void);
    const char *iptv_remote_pairing_code(void);
    unsigned iptv_remote_pairing_seconds(void);
    unsigned iptv_remote_paired_count(void);
    bool iptv_remote_forget_phones(void);
    void iptv_remote_set_volume(unsigned volume);
    void iptv_remote_set_volume_handler(bool (*save)(unsigned, void *), void *context);
    void iptv_remote_set_icon(const char *path);
    void iptv_remote_stop(void);
    void iptv_remote_poll(void);
    bool iptv_remote_take_connected(void);
    bool iptv_remote_next(iptv_input_event_t *event);
    bool iptv_remote_search(char text[IPTV_IME_MAX_TEXT_BYTES]);
    void iptv_remote_enable_search(bool enabled);
    /* Playback-only handler: 1 added, 0 removed, -1 save failed. Clear before
     * its context expires. Called synchronously by the polling thread. */
    void iptv_remote_set_playback_favorite(int (*toggle)(void *), void *context);
    const char *iptv_remote_hint(void);
#ifdef __cplusplus
}
#endif
#endif
