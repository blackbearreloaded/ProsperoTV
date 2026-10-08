/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_XTREAM_H
#define IPTV_XTREAM_H

#include "iptv_catalog.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace iptv
{

inline constexpr char kDefaultXtreamCredentialsPath[] = "/download0/prosperotv-xtream-v1.txt";
inline constexpr std::size_t kMaxXtreamServerBytes = 1020u;
inline constexpr std::size_t kMaxXtreamCredentialBytes = 255u;
// The list of live streams: about 600 bytes a channel, read as it arrives.
inline constexpr std::size_t kMaxXtreamResponseBytes = 512u * 1024u * 1024u;
// The sign-in and the categories are small answers, read whole.
inline constexpr std::size_t kMaxXtreamReplyBytes = 4u * 1024u * 1024u;

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
    std::string parent_id{};
};

bool NormalizeXtreamServerUrl(std::string_view input, std::string *normalized);
bool ValidateXtreamCredentials(const XtreamCredentials &credentials);
std::uint64_t XtreamSourceId(const XtreamCredentials &credentials);

bool BuildXtreamApiUrl(const XtreamCredentials &credentials, std::string_view action,
                       std::string *url);
bool BuildXtreamGuideUrl(const XtreamCredentials &credentials, std::string *url);
bool BuildXtreamLiveUrl(const XtreamCredentials &credentials, std::string_view stream_id,
                        std::string_view extension, std::string *url);
bool BuildXtreamMediaUrl(const XtreamCredentials &credentials, bool episode,
                         std::string_view stream_id, std::string_view extension, std::string *url);
bool BuildXtreamSeriesUrl(const XtreamCredentials &credentials, std::string_view series_id,
                          std::string *url);

XtreamStatus SaveXtreamCredentials(const std::string &path, const XtreamCredentials &credentials);
XtreamStatus LoadXtreamCredentials(const std::string &path, XtreamCredentials *credentials);
XtreamStatus SaveXtreamCredentials(const XtreamCredentials &credentials);
XtreamStatus LoadXtreamCredentials(XtreamCredentials *credentials);

XtreamStatus ParseXtreamAuth(std::string_view json, XtreamAuth *auth);
XtreamStatus ParseXtreamCategories(std::string_view json, std::vector<XtreamCategory> *categories);
// Reads the answer to get_live_streams as it arrives: Feed it the bytes in
// order, in pieces of any size, then Finish. Each stream goes into `catalog`
// when its last byte is in, so the answer (tens of megabytes for a large
// provider) is never held whole. Past max_channels the streams are counted in
// the report and left out; full() says so, which is when a download can stop.
class XtreamStreamsParser
{
  public:
    XtreamStreamsParser(const XtreamCredentials &credentials,
                        const std::vector<XtreamCategory> &categories, std::uint64_t source_id,
                        Catalog *catalog, ParseReport *report = nullptr,
                        std::size_t max_channels = kDefaultMaxChannels);
    ~XtreamStreamsParser();
    XtreamStreamsParser(const XtreamStreamsParser &) = delete;
    XtreamStreamsParser &operator=(const XtreamStreamsParser &) = delete;

    // False once the answer cannot be a list of streams this app reads.
    bool Feed(std::string_view bytes);
    // What became of it. Anything but ok and the catalog is empty.
    XtreamStatus Finish();
    bool full() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// The same, for an answer that is already whole in memory.
XtreamStatus ParseXtreamLiveStreams(std::string_view json, const XtreamCredentials &credentials,
                                    const std::vector<XtreamCategory> &categories,
                                    std::uint64_t source_id, Catalog *catalog,
                                    ParseReport *report = nullptr,
                                    std::size_t max_channels = kDefaultMaxChannels);

const char *XtreamStatusDescription(XtreamStatus status);

} // namespace iptv

#endif
