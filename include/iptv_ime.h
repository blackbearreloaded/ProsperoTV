/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_IME_H
#define IPTV_IME_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPTV_IME_MAX_TEXT_CHARACTERS 39U
#define IPTV_IME_MAX_TEXT_BYTES (IPTV_IME_MAX_TEXT_CHARACTERS * 4U + 1U)
#define IPTV_IME_BUFFER_CHARACTERS 255U
#define IPTV_IME_BUFFER_BYTES (IPTV_IME_BUFFER_CHARACTERS * 4U + 1U)

typedef void (*iptv_ime_result_fn)(const char *text, void *user_data);

bool iptv_ime_init(void);
void iptv_ime_request(const char *initial_text, iptv_ime_result_fn callback,
                      void *user_data);
void iptv_ime_request_prompt(const char *initial_text, const char *title,
                             const char *placeholder,
                             unsigned max_text_characters,
                             iptv_ime_result_fn callback, void *user_data);
void iptv_ime_poll(void);
void iptv_ime_cancel(void);
void iptv_ime_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
