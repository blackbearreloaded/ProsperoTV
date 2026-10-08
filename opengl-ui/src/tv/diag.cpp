// ProsperoTV - The diagnostic log: what the app did, written down while the viewer asks for it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/diag.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace ptv::diag
{

namespace
{

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_forced{false};
std::atomic<Sink> g_sink{nullptr};

} // namespace

void set_sink(Sink sink)
{
    g_sink.store(sink, std::memory_order_release);
}

void set_enabled(bool on)
{
    g_enabled.store(on, std::memory_order_release);
}

void set_forced(bool forced)
{
    g_forced.store(forced, std::memory_order_release);
}

bool enabled()
{
    return g_enabled.load(std::memory_order_acquire) || g_forced.load(std::memory_order_acquire);
}

void event(const char *format, ...)
{
    if (!enabled())
        return;
    const Sink sink = g_sink.load(std::memory_order_acquire);
    if (sink == nullptr)
        return;
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length > 0)
        sink(line);
}

} // namespace ptv::diag

extern "C" int tv_diag_enabled(void)
{
    return ptv::diag::enabled() ? 1 : 0;
}

extern "C" void tv_diag_line(const char *line)
{
    if (line != nullptr)
        ptv::diag::event("%s", line);
}
