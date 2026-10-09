// ProsperoTV - Alpha-composite subtitle pictures onto the presenter's YUV copy.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/playback_osd.hpp"
#include "iptv_color.h"
#include <algorithm>
#include <cstring>

namespace ptv
{
bool composite_subtitle_bitmaps(const iptv::SubtitleCue &cue, void *surface, std::size_t bytes,
                                unsigned pitch, unsigned sh, unsigned vw, unsigned vh,
                                unsigned depth, unsigned hdr)
{
    if (!surface || (depth != 8 && depth != 10) || (hdr && depth != 10) || !vw || !vh ||
        vw > pitch || vh > sh || pitch > 8192 || sh > 8192 || (pitch & 1u) || !cue.canvas_width ||
        !cue.canvas_height || cue.canvas_width > 3840 || cue.canvas_height > 2160)
        return false;
    const unsigned component = depth == 10 ? 2 : 1, scale = depth == 10 ? 4 : 1;
    const std::size_t y_bytes = static_cast<std::size_t>(pitch) * sh;
    if ((y_bytes + static_cast<std::size_t>(pitch) * ((sh + 1) / 2)) * component > bytes)
        return false;
    auto *out = static_cast<std::uint8_t *>(surface);
    const auto get = [&](std::size_t at) -> unsigned
    {
        if (component == 1)
            return out[at];
        std::uint16_t value;
        std::memcpy(&value, out + at * 2, 2);
        return value;
    };
    const auto put = [&](std::size_t at, unsigned value)
    {
        if (component == 1)
            out[at] = static_cast<std::uint8_t>(value);
        else
        {
            const auto word = static_cast<std::uint16_t>(value);
            std::memcpy(out + at * 2, &word, 2);
        }
    };
    bool drawn = false;
    for (const auto &bitmap : cue.bitmaps)
    {
        if (!bitmap.width || !bitmap.height || bitmap.width > 3840 || bitmap.height > 2160 ||
            bitmap.x < -3840 || bitmap.x > 3840 || bitmap.y < -2160 || bitmap.y > 2160 ||
            bitmap.argb.size() != static_cast<std::size_t>(bitmap.width) * bitmap.height)
            continue;
        const auto sample = [&](unsigned x, unsigned y) -> std::uint32_t
        {
            const int sx = static_cast<int>(x * cue.canvas_width / vw) - bitmap.x;
            const int sy = static_cast<int>(y * cue.canvas_height / vh) - bitmap.y;
            return sx >= 0 && sy >= 0 && static_cast<unsigned>(sx) < bitmap.width &&
                           static_cast<unsigned>(sy) < bitmap.height
                       ? bitmap.argb[static_cast<std::size_t>(sy) * bitmap.width + sx]
                       : 0;
        };
        const auto map_x = [&](int x)
        {
            const int numerator = x * static_cast<int>(vw), divisor = cue.canvas_width;
            return std::clamp((numerator >= 0 ? numerator + divisor - 1 : numerator) / divisor, 0,
                              static_cast<int>(vw));
        };
        const auto map_y = [&](int y)
        {
            const int numerator = y * static_cast<int>(vh), divisor = cue.canvas_height;
            return std::clamp((numerator >= 0 ? numerator + divisor - 1 : numerator) / divisor, 0,
                              static_cast<int>(vh));
        };
        const int left = map_x(bitmap.x), top = map_y(bitmap.y);
        const int right = map_x(bitmap.x + static_cast<int>(bitmap.width));
        const int bottom = map_y(bitmap.y + static_cast<int>(bitmap.height));
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
            {
                const auto color = sample(x, y);
                const unsigned a = color >> 24;
                if (!a)
                    continue;
                const int r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
                unsigned luma =
                    static_cast<unsigned>(16 + ((47 * r + 157 * g + 16 * b + 128) >> 8)) * scale;
                if (hdr)
                {
                    std::uint16_t yuv[3];
                    iptv_color_ui_yuv(r, g, b, yuv, hdr == 2);
                    luma = yuv[0];
                }
                const auto at = static_cast<std::size_t>(y) * pitch + x;
                put(at, (get(at) * (255 - a) + luma * a + 127) / 255);
                drawn = true;
            }
        for (int y = top & ~1; y < bottom; y += 2)
            for (int x = left & ~1; x < right; x += 2)
            {
                unsigned alpha = 0, u = 0, v = 0;
                for (unsigned dy = 0; dy < 2; ++dy)
                    for (unsigned dx = 0; dx < 2; ++dx)
                    {
                        if (static_cast<unsigned>(x) + dx >= vw ||
                            static_cast<unsigned>(y) + dy >= vh)
                            continue;
                        const auto color = sample(x + dx, y + dy);
                        const unsigned a = color >> 24;
                        const int r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
                        alpha += a;
                        if (hdr)
                        {
                            std::uint16_t yuv[3];
                            iptv_color_ui_yuv(r, g, b, yuv, hdr == 2);
                            u += a * yuv[1];
                            v += a * yuv[2];
                        }
                        else
                        {
                            u += a * scale *
                                 static_cast<unsigned>(std::clamp(
                                     128 + ((-26 * r - 87 * g + 113 * b + 128) >> 8), 16, 240));
                            v += a * scale *
                                 static_cast<unsigned>(128 +
                                                       ((112 * r - 102 * g - 10 * b + 128) >> 8));
                        }
                    }
                if (alpha)
                {
                    const auto at = y_bytes + static_cast<std::size_t>(y / 2) * pitch + x;
                    put(at, (get(at) * (1020 - alpha) + u + 510) / 1020);
                    put(at + 1, (get(at + 1) * (1020 - alpha) + v + 510) / 1020);
                }
            }
    }
    return drawn;
}
} // namespace ptv
