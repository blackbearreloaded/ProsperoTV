/* ps5-native-app-boilerplate - Bounded transport audio framing.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IPTV_AUDIO_FRAME_H
#define IPTV_AUDIO_FRAME_H
#include <stddef.h>
#include <stdint.h>

/* Canonical TS types; private-stream descriptors are resolved by the demuxer. */
static inline int iptv_audio_software_type(uint32_t type)
{
    return type == 0x81u || type == 0x87u || type == 0x11u;
}

/* Zero means invalid sync/header. Callers wait for seven bytes before probing.
 * Frame size may exceed available bytes; callers must wait for the full frame. */
static inline size_t iptv_audio_frame_info(uint32_t type, const uint8_t *p, size_t bytes,
                                           uint32_t *rate, uint32_t *samples)
{
    static const uint32_t rates[] = {48000, 44100, 32000};
    static const uint16_t kbps[] = {32,  40,  48,  56,  64,  80,  96,  112, 128, 160,
                                    192, 224, 256, 320, 384, 448, 512, 576, 640};
    *rate = *samples = 0;
    if (!p || bytes < 7)
        return 0;
    if (type == 0x11u)
        return p[0] == 0x56u && (p[1] & 0xe0u) == 0xe0u ? 3u + ((size_t)(p[1] & 31u) << 8) + p[2]
                                                        : 0;
    if (p[0] != 0x0bu || p[1] != 0x77u)
        return 0;
    const unsigned bsid = p[5] >> 3;
    if (bsid <= 10u)
    {
        const unsigned code = p[4] & 63u, frequency = p[4] >> 6;
        if (frequency == 3u || code > 37u)
            return 0;
        const unsigned bitrate = kbps[code >> 1];
        const unsigned words = frequency == 0   ? bitrate * 2u
                               : frequency == 2 ? bitrate * 3u
                                                : bitrate * 320u / 147u + (code & 1u);
        *rate = rates[frequency] >> (bsid > 8u ? bsid - 8u : 0u);
        *samples = 1536;
        return words * 2u;
    }
    if (bsid > 16u || (p[2] >> 6) == 3u)
        return 0;
    const unsigned frequency = p[4] >> 6;
    const unsigned blocks[] = {1, 2, 3, 6};
    if (frequency == 3u)
    {
        const unsigned frequency2 = (p[4] >> 4) & 3u;
        if (frequency2 == 3u)
            return 0;
        *rate = rates[frequency2] / 2u;
        *samples = 1536;
    }
    else
    {
        *rate = rates[frequency];
        *samples = 256u * blocks[(p[4] >> 4) & 3u];
    }
    const size_t frame_bytes = 2u * (1u + ((size_t)(p[2] & 7u) << 8) + p[3]);
    return frame_bytes >= 7u ? frame_bytes : 0;
}
#endif
