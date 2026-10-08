// ProsperoTV - The diagnostic log: what the app did, written down while the viewer asks for it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings has a switch, off by default. While it is on, the interface, the
// catalog, the player and the calls the decoders make to the system each say
// what they do, one line at a time, and the machine keeps the lines (the
// console: logs/debug-trace.txt, see ps5/src/tv_diag.cpp). The lines are meant
// to be sent to someone else: nothing that identifies the viewer's account
// goes into one (addresses pass through redact_address() first).

#pragma once

namespace ptv::diag
{

// Where a line goes. Called from any thread; the text has no newline.
using Sink = void (*)(const char *line);
void set_sink(Sink sink);

// The viewer's switch, and a build's own (a debug build, a scripted run).
void set_enabled(bool enabled);
void set_forced(bool forced);
bool enabled();

// One line, when the log is on. Costs a load and a branch when it is off.
void event(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace ptv::diag

// The same for the C sources (the player's backend, the decoder trace).
extern "C" int tv_diag_enabled(void);
extern "C" void tv_diag_line(const char *line);
