/* ps5-native-app-boilerplate - Software audio fallback, stereo PCM output.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_audio_decode.h"
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct iptv_audio_decoder
{
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *mix;
    AVChannelLayout layout;
    int rate, format;
};

void iptv_audio_decoder_free(iptv_audio_decoder_t **decoder)
{
    if (!decoder || !*decoder)
        return;
    iptv_audio_decoder_t *d = *decoder;
    swr_free(&d->mix);
    av_channel_layout_uninit(&d->layout);
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    avcodec_free_context(&d->codec);
    free(d);
    *decoder = NULL;
}

iptv_audio_decoder_t *iptv_audio_decoder_create(uint32_t type)
{
    enum AVCodecID id = type == 0x81u   ? AV_CODEC_ID_AC3
                        : type == 0x87u ? AV_CODEC_ID_EAC3
                        : type == 0x11u ? AV_CODEC_ID_AAC_LATM
                                        : AV_CODEC_ID_AAC;
    const AVCodec *codec = avcodec_find_decoder(id);
    iptv_audio_decoder_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->codec = avcodec_alloc_context3(codec);
    d->frame = av_frame_alloc();
    d->packet = av_packet_alloc();
    if (!codec || !d->codec || !d->frame || !d->packet)
        goto failed;
    d->codec->thread_count = 1; // Existing audio worker owns decoding; no extra codec threads.
    if (avcodec_open2(d->codec, codec, NULL) < 0)
        goto failed;
    return d;
failed:
    iptv_audio_decoder_free(&d);
    return NULL;
}

void iptv_audio_decoder_reset(iptv_audio_decoder_t *d)
{
    if (!d)
        return;
    avcodec_flush_buffers(d->codec);
    swr_free(&d->mix);
    av_channel_layout_uninit(&d->layout);
}

int iptv_audio_decode(iptv_audio_decoder_t *d, const uint8_t *data, size_t bytes, int16_t *pcm,
                      size_t capacity, uint32_t *sample_rate)
{
    if (!d || !data || !pcm || !sample_rate || !bytes || bytes > 8194u || capacity > INT_MAX)
        return AVERROR(EINVAL);
    av_packet_unref(d->packet);
    int result = av_new_packet(d->packet, (int)bytes);
    if (result < 0)
        return result;
    memcpy(d->packet->data, data, bytes); // av_new_packet supplies required zero padding.
    result = avcodec_send_packet(d->codec, d->packet);
    if (result < 0)
        return result;
    int written = 0;
    while ((result = avcodec_receive_frame(d->codec, d->frame)) >= 0)
    {
        AVFrame *f = d->frame;
        if (f->sample_rate < 8000 || f->sample_rate > 192000 || f->ch_layout.nb_channels < 1 ||
            f->ch_layout.nb_channels > 8 || f->nb_samples < 0)
            return AVERROR_INVALIDDATA;
        if (written && *sample_rate != (uint32_t)f->sample_rate)
            return AVERROR_INVALIDDATA;
        if (!d->mix || d->rate != f->sample_rate || d->format != f->format ||
            av_channel_layout_compare(&d->layout, &f->ch_layout))
        {
            const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            swr_free(&d->mix);
            av_channel_layout_uninit(&d->layout);
            result = swr_alloc_set_opts2(&d->mix, &stereo, AV_SAMPLE_FMT_S16, f->sample_rate,
                                         &f->ch_layout, (enum AVSampleFormat)f->format,
                                         f->sample_rate, 0, NULL);
            if (result < 0)
                return result;
            // Normalize surround downmix to avoid clipping; include dialogue/center channels.
            av_opt_set_double(d->mix, "rematrix_maxval", 1.0, 0);
            if ((result = swr_init(d->mix)) < 0)
                return result;
            if ((result = av_channel_layout_copy(&d->layout, &f->ch_layout)) < 0)
                return result;
            d->rate = f->sample_rate;
            d->format = f->format;
        }
        const int room = ((int)capacity - written) / (2 * (int)sizeof(int16_t));
        if (swr_get_out_samples(d->mix, f->nb_samples) > room)
            return AVERROR(ENOSPC);
        uint8_t *out = (uint8_t *)pcm + written;
        result = swr_convert(d->mix, &out, room, (const uint8_t **)f->extended_data, f->nb_samples);
        if (result < 0)
            return result;
        written += result * 2 * (int)sizeof(int16_t);
        *sample_rate = (uint32_t)f->sample_rate;
        av_frame_unref(f);
    }
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? written : result;
}
