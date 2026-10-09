/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_PLAYER_H
#define IPTV_PLAYER_H
#include "iptv_stream.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Runs one foreground channel session after the launcher has released
     * RmlUi, SDL, and VideoOut. Returns only after every native stream resource
     * has been released so the launcher can safely be recreated. */
    int iptv_player_run(const char *url, const char *channel_name);
    int iptv_player_run_with_headers(const char *url, const char *channel_name,
                                     const char *user_agent, const char *referrer,
                                     int reconnect_live);
    /* Live history is independent of reconnect policy; disable it for VOD/catch-up. */
    int iptv_player_run_authenticated(const char *url, const char *channel_name,
                                      const char *user_agent, const char *referrer,
                                      const char *authorization, const char *credential_origin,
                                      unsigned stop_after_ms, int reconnect_live, int live);
    const char *iptv_player_last_error(void);
    /* Monotonic deadline shared across channel switches and URL retries; 0 disables it. */
    void iptv_player_set_sleep_deadline(uint64_t deadline_usec);
    /* Set only between sessions. Called on the player's control thread with
     * -1 for a tick,
     * otherwise a pressed iptv_input_action_t. Return 0 to leave
     * an event to the player, 1
     * to consume it, or 2 to stop for a channel switch. */
    typedef int (*iptv_player_controls_t)(void *context, int action);
    void iptv_player_set_controls(iptv_player_controls_t controls, void *context);
    typedef struct iptv_player_audio_state
    {
        iptv_stream_audio_track_t tracks[IPTV_STREAM_MAX_AUDIO_TRACKS];
        char titles[IPTV_STREAM_MAX_AUDIO_TRACKS][128];
        char languages[IPTV_STREAM_MAX_AUDIO_TRACKS][32];
        uint32_t count, selected_pid, disabled, pending;
        int32_t result;
    } iptv_player_audio_state_t;
    /* Thread-safe snapshots and requests. The demux worker applies changes
     * between chunks. Selection never accesses its session concurrently. */
    void iptv_player_audio_state(iptv_player_audio_state_t *state);
    int iptv_player_select_audio(uint32_t pid);
    typedef struct iptv_player_live_state
    {
        uint64_t first_us, last_us, position_us;
        uint32_t available, paused, expired;
    } iptv_player_live_state_t;
    /* Thread-safe requests, applied by the foreground control thread. */
    void iptv_player_live_state(iptv_player_live_state_t *state);
    int iptv_player_pause_live(int paused);
    int iptv_player_seek_live(int seconds);
    int iptv_player_go_live(void);
    /* Runs the same foreground path with a bounded automatic stop. A zero timeout
 * disables the
     * deadline. This is used by controlled hardware acceptance. */
    int iptv_player_run_controlled(const char *url, const char *channel_name,
                                   unsigned stop_after_ms);

#ifdef __cplusplus
}
#include "iptv_subtitles.h"
namespace iptv
{
// The foreground session owns this synchronized decoder and cue queue.
Subtitles &player_subtitles();
}
#endif

#endif
