// ProsperoTV - What the app's logic asks of the machine it runs on.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console answers these with its own threads and libcurl
// (ps5/src/tv_platform.cpp, tv_update.cpp); a PC answers them with pthreads
// and stand-ins for the network and the update (host/host_platform.cpp), so
// the same logic runs in tests and in the PC renderer.

#pragma once

#include "iptv_http.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptv::platform
{

// A worker thread with a stack of its own. Returns null when it cannot start.
void *thread_start(void *(*entry)(void *), void *argument, std::size_t stack_bytes,
                   const char *name);
// 0 once the thread has ended and was joined.
int thread_join(void *thread);
int thread_detach(void *thread);
void sleep_ms(unsigned milliseconds);
// Seconds since 1970, or 0 when the clock is not set.
std::uint64_t unix_time();

// The network, as the catalog worker uses it. Always called from that thread,
// except network_cancel(), which interrupts it from the frame loop.
iptv::http::Status network_init();
void network_shutdown();
void network_cancel();
iptv::http::FetchResult fetch(const char *url, char *buffer, std::size_t capacity,
                              std::size_t max_bytes, const iptv::http::RequestControl *control,
                              const iptv::http::RequestHeaders *headers = nullptr);
// The same download handed to `sink` piece by piece as it arrives and kept
// nowhere: how a channel list of any size is read.
iptv::http::FetchResult fetch_list(const char *url, const iptv::http::ListSink &sink,
                                   std::size_t max_bytes, const iptv::http::RequestControl *control,
                                   const iptv::http::RequestHeaders *headers = nullptr);
// Artwork has an independent connection and cancellation lifetime, so a slow
// logo cannot interrupt a source refresh or programme guide download.
bool fetch_image(const char *url, std::vector<std::uint8_t> *bytes,
                 const iptv::http::RequestControl *control);

// ---- updates ----
// Once per launch the machine asks homebrew.page whether a newer ProsperoTV is
// listed (the console: ps5/src/tv_update.cpp), and can replace the app with
// it: the release is downloaded and unpacked beside the app, and its files
// take the place of the old ones once the app has closed. Nothing is changed
// before update_apply(), and a cancel or a failure leaves the app as it was.
struct UpdateOffer
{
    bool installable = false; // the app can install it itself
    std::string version;      // the release's name, for the screen
    std::string installed;    // this build's content version
    std::string available;    // the release's content version
    std::uint64_t size = 0;   // the download in bytes; 0 when the catalog does not say
    // What the developer wrote on the release, as the catalog gives it: plain
    // text, lines split by \n, list items starting "- ". Empty when it has none.
    std::string notes;
    bool notes_truncated = false; // the catalog cut them; the rest is on the app's page
};
enum class UpdatePhase : std::uint8_t
{
    idle,
    starting,    // the helper is being started
    downloading, // done/total are bytes of the download
    unpacking,   // done/total are bytes unpacked
    ready,       // staged: update_apply() or update_cancel()
    applying,    // the helper waits for the app to close: close it now
    cancelled,
    failed, // error says why; nothing was changed
};
struct UpdateProgress
{
    UpdatePhase phase = UpdatePhase::idle;
    std::uint64_t done = 0;
    std::uint64_t total = 0; // 0 while it is not known
    std::string time_left;   // "about 20 s left"; empty until it can be said
    std::string error;
};
// A newer release, once, when the answer has come; false while there is
// nothing (yet) to tell.
bool update_take(UpdateOffer *offer);
// The update of the offered release. update_begin: true when it has begun.
// update_apply: true when the files will be replaced once the app closes.
// update_finish: after a cancel or a failure, before beginning again.
bool update_begin();
UpdateProgress update_poll();
void update_cancel();
bool update_apply();
void update_finish();

} // namespace ptv::platform
