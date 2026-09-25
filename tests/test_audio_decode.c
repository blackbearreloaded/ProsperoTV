/* ps5-native-app-boilerplate - Audio fallback and downmix regression check.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_audio_decode.h"
#include "iptv_audio_frame.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    assert(argc == 3);
    const unsigned type = (unsigned)strtoul(argv[1], NULL, 0);
    FILE *input = fopen(argv[2], "rb");
    assert(input);
    uint8_t data[65536];
    const size_t bytes = fread(data, 1, sizeof(data), input);
    fclose(input);
    assert(bytes > 7 && bytes < sizeof(data));
    iptv_audio_decoder_t *decoder = iptv_audio_decoder_create(type);
    assert(decoder);
    for (unsigned pass = 0; pass < 2; ++pass)
    {
        size_t at = 0;
        unsigned output_frames = 0;
        long long left = 0, right = 0;
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
                left += (long long)pcm[i] * pcm[i];
                right += (long long)pcm[i + 1] * pcm[i + 1];
            }
            output_frames += count / 4;
            at += frame;
        }
        assert(at == bytes && output_frames >= 6000 && left > 1000000 && right > 1000000);
        iptv_audio_decoder_reset(decoder);
    }
    iptv_audio_decoder_free(&decoder);
    assert(!decoder);
    return 0;
}
