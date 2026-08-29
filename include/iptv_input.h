/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_INPUT_H
#define IPTV_INPUT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Edge-triggered actions emitted by the native DualSense poller. */
typedef enum {
    IPTV_INPUT_CROSS,
    IPTV_INPUT_CIRCLE,
    IPTV_INPUT_SQUARE,
    IPTV_INPUT_TRIANGLE,
    IPTV_INPUT_OPTIONS,
    IPTV_INPUT_L1,
    IPTV_INPUT_R1,
    IPTV_INPUT_TOUCHPAD,
    IPTV_INPUT_UP,
    IPTV_INPUT_DOWN,
    IPTV_INPUT_LEFT,
    IPTV_INPUT_RIGHT,
    IPTV_INPUT_COUNT
} iptv_input_action_t;

typedef struct {
    iptv_input_action_t action;
    bool pressed;
} iptv_input_event_t;

/* Safe defaults: 64/255 stick dead zone, 350 ms initial delay, 110 ms repeat. */
#define IPTV_INPUT_STICK_DEADZONE 64U
#define IPTV_INPUT_REPEAT_DELAY_MS 350U
#define IPTV_INPUT_REPEAT_MS 110U

bool iptv_input_init(void);
void iptv_input_poll(void);
bool iptv_input_next(iptv_input_event_t* event);
bool iptv_input_pressed(iptv_input_action_t action);
void iptv_input_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
