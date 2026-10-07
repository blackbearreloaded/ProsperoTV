/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_remote.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

// Runs the production server without platform/UI dependencies for HTTP tests.
int main(int argc, char **argv)
{
    if (argc != 2 || !iptv_remote_start(static_cast<unsigned short>(std::atoi(argv[1]))))
        return 1;
    iptv_remote_enable_search(true);
    std::puts(iptv_remote_hint());
    std::fflush(stdout);
    for (;;)
    {
        iptv_remote_poll();
        iptv_input_event_t event{};
        while (iptv_remote_next(&event))
        {
            std::printf("key:%d\n", event.action);
            std::fflush(stdout);
        }
        char text[IPTV_IME_MAX_TEXT_BYTES];
        if (iptv_remote_search(text))
        {
            std::printf("search:%s\n", text);
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
