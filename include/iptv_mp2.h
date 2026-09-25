/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_MP2_H
#define IPTV_MP2_H

#include <stddef.h>
#include <stdint.h>

/* MPEG-1/2 Layer II, fixed bitrate. Free-format and other layers are rejected. */
static inline size_t iptv_mp2_frame_info(const uint8_t *data, size_t bytes,
                                        uint32_t *rate, uint32_t *channels)
{
    static const uint16_t mpeg1[] = {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
    static const uint16_t mpeg2[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const uint32_t rates[] = {44100,48000,32000};
    if (!data || bytes < 4 || data[0] != 0xff || (data[1] & 0xe0) != 0xe0 ||
        ((data[1] >> 1) & 3) != 2)
        return 0;
    const unsigned version = (data[1] >> 3) & 3;
    const unsigned index = (data[2] >> 2) & 3;
    if (version < 2 || index == 3 || (data[3] & 3) == 2)
        return 0;
    const uint32_t kbps = (version == 3 ? mpeg1 : mpeg2)[data[2] >> 4];
    if (!kbps)
        return 0;
    *rate = rates[index] / (version == 3 ? 1u : 2u);
    *channels = (data[3] >> 6) == 3 ? 1u : 2u;
    return 144000u * kbps / *rate + ((data[2] >> 1) & 1u);
}
#endif
