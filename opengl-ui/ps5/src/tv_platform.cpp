// ProsperoTV - What the app's logic asks of the console: threads, clock, network.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/platform.hpp"

#include <ctime>
#include <pthread.h>

extern "C" int scePthreadCreate(void **thread, const void *attributes, void *(*entry)(void *),
                                void *argument, const char *name);
extern "C" int scePthreadDetach(void *thread);
extern "C" int scePthreadJoin(void *thread, void **result);
extern "C" int sceKernelUsleep(unsigned int microseconds);

namespace ptv::platform
{

void *thread_start(void *(*entry)(void *), void *argument, std::size_t stack_bytes,
                   const char *name)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
        return nullptr;
    void *thread = nullptr;
    const int sized = pthread_attr_setstacksize(&attributes, stack_bytes);
    const int created =
        sized == 0 ? scePthreadCreate(&thread, &attributes, entry, argument, name) : sized;
    pthread_attr_destroy(&attributes);
    return created == 0 ? thread : nullptr;
}

int thread_join(void *thread)
{
    void *result = nullptr;
    return scePthreadJoin(thread, &result);
}

int thread_detach(void *thread)
{
    return scePthreadDetach(thread);
}

void sleep_ms(unsigned milliseconds)
{
    sceKernelUsleep(milliseconds * 1000u);
}

std::uint64_t unix_time()
{
    const std::time_t now = std::time(nullptr);
    return now > 0 ? static_cast<std::uint64_t>(now) : 0u;
}

iptv::http::Status network_init()
{
    return iptv::http::NetworkInit();
}

void network_shutdown()
{
    iptv::http::NetworkShutdown();
}

void network_cancel()
{
    iptv::http::CancelActivePlaylistRequest();
}

iptv::http::FetchResult fetch(const char *url, char *buffer, std::size_t capacity,
                              std::size_t max_bytes, const iptv::http::RequestControl *control,
                              const iptv::http::RequestHeaders *headers)
{
    return iptv::http::GetM3u(url, buffer, capacity, max_bytes, headers, control);
}

iptv::http::FetchResult fetch_list(const char *url, const iptv::http::ListSink &sink,
                                   std::size_t max_bytes, const iptv::http::RequestControl *control,
                                   const iptv::http::RequestHeaders *headers)
{
    return iptv::http::GetList(url, sink, max_bytes, headers, control);
}

} // namespace ptv::platform
