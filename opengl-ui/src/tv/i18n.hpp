// ProsperoTV - Menu translations selected from the console language.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ptv
{
// Call at startup before constructing screens. Unknown languages use English.
void set_console_language(int language);
const char *interface_language();
// Only presentation text belongs here, never source IDs or provider metadata.
const char *tr(const char *english);
} // namespace ptv
