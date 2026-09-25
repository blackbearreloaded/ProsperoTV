/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_mp2.h"
#define MINIMP3_IMPLEMENTATION
#include "../vendor/minimp3/minimp3.h"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    assert(argc == 3);
    for (int file = 1; file < argc; ++file)
    {
        FILE *input = fopen(argv[file], "rb");
        assert(input);
        unsigned char data[8192];
        size_t length = fread(data, 1, sizeof(data), input), offset = 0;
        fclose(input);
        mp3dec_t decoder;
        mp3dec_init(&decoder);
        unsigned frames = 0;
        long long energy = 0;
        while (offset < length)
        {
            uint32_t rate = 0, channels = 0;
            size_t bytes = iptv_mp2_frame_info(data + offset, length - offset, &rate, &channels);
            assert(bytes && bytes <= length - offset);
            assert(rate == (file == 1 ? 48000u : 24000u));
            assert(channels == (file == 1 ? 2u : 1u));
            for (size_t n = 0; n < 4; ++n)
                assert(iptv_mp2_frame_info(data + offset, n, &rate, &channels) == 0);
            mp3dec_frame_info_t info = {0};
            mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            int samples = mp3dec_decode_frame(&decoder, data + offset, (int)bytes, pcm, &info);
            assert(samples == 1152 && info.layer == 2 && info.frame_offset == 0);
            assert(info.frame_bytes == (int)bytes && info.hz == (int)rate);
            assert(info.channels == (int)channels);
            for (int i = 0; i < samples * info.channels; ++i)
                energy += (long long)pcm[i] * pcm[i];
            offset += bytes;
            ++frames;
        }
        assert(frames >= 3 && energy > 1000000);
    }
    uint32_t rate, channels;
    const uint8_t invalid[][4] = {{0xff, 0xfb, 0x94, 0},
                                  {0xff, 0xfd, 0x04, 0},
                                  {0xff, 0xfd, 0xfc, 0},
                                  {0xff, 0xed, 0x94, 0},
                                  {0xff, 0xfd, 0x94, 2}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!iptv_mp2_frame_info(invalid[i], 4, &rate, &channels));
    return 0;
}
