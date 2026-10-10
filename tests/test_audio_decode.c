/* ps5-native-app-boilerplate - Audio fallback and downmix regression check.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_audio_decode.h"
#include "iptv_audio_frame.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum
{
    CEILING = 29205
}; /* -1 dBFS in 16 bits, rounded up. */

static void check_limiter(void)
{
    enum
    {
        FRAMES = 48000
    };
    float *in = malloc(sizeof(float) * 2 * FRAMES);
    int16_t *out = malloc(sizeof(int16_t) * 2 * FRAMES);
    assert(in && out);
    /* A quiet tone passes untouched. */
    for (int i = 0; i < FRAMES; ++i)
        in[2 * i] = in[2 * i + 1] = 0.25f * sinf(2.0f * 3.14159265f * 997.0f * (float)i / 48000.0f);
    float gain = 1.0f;
    iptv_audio_limit_to_s16(&gain, 48000, in, FRAMES, out);
    assert(gain == 1.0f);
    for (int i = 0; i < FRAMES; ++i)
        assert(out[2 * i] == (int16_t)lrintf(in[2 * i] * 32767.0f) && out[2 * i] == out[2 * i + 1]);
    /* A tone over full scale on one side is held under the ceiling on both, in step. */
    for (int i = 0; i < FRAMES; ++i)
    {
        in[2 * i] = 2.4f * sinf(2.0f * 3.14159265f * 997.0f * (float)i / 48000.0f);
        in[2 * i + 1] = 0.5f * in[2 * i];
    }
    in[10] = NAN;
    iptv_audio_limit_to_s16(&gain, 48000, in, FRAMES, out);
    int peak = 0;
    for (int i = 0; i < FRAMES; ++i)
    {
        peak = abs(out[2 * i]) > peak ? abs(out[2 * i]) : peak;
        assert(abs(out[2 * i]) <= CEILING && abs(out[2 * i + 1]) <= CEILING / 2 + 1);
    }
    assert(peak > CEILING - 300 && gain < 0.5f && gain > 0.3f);
    /* Silence afterwards lets the gain come back within a quarter of a second. */
    for (int i = 0; i < 2 * FRAMES; ++i)
        in[i] = 0.0f;
    iptv_audio_limit_to_s16(&gain, 48000, in, FRAMES / 4, out);
    assert(gain == 1.0f);
    free(in);
    free(out);
}

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    check_limiter();
    /* The loudest sample a surround fixture must reach: its tone is in the centre only. */
    const int least_peak = argc == 4 ? atoi(argv[3]) : 0;
    const unsigned type = (unsigned)strtoul(argv[1], NULL, 0);
    FILE *input = fopen(argv[2], "rb");
    assert(input);
    uint8_t data[65536];
    const size_t bytes = fread(data, 1, sizeof(data), input);
    fclose(input);
    assert(bytes > 7 && bytes < sizeof(data));
    iptv_audio_decoder_t *decoder = iptv_audio_decoder_create(type);
    assert(decoder);
    if (type == 129 || type == 135)
        assert(iptv_audio_frame_channels(type, data, bytes) == 6);
    for (unsigned pass = 0; pass < 2; ++pass)
    {
        size_t at = 0;
        unsigned output_frames = 0;
        long long left = 0, right = 0;
        int peak = 0;
        while (at + 7 <= bytes)
        {
            uint32_t rate = 0, samples = 0;
            size_t frame =
                type == 15 ? ((data[at + 3] & 3u) << 11) + (data[at + 4] << 3) + (data[at + 5] >> 5)
                           : iptv_audio_frame_info(type, data + at, bytes - at, &rate, &samples);
            assert(frame && at + frame <= bytes);
            int16_t pcm[8192];
            const int count = iptv_audio_decode(decoder, data + at, frame, pcm, sizeof(pcm), &rate);
            assert(count >= 0 && count % 4 == 0);
            if (count)
                assert(rate >= 8000 && rate <= 192000);
            for (int i = 0; i < count / 2; i += 2)
            {
                peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
                left += (long long)pcm[i] * pcm[i];
                right += (long long)pcm[i + 1] * pcm[i + 1];
            }
            output_frames += count / 4;
            at += frame;
        }
        assert(at == bytes && output_frames >= 6000 && left > 1000000 && right > 1000000);
        if (getenv("AUDIO_TEST_PRINT"))
            printf("type %u pass %u peak %d\n", type, pass, peak);
        assert(peak >= least_peak && peak <= CEILING);
        iptv_audio_decoder_reset(decoder);
    }
    iptv_audio_decoder_free(&decoder);
    assert(!decoder);
    return 0;
}
