/* ps5-native-app-boilerplate - Software audio fallback, stereo PCM output.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IPTV_AUDIO_DECODE_H
#define IPTV_AUDIO_DECODE_H
#include <stddef.h>
#include <stdint.h>
typedef struct iptv_audio_decoder iptv_audio_decoder_t;
iptv_audio_decoder_t *iptv_audio_decoder_create(uint32_t stream_type);
void iptv_audio_decoder_free(iptv_audio_decoder_t **decoder);
void iptv_audio_decoder_reset(iptv_audio_decoder_t *decoder);
/* Returns PCM bytes (possibly zero while buffering), or a negative codec error. */
int iptv_audio_decode(iptv_audio_decoder_t *decoder, const uint8_t *data, size_t bytes,
                      int16_t *pcm, size_t capacity_bytes, uint32_t *sample_rate);
#endif
