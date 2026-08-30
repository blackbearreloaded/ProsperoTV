/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_NATIVE_AGC_PRESENT_H
#define IPTV_NATIVE_AGC_PRESENT_H

#include <stddef.h>
#include <stdint.h>

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
    } iptv_native_video_overlay_t;

    int32_t iptv_native_agc_present_nv12(const void *source, size_t source_bytes, uint32_t pitch,
                                         uint32_t surface_height, uint32_t visible_width,
                                         uint32_t visible_height,
                                         const iptv_native_video_overlay_t *overlay);
    int32_t iptv_native_agc_loading_start(void);
    void iptv_native_agc_loading_stop(void);
    void iptv_native_agc_set_overlay_enabled(int enabled);
    int iptv_native_agc_overlay_enabled(void);
    int32_t iptv_native_agc_present_drain(void);
    void iptv_native_agc_present_set_cancelled(int cancelled);
    int32_t iptv_native_agc_present_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
