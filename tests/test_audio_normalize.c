/* ProsperoTV - behavioral regression tests for loudness leveling.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_audio_normalize.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 256u
#define PI 3.14159265358979323846

static void tone(int16_t pcm[FRAMES * 2], size_t offset, double amplitude)
{
    for (size_t i = 0; i < FRAMES; ++i)
        pcm[i * 2] = pcm[i * 2 + 1] =
            (int16_t)(amplitude * sin(2.0 * PI * 1000.0 * (double)(offset + i) / 48000.0));
}

static void advance(iptv_audio_normalizer_t *n, double amplitude, unsigned seconds)
{
    int16_t pcm[FRAMES * 2];
    for (size_t i = 0; i < (size_t)seconds * 48000; i += FRAMES)
    {
        tone(pcm, i, amplitude);
        const double before = n->gain_db;
        iptv_audio_normalizer_process(n, pcm, FRAMES, 1);
        assert(n->gain_db >= -18.000001 && n->gain_db <= 18.000001);
        assert(n->gain_db - before <= 3.0 * FRAMES / 48000.0 + 0.000001);
        assert(before - n->gain_db <= 12.0 * FRAMES / 48000.0 + 0.000001);
        for (unsigned j = 0; j < FRAMES * 2; ++j)
            assert(pcm[j] >= -29203 && pcm[j] <= 29203);
    }
}

int main(void)
{
    iptv_audio_normalizer_t n;
    int16_t pcm[FRAMES * 2], original[FRAMES * 2];
    iptv_audio_normalizer_reset(&n);
    for (unsigned i = 0; i < FRAMES * 2; ++i)
        original[i] = pcm[i] = i % 2 ? INT16_MIN : INT16_MAX;
    iptv_audio_normalizer_process(&n, pcm, FRAMES, 0);
    assert(memcmp(pcm, original, sizeof(pcm)) == 0);

    /* Silence and tiny background levels must not train an upward gain. */
    memset(pcm, 0, sizeof(pcm));
    for (unsigned i = 0; i < 3000; ++i)
        iptv_audio_normalizer_process(&n, pcm, FRAMES, 1);
    assert(n.gain_db == 0.0 && !n.meter_valid);
    advance(&n, 5.0, 10);
    assert(n.gain_db == 0.0 && !n.meter_valid);

    /* A sustained quiet program gets a capped gradual boost. */
    iptv_audio_normalizer_reset(&n);
    advance(&n, 200.0, 12);
    assert(n.meter_valid && n.gain_db > 17.9);
    const double acquired = n.gain_db;
    memset(pcm, 0, sizeof(pcm));
    iptv_audio_normalizer_process(&n, pcm, FRAMES, 1);
    assert(n.gain_db < acquired);
    for (unsigned i = 0; i < FRAMES * 2; ++i)
        assert(pcm[i] == 0);

    /* Sudden loud transients after boosting must stay within the ceiling.
     * Both channels share the limiter, preserving the stereo level ratio. */
    for (unsigned i = 0; i < FRAMES; ++i)
    {
        pcm[i * 2] = i % 2 ? INT16_MIN : INT16_MAX;
        pcm[i * 2 + 1] = (int16_t)(pcm[i * 2] / 2);
    }
    iptv_audio_normalizer_process(&n, pcm, FRAMES, 1);
    assert(n.limited_blocks > 0);
    for (unsigned i = 0; i < FRAMES; ++i)
    {
        assert(pcm[i * 2] >= -29203 && pcm[i * 2] <= 29203);
        assert(abs((int)pcm[i * 2] - 2 * (int)pcm[i * 2 + 1]) <= 2);
    }
    advance(&n, 20000.0, 12);
    assert(n.gain_db < 0.0);

    /* Disable is immediately transparent, and re-enable cannot inherit boost. */
    memcpy(pcm, original, sizeof(pcm));
    iptv_audio_normalizer_process(&n, pcm, FRAMES, 0);
    assert(memcmp(pcm, original, sizeof(pcm)) == 0 && n.gain_db == 0.0);
    memset(pcm, 0, sizeof(pcm));
    iptv_audio_normalizer_process(&n, pcm, FRAMES, 1);
    assert(n.gain_db == 0.0 && !n.meter_valid);
    iptv_audio_normalizer_reset(&n);
    assert(n.window_count == 0 && n.limiter_gain == 1.0 && !n.enabled);
    puts("Audio normalization behavioral tests passed.");
    return 0;
}
