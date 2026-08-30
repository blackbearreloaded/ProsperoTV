/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_MEMORY_H
#define IPTV_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Catalog parsing creates many small, long-lived strings. On PS5 they are
 * grouped into one mmap-backed arena so the libc small-allocation heap is not
 * exhausted. The arena retires after the operation and unmaps itself when the
 * last object allocated from it is destroyed. */
int iptv_catalog_arena_begin(size_t capacity);
void iptv_catalog_arena_end(void);

#ifdef __cplusplus
}
#endif

#endif
