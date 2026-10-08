// ProsperoTV - Home-network tuner and TV server sources.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tv/library.hpp"
#include "iptv_catalog.h"
#include "iptv_http.h"

namespace ptv
{
bool local_tv_address(std::string_view input, std::string *address);
std::uint64_t local_tv_source_id(const SavedSource &source);
std::string local_tv_authorization(const SavedSource &source);
bool parse_hdhomerun(std::string_view json, std::uint64_t source, iptv::Catalog *catalog,
                     iptv::ParseReport *report);
iptv::http::FetchResult load_local_tv(const SavedSource &source, iptv::Catalog *catalog,
                                      iptv::ParseReport *report,
                                      const iptv::http::RequestControl *control);
} // namespace ptv
