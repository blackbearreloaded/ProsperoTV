// ProsperoTV - The PC's stand-ins for the console: keyboard, network, clock.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "tv/platform.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace host
{

// What the next keyboard request is answered with at the next poll. The
// answer is used once; with none set, a request stays open (as when the
// player has not typed yet) until cancel_keyboard().
void set_keyboard_text(const std::string &text);
void cancel_keyboard();
// How many requests the keyboard has had, and the title of the last one.
int keyboard_requests();
const std::string &keyboard_title();
void set_keyboard_available(bool available);

// The stand-in network: every download answers with this file, after this
// long, or fails when `reachable` is false.
void set_network(bool reachable, const std::string &playlist_path, unsigned delay_ms = 0);
// An exact URL's response, for sources with distinct catalog and guide endpoints.
void set_network_response(const std::string &url, const std::string &file);
// The size of the pieces a list arrives in (0: as on the console).
void set_network_piece(std::size_t bytes);
// How many downloads were asked for.
int fetch_count();
struct RequestRecord
{
    std::string url, cookie, authorization;
};
std::vector<RequestRecord> requests();
// How many bytes of lists were handed over, all downloads together.
std::size_t delivered_bytes();

// The clock the logic sees (seconds since 1970); 0 is the PC's own clock.
void set_unix_time(std::uint64_t seconds);

// The stand-in update. offer_update: what the next update_take() answers,
// once. set_update_progress: what update_poll() answers from now on.
// refuse_update: update_begin() (or update_apply()) says no.
void offer_update(const ptv::platform::UpdateOffer &offer);
void set_update_progress(const ptv::platform::UpdateProgress &progress);
void refuse_update(bool begin, bool apply);
// How often each was asked for since reset().
struct UpdateCalls
{
    int begin = 0;
    int cancel = 0;
    int apply = 0;
    int finish = 0;
};
UpdateCalls update_calls();

// Forgets everything above (between tests).
void reset();

} // namespace host
