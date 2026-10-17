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
        /* The sound being played: its transport stream type in bits 0-7, 1 for the console's
         * decoder or 2 for software in bits 8-15, the source's channels from bit 16; 0 for
         * none. */
        uint32_t audio;
    } iptv_native_video_overlay_t;

    /* Register only between foreground sessions; the player joins its workers
     * before it returns. A NULL surface queries visibility. A real surface is a
     * presenter-owned copy, valid only during this call. Return 1 if drawn.
     * hdr describes the source encoding: 0=SDR, 1=PQ, 2=HLG, even when the
     * output subsequently tone maps the composed surface to SDR. */
    typedef int (*iptv_native_osd_t)(void *context, void *surface, size_t bytes, uint32_t pitch,
                                     uint32_t surface_height, uint32_t width, uint32_t height,
                                     uint32_t depth, uint64_t pts_us, int hdr, int buffering);
    enum
    {
        IPTV_PICTURE_FIT,
        IPTV_PICTURE_STRETCH,
        IPTV_PICTURE_4_3,
        IPTV_PICTURE_16_9,
        IPTV_PICTURE_FILL,
        IPTV_PICTURE_ZOOM_125,
        IPTV_PICTURE_ZOOM_150
    };
    void iptv_native_agc_set_picture_mode(unsigned mode);
    void iptv_native_agc_set_osd(iptv_native_osd_t draw, void *context);
    /* Controls drawn at the screen's own resolution rather than the picture's: `color` holds
     * premultiplied colour and `mask` coverage, each limited-range YUV 4:2:0 of `width` x
     * `height` with rows `pitch` samples apart and `surface_height` rows to a plane, in the
     * picture's sample depth (Main10 in the low bits). Both start transparent (black) and keep
     * what was drawn between frames; `cleared` says they were made transparent again. `hdr`
     * is 1 when the screen is PQ. Returns bit 0 when anything is on them and bit 1 when they
     * changed. */
    typedef int (*iptv_native_screen_osd_t)(void *context, void *color, void *mask, size_t bytes,
                                            uint32_t pitch, uint32_t surface_height, uint32_t width,
                                            uint32_t height, uint32_t depth, int hdr, int cleared);
    void iptv_native_agc_set_screen_osd(iptv_native_screen_osd_t draw, void *context);

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
    /* When the picture iptv_native_agc_present_finish_frame last saw reach the screen, as
     * CLOCK_MONOTONIC microseconds from the flip's own time; 0 when unknown. */
    uint64_t iptv_native_agc_last_flip_us(void);
    /* How long the presenter's last start took, in microseconds: [0] in all, [1] the shaders,
     * [2] opening the video output, [3] the framebuffers, [4] the controls' planes. Zeros
     * until it has started once. */
    void iptv_native_agc_last_startup_us(uint32_t steps[5]);
    int32_t iptv_native_agc_loading_start(void);
    void iptv_native_agc_loading_stop(void);
    void iptv_native_agc_set_overlay_enabled(int enabled);
    /* The stats line stays on screen instead of going after five seconds. */
    void iptv_native_agc_set_overlay_always(int always);
    int iptv_native_agc_overlay_enabled(void);
    int32_t iptv_native_agc_present_drain(void);
    void iptv_native_agc_present_set_cancelled(int cancelled);
    int32_t iptv_native_agc_present_shutdown(void);
    /* Keep the completed scanout during a same-video decoder reopen. Clear on exit. */
    void iptv_native_agc_hold_frame(int hold);
    int32_t iptv_native_agc_show_buffering(void);
    int iptv_native_agc_hdr_active(void);
    /* Disposable-title scripts only: exercise SDR fallback and sample completed output. */
    void iptv_native_agc_set_color_test(int force_sdr, int sample_output);

#ifdef __cplusplus
}
#endif

#endif
