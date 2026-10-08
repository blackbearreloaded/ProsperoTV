// ProsperoTV - Independent, cancellable artwork downloads.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/platform.hpp"
#include "tv_http.h"
#include "update_kit/console_curl.h"
#include <curl/curl.h>

namespace ptv::platform
{
namespace
{
struct Download
{
    std::vector<std::uint8_t> *bytes;
    const iptv::http::RequestControl *control;
};
std::size_t receive(char *data, std::size_t size, std::size_t count, void *context)
{
    auto &download = *static_cast<Download *>(context);
    constexpr std::size_t limit = 2u * 1024u * 1024u;
    if (size != 0 && count > (limit - download.bytes->size()) / size)
        return 0;
    const auto *begin = reinterpret_cast<const std::uint8_t *>(data);
    download.bytes->insert(download.bytes->end(), begin, begin + size * count);
    return size * count;
}
int progress(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto *control = static_cast<Download *>(context)->control;
    return control && control->cancelled && control->cancelled(control->context) ? 1 : 0;
}
} // namespace

bool fetch_image(const char *url, std::vector<std::uint8_t> *bytes,
                 const iptv::http::RequestControl *control)
{
    bytes->clear();
    if (!iptv::http::IsSupportedPlaylistUrl(url) || tv_http_init(0, 0, 0) < 0)
        return false;
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;
    Download download{bytes, control};
    console_curl_setup(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ProsperoTV");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &download);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result == CURLE_OK && status == 200 && !progress(&download, 0, 0, 0, 0))
        return true;
    bytes->clear();
    return false;
}
} // namespace ptv::platform
