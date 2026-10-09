// ProsperoTV - Use the bundled H.264 parser; reconstruct each field separately.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_fields.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>
extern "C"
{
#include <libavcodec/avcodec.h>
}

struct iptv_field_parser
{
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext *codec = avcodec_alloc_context3(nullptr);
    std::vector<std::uint8_t> padded;
    ~iptv_field_parser()
    {
        av_parser_close(parser);
        avcodec_free_context(&codec);
    }
};

iptv_field_parser_t *iptv_field_parser_create()
{
    auto *state = new (std::nothrow) iptv_field_parser;
    if (state && state->parser && state->codec)
    {
        state->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
        state->codec->codec_id = AV_CODEC_ID_H264;
        state->codec->codec_type = AVMEDIA_TYPE_VIDEO;
        return state;
    }
    delete state;
    return nullptr;
}
void iptv_field_parser_destroy(iptv_field_parser_t *parser)
{
    delete parser;
}
iptv_field_info_t iptv_field_parse(iptv_field_parser_t *state, const void *data, size_t bytes)
{
    iptv_field_info_t result{};
    if (!state || !data || !bytes || bytes > 8u * 1024u * 1024u)
        return result;
    state->padded.resize(bytes + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(state->padded.data(), data, bytes);
    std::memset(state->padded.data() + bytes, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    std::uint8_t *parsed = nullptr;
    int parsed_bytes = 0;
    if (av_parser_parse2(state->parser, state->codec, &parsed, &parsed_bytes, state->padded.data(),
                         static_cast<int>(bytes), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0) < 0 ||
        !parsed_bytes)
        return result;
    const auto structure = state->parser->picture_structure;
    const auto order = state->parser->field_order;
    if (structure == AV_PICTURE_STRUCTURE_TOP_FIELD ||
        structure == AV_PICTURE_STRUCTURE_BOTTOM_FIELD)
    {
        result.first = structure == AV_PICTURE_STRUCTURE_TOP_FIELD ? 1u : 2u;
        result.field_picture = 1;
        result.count = 2; // The native decoder combines the pair into one surface.
    }
    else if (order == AV_FIELD_TT || order == AV_FIELD_TB || order == AV_FIELD_BB ||
             order == AV_FIELD_BT)
    {
        result.first = order == AV_FIELD_TT || order == AV_FIELD_BT ? 1u : 2u;
        result.count = static_cast<unsigned>(std::clamp(state->parser->repeat_pict + 1, 2, 6));
    }
    const auto rate = state->codec->framerate;
    if (result.first && rate.num > 0 && rate.den > 0)
    {
        const auto duration = UINT64_C(1000000) * rate.den / (UINT64_C(2) * rate.num);
        if (duration >= 1000 && duration <= 1000000)
            result.duration_us = static_cast<std::uint32_t>(duration);
    }
    return result;
}

int iptv_field_bob(void *output, size_t output_bytes, const void *input, size_t input_bytes,
                   uint32_t pitch, uint32_t height, uint32_t width, uint32_t field)
{
    if (!output || !input || field > 1 || !width || width > pitch || (width & 1u) || (pitch & 1u) ||
        height < 4 || (height & 3u) || pitch > 8192 || height > 8192)
        return -1;
    const size_t required = static_cast<size_t>(pitch) * (height + height / 2);
    const auto dst_address = reinterpret_cast<uintptr_t>(output);
    const auto src_address = reinterpret_cast<uintptr_t>(input);
    if (required > output_bytes || required > input_bytes || required > UINTPTR_MAX - dst_address ||
        required > UINTPTR_MAX - src_address ||
        (dst_address < src_address + required && src_address < dst_address + required))
        return -1;
    auto *dst = static_cast<std::uint8_t *>(output);
    const auto *src = static_cast<const std::uint8_t *>(input);
    // ponytail: spatial bob preserves field-rate motion at half vertical detail;
    // a motion-adaptive filter can recover detail if its measured cost allows it.
    for (const auto rows : {height, height / 2})
    {
        for (std::uint32_t y = 0; y < rows; ++y)
        {
            auto *row = dst + static_cast<size_t>(y) * pitch;
            if ((y & 1u) == field)
                std::memcpy(row, src + static_cast<size_t>(y) * pitch, width);
            else
            {
                const auto above = y ? y - 1 : 1;
                const auto below = y + 1 < rows ? y + 1 : y - 1;
                const auto *a = src + static_cast<size_t>(above) * pitch;
                const auto *b = src + static_cast<size_t>(below) * pitch;
                for (std::uint32_t x = 0; x < width; ++x)
                    row[x] = static_cast<std::uint8_t>((a[x] + b[x] + 1u) / 2);
            }
        }
        dst += static_cast<size_t>(rows) * pitch;
        src += static_cast<size_t>(rows) * pitch;
    }
    return 0;
}
