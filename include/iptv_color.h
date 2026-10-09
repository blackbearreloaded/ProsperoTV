/* ProsperoTV - Color signalling shared by decoded pictures and presentation.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

typedef struct iptv_hdr_metadata
{
    uint32_t flags;           /* 1: mastering display, 2: content light levels. */
    uint16_t primaries[3][2]; /* RGB order, in units of 1/50000. */
    uint16_t white[2];
    uint32_t min_luminance, max_luminance; /* Units of 1/10000 cd/m2. */
    uint16_t max_cll, max_fall;            /* cd/m2; zero means unknown. */
} iptv_hdr_metadata_t;

typedef struct iptv_color_info
{
    uint32_t primaries; /* H.273 values; 2 means unspecified. */
    uint32_t transfer;
    uint32_t matrix;
    uint32_t range; /* 0: unspecified, 1: limited, 2: full. */
    iptv_hdr_metadata_t hdr;
} iptv_color_info_t;

static inline int iptv_color_is_hdr10(iptv_color_info_t color)
{
    return color.primaries == 9 && color.transfer == 16 && color.matrix == 9 && color.range == 1;
}

static inline int iptv_color_is_hlg(iptv_color_info_t color)
{
    return color.primaries == 9 && color.transfer == 18 && color.matrix == 9 && color.range == 1;
}

#ifdef __cplusplus
extern "C"
{
#endif
    /* 203-nit sRGB UI colors in limited BT.2020: PQ or reference-display HLG. */
    void iptv_color_ui_yuv(uint8_t red, uint8_t green, uint8_t blue, uint16_t output[3], int hlg);
    uint16_t iptv_color_ui_luma(uint8_t limited_sdr, int hlg);
    /* Bounded SDR preview: linear BT.2020-to-709 conversion and a 203-nit Reinhard
     * shoulder. The full-screen HDR path preserves the original PQ signal. */
    void iptv_color_pq_to_srgb(float red, float green, float blue, uint8_t output[3]);
    /* BT.2100 HLG at the 1000-nit reference display, including luminance OOTF. */
    void iptv_color_hlg_to_srgb(float red, float green, float blue, uint8_t output[3]);
#ifdef __cplusplus
}
#endif
