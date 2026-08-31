/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_XTREAM_H
#define IPTV_XTREAM_H

#include "iptv_catalog.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iptv
{

inline constexpr char kDefaultXtreamCredentialsPath[] =
    "/download0/prosperotv-xtream-v1.txt";
inline constexpr std::size_t kMaxXtreamServerBytes = 1020u;
inline constexpr std::size_t kMaxXtreamCredentialBytes = 255u;
inline constexpr std::size_t kMaxXtreamResponseBytes = 16u * 1024u * 1024u;

enum class XtreamStatus : std::uint8_t
{
    ok,
    invalid_argument,
    not_found,
    too_large,
    io_error,
    corrupt,
    malformed_json,
    authentication_failed,
    account_inactive,
    no_channels,
};

struct XtreamCredentials
{
    std::string server_url;
    std::string username;
    std::string password;
};

struct XtreamAuth
{
    bool authenticated = false;
    std::string status;
    std::string message;
};

struct XtreamCategory
{
    std::string id;
    std::string name;
};

bool NormalizeXtreamServerUrl(std::string_view input, std::string *normalized);
bool ValidateXtreamCredentials(const XtreamCredentials &credentials);
std::uint64_t XtreamSourceId(const XtreamCredentials &credentials);

bool BuildXtreamApiUrl(const XtreamCredentials &credentials, std::string_view action,
                       std::string *url);
bool BuildXtreamLiveUrl(const XtreamCredentials &credentials, std::string_view stream_id,
                        std::string_view extension, std::string *url);

XtreamStatus SaveXtreamCredentials(const std::string &path,
                                    const XtreamCredentials &credentials);
XtreamStatus LoadXtreamCredentials(const std::string &path, XtreamCredentials *credentials);
XtreamStatus SaveXtreamCredentials(const XtreamCredentials &credentials);
XtreamStatus LoadXtreamCredentials(XtreamCredentials *credentials);

XtreamStatus ParseXtreamAuth(std::string_view json, XtreamAuth *auth);
XtreamStatus ParseXtreamCategories(std::string_view json,
                                   std::vector<XtreamCategory> *categories);
XtreamStatus ParseXtreamLiveStreams(std::string_view json,
                                    const XtreamCredentials &credentials,
                                    const std::vector<XtreamCategory> &categories,
                                    std::uint64_t source_id, CatalogState *catalog,
                                    ParseReport *report = nullptr);

const char *XtreamStatusDescription(XtreamStatus status);

} // namespace iptv

#endif
