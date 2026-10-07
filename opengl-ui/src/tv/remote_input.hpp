// ProsperoTV - Phone buttons use the same logical input as the controller.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/input.hpp"
#include "iptv_input.h"

namespace ptv
{
inline hui::InputFrame remote_input(iptv_input_action_t key)
{
    using hui::Action;
    constexpr Action actions[] = {Action::confirm,   Action::back,  Action::west,
                                  Action::north,     Action::menu,  Action::page_prev,
                                  Action::page_next, Action::touch, Action::up,
                                  Action::down,      Action::left,  Action::right};
    hui::InputFrame input;
    if (key < 0 || key >= IPTV_INPUT_COUNT)
        return input;
    input.connected = true;
    input.pressed = input.held = hui::action_bit(actions[key]);
    if (key >= IPTV_INPUT_UP)
        input.nav = static_cast<hui::Direction>(key - IPTV_INPUT_UP + 1);
    return input;
}
} // namespace ptv
