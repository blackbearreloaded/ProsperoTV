/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_NATIVE_AGC_PRESENT_H
#define IPTV_NATIVE_AGC_PRESENT_H

#include <stddef.h>
#include <stdint.h>
#include "iptv_color.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct iptv_native_video_overlay
    {
        uint32_t codec;
        uint32_t width;
        uint32_t height;
        uint32_t fps_x100;
        uint32_t bitrate_kbps;
        uint32_t show_controls;
        uint64_t pts_us;
        uint32_t field;       /* 0: full picture; 1/2: reconstruct top/bottom field. */
        uint32_t cpu_written; /* Copy and flush CPU-decoded surfaces before the GPU reads. */
        iptv_color_info_t color;
    } iptv_native_video_overlay_t;

    /* Register only between foreground sessions; the player joins its workers
     * before it returns. A NULL surface queries visibility. A real surface is a
     * presenter-owned copy, valid only during this call. Return 1 if drawn.
     * hdr describes the source encoding: 0=SDR, 1=PQ, 2=HLG, even when the
     * output subsequently tone maps the composed surface to SDR. */
    typedef int (*iptv_native_osd_t)(void *context, void *surface, size_t bytes, uint32_t pitch,
                                     uint32_t surface_height, uint32_t width, uint32_t height,
                                     uint32_t depth, uint64_t pts_us, int hdr);
    void iptv_native_agc_set_osd(iptv_native_osd_t draw, void *context);

    int32_t iptv_native_agc_present_nv12(const void *source, size_t source_bytes, uint32_t pitch,
                                         uint32_t surface_height, uint32_t visible_width,
                                         uint32_t visible_height,
                                         const iptv_native_video_overlay_t *overlay);
    int32_t iptv_native_agc_present_nv12_deferred(const void *source, size_t source_bytes,
                                                  uint32_t pitch, uint32_t surface_height,
                                                  uint32_t visible_width, uint32_t visible_height,
                                                  const iptv_native_video_overlay_t *overlay);
    int32_t iptv_native_agc_present_yuv_deferred(const void *source, size_t source_bytes,
                                                 uint32_t pitch, uint32_t surface_height,
                                                 uint32_t visible_width, uint32_t visible_height,
                                                 uint32_t bit_depth,
                                                 const iptv_native_video_overlay_t *overlay);
    int32_t iptv_native_agc_present_finish_frame(void);
    int32_t iptv_native_agc_loading_start(void);
    void iptv_native_agc_loading_stop(void);
    void iptv_native_agc_set_overlay_enabled(int enabled);
    int iptv_native_agc_overlay_enabled(void);
    int32_t iptv_native_agc_present_drain(void);
    void iptv_native_agc_present_set_cancelled(int cancelled);
    int32_t iptv_native_agc_present_shutdown(void);
    int iptv_native_agc_hdr_active(void);
    /* Disposable-title scripts only: exercise SDR fallback and sample completed output. */
    void iptv_native_agc_set_color_test(int force_sdr, int sample_output);

#ifdef __cplusplus
}
#endif

#endif
