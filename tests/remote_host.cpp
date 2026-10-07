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
    iptv_remote_set_icon("opengl-ui/ps5/sce_sys/icon0.png");
    if ((argc != 2 && argc != 3) ||
        !iptv_remote_start(static_cast<unsigned short>(std::atoi(argv[1]))))
        return 1;
    iptv_remote_enable_search(argc == 2);
    int favorite_calls = 0;
    if (argc == 3)
        iptv_remote_set_playback_favorite(
            [](void *context) -> int
            {
                const int call = ++*static_cast<int *>(context);
                std::printf("favorite:%d\n", call);
                std::fflush(stdout);
                return call == 1 ? 1 : call == 2 ? 0 : -1;
            },
            &favorite_calls);
    std::puts(iptv_remote_hint());
    std::fflush(stdout);
    for (;;)
    {
        iptv_remote_poll();
        iptv_input_event_t event{};
        while (iptv_remote_next(&event))
        {
            if (event.action == IPTV_INPUT_CIRCLE)
                iptv_remote_set_playback_favorite(nullptr, nullptr);
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
