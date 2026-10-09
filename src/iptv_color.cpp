// ProsperoTV - SDR UI colors at bounded HDR paper white.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_color.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
struct Tables
{
    std::array<float, 256> linear{};
    std::array<float, 4098> pq{};
    std::array<float, 1024> luminance{};
    std::array<uint8_t, 4097> srgb{};
    Tables()
    {
        for (unsigned i = 0; i < linear.size(); ++i)
        {
            const float c = float(i) / 255;
            linear[i] = c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
        }
        for (unsigned i = 0; i < pq.size(); ++i)
        {
            const float root = std::min(float(i) / 4096, 1.0f);
            const float p = std::pow(root * root * 203 / 10000, .1593017578125f);
            pq[i] = std::pow((.8359375f + 18.8515625f * p) / (1 + 18.6875f * p), 78.84375f);
        }
        for (unsigned i = 0; i < luminance.size(); ++i)
        {
            const float p = std::pow(float(i) / 1023, 1 / 78.84375f);
            luminance[i] = (10000.0f / 203) *
                           std::pow(std::max(p - .8359375f, 0.0f) / (18.8515625f - 18.6875f * p),
                                    1 / .1593017578125f);
        }
        for (unsigned i = 0; i < srgb.size(); ++i)
        {
            const float c = float(i) / 4096;
            srgb[i] = static_cast<uint8_t>(
                255 * (c <= .0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - .055f) + .5f);
        }
    }
    float encode(float c) const
    {
        const float at = std::sqrt(std::clamp(c, 0.0f, 1.0f)) * 4096;
        const unsigned index = static_cast<unsigned>(at);
        return std::lerp(pq[index], pq[index + 1], at - index);
    }
};
const Tables &tables()
{
    static const Tables value;
    return value;
}
} // namespace

void iptv_color_ui_yuv(uint8_t red, uint8_t green, uint8_t blue, uint16_t output[3])
{
    if (!output)
        return;
    const auto &t = tables();
    const float lr = t.linear[red], lg = t.linear[green], lb = t.linear[blue];
    const float r = t.encode(.6274f * lr + .3293f * lg + .0433f * lb);
    const float g = t.encode(.0691f * lr + .9195f * lg + .0114f * lb);
    const float b = t.encode(.0164f * lr + .0880f * lg + .8956f * lb);
    const float y = .2627f * r + .6780f * g + .0593f * b;
    output[0] = static_cast<uint16_t>(64 + 876 * y + .5f);
    output[1] = static_cast<uint16_t>(512 + 896 * (b - y) / 1.8814f + .5f);
    output[2] = static_cast<uint16_t>(512 + 896 * (r - y) / 1.4746f + .5f);
}

uint16_t iptv_color_ui_luma(uint8_t limited_sdr)
{
    static const auto luma = []
    {
        std::array<uint16_t, 256> result{};
        for (unsigned i = 0; i < result.size(); ++i)
        {
            const auto rgb = static_cast<uint8_t>(std::clamp((int(i) - 16) * 255 / 219, 0, 255));
            uint16_t yuv[3];
            iptv_color_ui_yuv(rgb, rgb, rgb, yuv);
            result[i] = yuv[0];
        }
        return result;
    }();
    return luma[limited_sdr];
}

void iptv_color_pq_to_srgb(float red, float green, float blue, uint8_t output[3])
{
    if (!output)
        return;
    const auto &t = tables();
    const auto light = [&](float code)
    { return t.luminance[static_cast<unsigned>(std::clamp(code, 0.0f, 1.0f) * 1023 + .5f)]; };
    const float r = light(red), g = light(green), b = light(blue);
    const float rgb[] = {1.6605f * r - .5876f * g - .0728f * b,
                         -.1246f * r + 1.1329f * g - .0083f * b,
                         -.0182f * r - .1006f * g + 1.1187f * b};
    const float scale = 1 / (1 + std::max({rgb[0], rgb[1], rgb[2], 0.0f}));
    for (unsigned i = 0; i < 3; ++i)
        output[i] =
            t.srgb[static_cast<unsigned>(std::clamp(rgb[i] * scale, 0.0f, 1.0f) * 4096 + .5f)];
}
