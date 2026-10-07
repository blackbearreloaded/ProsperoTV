/* ProsperoTV - ps5-native-app-boilerplate HTTP test process.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "iptv_remote.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <thread>
#include <unistd.h>
int main(int argc, char **argv)
{
    if (argc < 3)
        return 1;
    iptv_remote_set_icon("opengl-ui/ps5/sce_sys/icon0.png");
    iptv_remote_set_pairing_store(argv[2]);
    if (!iptv_remote_start(static_cast<unsigned short>(std::atoi(argv[1]))))
        return 1;
    iptv_remote_enable_search(argc == 3);
    iptv_remote_set_volume(75);
    iptv_remote_set_volume_handler([](unsigned v, void *) { return v != 13; }, nullptr);
    int calls = 0;
    if (argc == 4)
        iptv_remote_set_playback_favorite(
            [](void *context) -> int
            {
                const int call = ++*static_cast<int *>(context);
                std::printf("favorite:%d\n", call);
                std::fflush(stdout);
                return call == 1 ? 1 : call == 2 ? 0 : -1;
            },
            &calls);
    std::printf("url:%s\n", iptv_remote_url());
    std::fflush(stdout);
    for (;;)
    {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, 0) > 0 && (input.revents & POLLIN))
        {
            char command[80];
            if (!std::fgets(command, sizeof(command), stdin))
                break;
            if (std::strcmp(command, "pair\n") == 0)
            {
                iptv_remote_begin_pairing();
                std::printf("code:%s\n", iptv_remote_pairing_code());
            }
            else if (std::strcmp(command, "forget\n") == 0)
            {
                std::printf("forgot:%d\n", iptv_remote_forget_phones());
            }
            else if (std::strcmp(command, "connected\n") == 0)
            {
                std::printf("connected:%d\n", iptv_remote_take_connected());
            }
            else if (std::strcmp(command, "cancel\n") == 0)
            {
                iptv_remote_cancel_pairing();
                std::puts("cancelled");
            }
            std::fflush(stdout);
        }
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
