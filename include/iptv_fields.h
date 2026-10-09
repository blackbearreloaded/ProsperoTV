/* ProsperoTV - Field order and spatial deinterlacing for broadcast H.264.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "iptv_color.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct iptv_field_info
    {
        uint32_t first; /* 0: progressive/unknown, 1: top, 2: bottom. */
        uint32_t count; /* Displayed fields, including repeat-first-field. */
        uint32_t duration_us;
        uint32_t field_picture; /* One access unit contains only one field. */
        iptv_color_info_t color;
    } iptv_field_info_t;
    typedef struct iptv_field_parser iptv_field_parser_t;

    /* Codec matches the stream API: 1 H.264, 2 HEVC. HEVC exports color only. */
    iptv_field_parser_t *iptv_field_parser_create(uint32_t codec);
    void iptv_field_parser_destroy(iptv_field_parser_t *parser);
    iptv_field_info_t iptv_field_parse(iptv_field_parser_t *parser, const void *data, size_t bytes);

    /* Bounded H.264 software fallback into caller-owned NV12. Returns 0 while
     * buffered, 1 for a progressive picture, 2 for interlaced, or a negative error.
     * NULL/0 drains delayed pictures. Dimensions must match the decoded picture. */
    int iptv_field_decode(iptv_field_parser_t *parser, const void *data, size_t bytes, void *output,
                          size_t output_bytes, uint32_t pitch, uint32_t surface_height,
                          uint32_t width, uint32_t height);
    void iptv_field_decoder_reset(iptv_field_parser_t *parser);

    /* Copy one NV12 field to a distinct surface, interpolating only between rows
     * from that field. Never blend two moments or modify decoder reference pixels.
     * Padded columns are left untouched. field is 0 for top, 1 for bottom. */
    int iptv_field_bob(void *output, size_t output_bytes, const void *input, size_t input_bytes,
                       uint32_t pitch, uint32_t height, uint32_t width, uint32_t field);

#ifdef __cplusplus
}
#endif
