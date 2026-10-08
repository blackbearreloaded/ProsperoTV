// ProsperoTV - Deterministic preview frames for the host renderer and tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/preview.hpp"
#include "tv/platform.hpp"
#include "host_preview.hpp"
#include <array>
#include <atomic>

namespace
{
std::atomic<bool> enabled{false};
std::atomic<unsigned> starts{0}, stops{0};
} // namespace
namespace host
{
void set_preview(bool value)
{
    enabled.store(value);
    starts.store(0);
    stops.store(0);
}
unsigned preview_starts()
{
    return starts.load();
}
unsigned preview_stops()
{
    return stops.load();
}
} // namespace host
namespace ptv::platform
{
void preview(const PlayRequest &, const iptv::http::RequestControl &control,
             void (*picture)(void *, const iptv_native_picture_t *), void *context)
{
    if (!enabled.load())
        return;
    ++starts;
    std::array<std::uint8_t, 96 * 54 * 3 / 2> nv12{};
    nv12.fill(128);
    unsigned frame = 0;
    while (!control.cancelled(control.context))
    {
        for (unsigned y = 0; y < 54; ++y)
            for (unsigned x = 0; x < 96; ++x)
                nv12[y * 96 + x] = static_cast<std::uint8_t>(32 + (x + y + frame) % 190);
        const iptv_native_picture_t p{nv12.data(), nv12.size(), 96, 54,
                                      96,          54,          8,  std::uint64_t(frame) * 20000};
        picture(context, &p);
        ++frame;
        sleep_ms(20);
    }
    ++stops;
}
} // namespace ptv::platform
