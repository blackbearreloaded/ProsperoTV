/* ProsperoTV - bounded stereo loudness leveling at the 48 kHz output sink.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IPTV_AUDIO_NORMALIZE_H
#define IPTV_AUDIO_NORMALIZE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
#define IPTV_NORMALIZE_RATE 48000u
#define IPTV_NORMALIZE_WINDOWS 30u

    typedef struct iptv_audio_normalizer
    {
        double shelf[2][2], highpass[2][2];
        double windows[IPTV_NORMALIZE_WINDOWS], window_sum, energy_sum;
        double gain_db, limiter_gain, loudness_lufs;
        uint32_t window_frames, window_count, window_position;
        uint64_t limited_blocks;
        int enabled, meter_valid;
    } iptv_audio_normalizer_t;

    /* Resets all channel history. Disabled processing is bit-for-bit transparent. */
    void iptv_audio_normalizer_reset(iptv_audio_normalizer_t *state);
    /* Interleaved stereo S16 only, after downmix/resampling and before user volume.
     * K-weighted rolling loudness control, not a certified integrated R128 meter.
     * Target -23 LUFS; gain [-18,+18] dB, rise 3 dB/s, fall 12 dB/s.
     * Gate -50 LUFS; linked block limiter at -1 dBFS (sample peak, not true peak). */
    void iptv_audio_normalizer_process(iptv_audio_normalizer_t *state, int16_t *pcm, size_t frames,
                                       int enabled);
#ifdef __cplusplus
}
#endif
#endif
