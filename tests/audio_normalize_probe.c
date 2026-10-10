/* ProsperoTV - Host-only PCM probe for independent FFmpeg validation.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_audio_normalize.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    iptv_audio_normalizer_t state;
    iptv_audio_normalizer_reset(&state);
    int16_t pcm[512];
    size_t samples;
    while ((samples = fread(pcm, sizeof(*pcm), 512, stdin)) != 0)
    {
        if (samples % 2)
            return 3;
        iptv_audio_normalizer_process(&state, pcm, samples / 2, atoi(argv[1]));
        if (fwrite(pcm, sizeof(*pcm), samples, stdout) != samples)
            return 4;
    }
    if (ferror(stdin) || fflush(stdout))
        return 5;
    fprintf(stderr, "meter_lufs=%.6f gain_db=%.6f limited_blocks=%llu\n", state.loudness_lufs,
            state.gain_db, (unsigned long long)state.limited_blocks);
    return 0;
}
