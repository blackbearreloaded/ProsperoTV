/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_VP9_PACKET_H
#define IPTV_VP9_PACKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPTV_VP9_MAX_SUPERFRAME_FRAMES 8u

typedef struct iptv_vp9_frame_slice {
    const uint8_t *data;
    size_t bytes;
} iptv_vp9_frame_slice_t;

typedef struct iptv_vp9_packet {
    iptv_vp9_frame_slice_t frames[IPTV_VP9_MAX_SUPERFRAME_FRAMES];
    uint32_t count;
} iptv_vp9_packet_t;

typedef struct iptv_vp9_frame_flags {
    uint8_t profile;
    uint8_t show_frame;
    uint8_t show_existing_frame;
    uint8_t displayable;
} iptv_vp9_frame_flags_t;

/* Splits a raw coded packet using the standard trailing VP9 superframe index.
 * A packet without an index is returned as one frame. Returned slices borrow
 * the caller's storage and remain valid only while that storage is valid. */
int iptv_vp9_split_packet(const void *data, size_t bytes,
                          iptv_vp9_packet_t *packet);

/* Reads only the fixed prefix of the VP9 uncompressed header needed by the
 * decoder queue. expected_profile must be 0..3. */
int iptv_vp9_read_frame_flags(const void *data, size_t bytes,
                              uint32_t expected_profile,
                              iptv_vp9_frame_flags_t *flags);

#ifdef __cplusplus
}
#endif

#endif
