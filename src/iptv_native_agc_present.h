/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_NATIVE_AGC_PRESENT_H
#define IPTV_NATIVE_AGC_PRESENT_H

#include <stddef.h>
#include <stdint.h>

int32_t iptv_native_agc_present_nv12(const void *source, size_t source_bytes, uint32_t pitch,
                                     uint32_t surface_height, uint32_t visible_width,
                                     uint32_t visible_height);
int32_t iptv_native_agc_present_drain(void);
void iptv_native_agc_present_set_cancelled(int cancelled);
int32_t iptv_native_agc_present_shutdown(void);

#endif
