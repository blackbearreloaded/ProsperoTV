/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_http.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <time.h>

namespace iptv::http
{
namespace
{

bool IsAsciiSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\v' ||
           value == '\f';
}

bool IsControl(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    return byte < 0x20u || byte == 0x7fu;
}

char LowerAscii(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    return byte < 128u ? static_cast<char>(std::tolower(byte)) : value;
}

bool EqualsAsciiInsensitive(const char *value, const char *expected, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i)
    {
        if (LowerAscii(value[i]) != expected[i])
            return false;
    }
    return true;
}

bool ContainsAsciiInsensitive(const char *value, std::size_t value_length, const char *needle)
{
    if (!value || !needle || !*needle)
        return false;
    const std::size_t needle_length = std::strlen(needle);
    if (needle_length > value_length)
        return false;
    for (std::size_t offset = 0; offset <= value_length - needle_length; ++offset)
    {
        std::size_t index = 0;
        while (index < needle_length &&
               LowerAscii(value[offset + index]) == LowerAscii(needle[index]))
        {
            ++index;
        }
        if (index == needle_length)
            return true;
    }
    return false;
}

bool IsDigits(const char *value, std::size_t length)
{
    if (length == 0)
        return false;
    for (std::size_t i = 0; i < length; ++i)
    {
        if (value[i] < '0' || value[i] > '9')
            return false;
    }
    return true;
}

std::size_t BoundedLength(const char *value, std::size_t limit)
{
    if (!value)
        return limit + 1u;
    std::size_t length = 0;
    while (length <= limit && value[length] != '\0')
        ++length;
    return length;
}

bool CopyText(char *output, std::size_t capacity, const char *value, std::size_t length)
{
    if (!output || capacity == 0 || length >= capacity)
        return false;
    if (length != 0)
        std::memcpy(output, value, length);
    output[length] = '\0';
    return true;
}

bool StartsAt(const char *value, std::size_t length, std::size_t offset, const char *prefix,
              std::size_t prefix_length)
{
    return offset <= length && prefix_length <= length - offset &&
           std::memcmp(value + offset, prefix, prefix_length) == 0;
}

bool AppendText(char *output, std::size_t capacity, std::size_t *length, const char *value,
                std::size_t value_length)
{
    if (!length || *length >= capacity || value_length >= capacity - *length)
        return false;
    if (value_length != 0)
        std::memcpy(output + *length, value, value_length);
    *length += value_length;
    output[*length] = '\0';
    return true;
}

bool IsSupportedUrl(const char *url)
{
    if (url == nullptr)
        return false;

    std::size_t length = 0;
    while (length <= kMaxUrlBytes && url[length] != '\0')
        ++length;
    if (length == 0 || length > kMaxUrlBytes)
        return false;
    for (std::size_t i = 0; i < length; ++i)
    {
        if (IsControl(url[i]) || IsAsciiSpace(url[i]) || url[i] == '\\')
        {
            return false;
        }
    }

    const std::size_t scheme_length = length >= 7 && EqualsAsciiInsensitive(url, "http", 4) &&
                                              url[4] == ':' && url[5] == '/' && url[6] == '/'
                                          ? 7
                                      : length >= 8 && EqualsAsciiInsensitive(url, "https", 5) &&
                                              url[5] == ':' && url[6] == '/' && url[7] == '/'
                                          ? 8
                                          : 0;
    if (scheme_length == 0)
        return false;

    const std::size_t authority_start = scheme_length;
    std::size_t authority_end = authority_start;
    while (authority_end < length && url[authority_end] != '/' && url[authority_end] != '?' &&
           url[authority_end] != '#')
    {
        ++authority_end;
    }
    const std::size_t authority_length = authority_end - authority_start;
    if (authority_length == 0)
        return false;
    for (std::size_t i = 0; i < authority_length; ++i)
    {
        if (url[authority_start + i] == '@')
            return false;
    }

    const char *authority = url + authority_start;
    if (authority[0] == '[')
    {
        std::size_t close = 1;
        while (close < authority_length && authority[close] != ']')
            ++close;
        if (close == 1 || close == authority_length)
            return false;
        if (close + 1 < authority_length &&
            (authority[close + 1] != ':' ||
             !IsDigits(authority + close + 2, authority_length - close - 2)))
        {
            return false;
        }
    }
    else
    {
        std::size_t port_separator = authority_length;
        for (std::size_t i = 0; i < authority_length; ++i)
        {
            if (authority[i] == ':' && port_separator == authority_length)
            {
                port_separator = i;
            }
            else if (authority[i] == ':' || authority[i] == '[' || authority[i] == ']')
            {
                return false;
            }
        }
        if (port_separator == 0 ||
            (port_separator != authority_length &&
             !IsDigits(authority + port_separator + 1, authority_length - port_separator - 1)))
        {
            return false;
        }
    }
    return true;
}

void RemoveLastPathSegment(char *output, std::size_t *output_length)
{
    if (!output_length || *output_length == 0)
        return;
    std::size_t position = *output_length;
    while (position > 0 && output[position - 1u] != '/')
        --position;
    *output_length = position == 0 ? 0 : position - 1u;
    output[*output_length] = '\0';
}

bool NormalizePath(const char *path, std::size_t path_length, char *output, std::size_t capacity,
                   std::size_t *output_length)
{
    if (!path || !output || !output_length || capacity == 0)
        return false;
    std::size_t input = 0;
    std::size_t written = 0;
    output[0] = '\0';
    while (input < path_length)
    {
        if (StartsAt(path, path_length, input, "../", 3u))
        {
            input += 3u;
        }
        else if (StartsAt(path, path_length, input, "./", 2u))
        {
            input += 2u;
        }
        else if (StartsAt(path, path_length, input, "/./", 3u))
        {
            input += 2u;
        }
        else if (StartsAt(path, path_length, input, "/.", 2u) && input + 2u == path_length)
        {
            input += 2u;
            if (!AppendText(output, capacity, &written, "/", 1u))
                return false;
        }
        else if (StartsAt(path, path_length, input, "/../", 4u))
        {
            input += 3u;
            RemoveLastPathSegment(output, &written);
        }
        else if (StartsAt(path, path_length, input, "/..", 3u) && input + 3u == path_length)
        {
            input += 3u;
            RemoveLastPathSegment(output, &written);
            if (!AppendText(output, capacity, &written, "/", 1u))
                return false;
        }
        else if ((StartsAt(path, path_length, input, ".", 1u) && input + 1u == path_length) ||
                 (StartsAt(path, path_length, input, "..", 2u) && input + 2u == path_length))
        {
            input = path_length;
        }
        else
        {
            std::size_t end = input;
            if (path[end] == '/')
                ++end;
            while (end < path_length && path[end] != '/')
                ++end;
            if (!AppendText(output, capacity, &written, path + input, end - input))
            {
                return false;
            }
            input = end;
        }
    }
    *output_length = written;
    return true;
}

bool NormalizeAbsoluteUrl(const char *candidate, char *output, std::size_t capacity)
{
    if (!IsSupportedUrl(candidate))
        return false;
    const std::size_t length = BoundedLength(candidate, kMaxUrlBytes);
    std::size_t authority_start = 0;
    while (authority_start + 2u < length &&
           !(candidate[authority_start] == ':' && candidate[authority_start + 1u] == '/' &&
             candidate[authority_start + 2u] == '/'))
    {
        ++authority_start;
    }
    authority_start += 3u;
    std::size_t path_start = authority_start;
    while (path_start < length && candidate[path_start] != '/' && candidate[path_start] != '?' &&
           candidate[path_start] != '#')
    {
        ++path_start;
    }
    std::size_t path_end = path_start;
    while (path_end < length && candidate[path_end] != '?' && candidate[path_end] != '#')
    {
        ++path_end;
    }

    char normalized_path[kMaxUrlBytes + 1u] = {};
    std::size_t normalized_length = 0;
    if (path_start < path_end &&
        !NormalizePath(candidate + path_start, path_end - path_start, normalized_path,
                       sizeof(normalized_path), &normalized_length))
    {
        return false;
    }

    std::size_t written = 0;
    if (!output || capacity == 0)
        return false;
    output[0] = '\0';
    return AppendText(output, capacity, &written, candidate, path_start) &&
           AppendText(output, capacity, &written, normalized_path, normalized_length) &&
           AppendText(output, capacity, &written, candidate + path_end, length - path_end) &&
           IsSupportedUrl(output);
}

bool BuildRedirectCandidate(const char *base_url, const char *location, char *candidate,
                            std::size_t capacity)
{
    const std::size_t base_length = BoundedLength(base_url, kMaxUrlBytes);
    const std::size_t location_length = BoundedLength(location, kMaxUrlBytes);
    if (base_length == 0 || base_length > kMaxUrlBytes || location_length == 0 ||
        location_length > kMaxUrlBytes || !IsSupportedUrl(base_url) || !candidate || capacity == 0)
    {
        return false;
    }
    for (std::size_t i = 0; i < location_length; ++i)
    {
        if (IsControl(location[i]) || IsAsciiSpace(location[i]) || location[i] == '\\')
        {
            return false;
        }
    }

    std::size_t scheme_end = 0;
    while (scheme_end < base_length && base_url[scheme_end] != ':')
        ++scheme_end;
    if (scheme_end == base_length)
        return false;
    std::size_t authority_end = scheme_end + 3u;
    while (authority_end < base_length && base_url[authority_end] != '/' &&
           base_url[authority_end] != '?' && base_url[authority_end] != '#')
    {
        ++authority_end;
    }

    std::size_t relative_scheme = 0;
    while (relative_scheme < location_length && location[relative_scheme] != ':' &&
           location[relative_scheme] != '/' && location[relative_scheme] != '?' &&
           location[relative_scheme] != '#')
    {
        ++relative_scheme;
    }

    std::size_t written = 0;
    candidate[0] = '\0';
    if (relative_scheme < location_length && location[relative_scheme] == ':')
    {
        return CopyText(candidate, capacity, location, location_length);
    }
    if (location_length >= 2u && location[0] == '/' && location[1] == '/')
    {
        return AppendText(candidate, capacity, &written, base_url, scheme_end) &&
               AppendText(candidate, capacity, &written, ":", 1u) &&
               AppendText(candidate, capacity, &written, location, location_length);
    }
    if (location[0] == '/')
    {
        return AppendText(candidate, capacity, &written, base_url, authority_end) &&
               AppendText(candidate, capacity, &written, location, location_length);
    }

    std::size_t base_suffix = authority_end;
    while (base_suffix < base_length && base_url[base_suffix] != '?' &&
           base_url[base_suffix] != '#')
    {
        ++base_suffix;
    }
    if (location[0] == '?')
    {
        return AppendText(candidate, capacity, &written, base_url, base_suffix) &&
               AppendText(candidate, capacity, &written, location, location_length);
    }
    if (location[0] == '#')
    {
        std::size_t fragment = base_suffix;
        while (fragment < base_length && base_url[fragment] != '#')
            ++fragment;
        return AppendText(candidate, capacity, &written, base_url, fragment) &&
               AppendText(candidate, capacity, &written, location, location_length);
    }

    std::size_t directory_end = base_suffix;
    while (directory_end > authority_end && base_url[directory_end - 1u] != '/')
    {
        --directory_end;
    }
    if (directory_end == authority_end)
    {
        if (!AppendText(candidate, capacity, &written, base_url, authority_end) ||
            !AppendText(candidate, capacity, &written, "/", 1u))
        {
            return false;
        }
    }
    else if (!AppendText(candidate, capacity, &written, base_url, directory_end))
    {
        return false;
    }
    return AppendText(candidate, capacity, &written, location, location_length);
}

} // namespace

bool IsSupportedPlaylistUrl(const char *url)
{
    return IsSupportedUrl(url);
}

Status ResolveRedirectUrl(const char *base_url, const char *location, char *resolved_url,
                          std::size_t resolved_capacity, const char *const *visited_urls,
                          std::size_t visited_count)
{
    if (resolved_url && resolved_capacity != 0)
        resolved_url[0] = '\0';
    if ((visited_count != 0 && !visited_urls) || visited_count > kMaxRedirects)
        return Status::redirect_error;
    char candidate[kMaxUrlBytes + 1u] = {};
    if (!BuildRedirectCandidate(base_url, location, candidate, sizeof(candidate)) ||
        !NormalizeAbsoluteUrl(candidate, resolved_url, resolved_capacity))
    {
        return Status::redirect_error;
    }
    for (std::size_t i = 0; i < visited_count; ++i)
    {
        if (!visited_urls[i] || std::strcmp(visited_urls[i], resolved_url) == 0)
        {
            resolved_url[0] = '\0';
            return Status::redirect_error;
        }
    }
    return Status::ok;
}

bool ResponseIndicatesGeographicBlock(const char *response, std::size_t bytes)
{
    if (!response || bytes == 0)
        return false;
    constexpr const char *patterns[] = {
        "geoip",
        "geo-block",
        "geo block",
        "geoblock",
        "geographic restriction",
        "geographical restriction",
        "not available in your country",
        "not available in your region",
        "country is blocked",
        "region is blocked",
        "country not allowed",
        "region not allowed",
    };
    for (const char *pattern : patterns)
    {
        if (ContainsAsciiInsensitive(response, bytes, pattern))
            return true;
    }
    return false;
}

void DescribeFailure(Status status, int http_status, int native_error, const char *response,
                     char *description, std::size_t description_capacity)
{
    if (!description || description_capacity == 0)
        return;
    description[0] = '\0';
    const std::size_t response_bytes = response ? std::strlen(response) : 0u;
    if (status == Status::http_status_error)
    {
        if (ResponseIndicatesGeographicBlock(response, response_bytes))
        {
            std::snprintf(description, description_capacity,
                          "unavailable in your region (GeoIP blocked; HTTP %d)", http_status);
            return;
        }
        switch (http_status)
        {
        case 400:
            std::snprintf(description, description_capacity,
                          "the provider rejected the stream request (HTTP 400)");
            return;
        case 401:
            std::snprintf(description, description_capacity,
                          "the stream requires authentication (HTTP 401)");
            return;
        case 403:
            std::snprintf(description, description_capacity,
                          "access denied by the channel provider (HTTP 403)");
            return;
        case 404:
            std::snprintf(description, description_capacity,
                          "stream is offline or no longer exists (HTTP 404)");
            return;
        case 408:
            std::snprintf(description, description_capacity,
                          "the channel provider timed out (HTTP 408)");
            return;
        case 410:
            std::snprintf(description, description_capacity,
                          "stream is no longer available (HTTP 410)");
            return;
        case 429:
            std::snprintf(description, description_capacity,
                          "the channel provider is rate limiting requests (HTTP 429)");
            return;
        case 451:
            std::snprintf(description, description_capacity,
                          "unavailable for legal or regional restrictions (HTTP 451)");
            return;
        default:
            if (http_status >= 500 && http_status <= 599)
            {
                std::snprintf(description, description_capacity,
                              "the channel provider is unavailable (HTTP %d)", http_status);
                return;
            }
            std::snprintf(description, description_capacity,
                          "the provider rejected the request (HTTP %d)", http_status);
            return;
        }
    }

    switch (status)
    {
    case Status::ok:
        std::snprintf(description, description_capacity, "no error");
        break;
    case Status::invalid_argument:
        std::snprintf(description, description_capacity, "invalid stream request");
        break;
    case Status::unsupported_url:
        std::snprintf(description, description_capacity, "unsupported stream URL");
        break;
    case Status::not_initialized:
    case Status::platform_unavailable:
    case Status::network_init_failed:
        std::snprintf(description, description_capacity, "network service unavailable");
        break;
    case Status::request_failed:
        if (native_error != 0)
            std::snprintf(description, description_capacity,
                          "connection failed (native error 0x%08X)",
                          static_cast<unsigned>(native_error));
        else
            std::snprintf(description, description_capacity, "connection failed");
        break;
    case Status::response_too_large:
        std::snprintf(description, description_capacity, "provider response is too large");
        break;
    case Status::read_failed:
        std::snprintf(description, description_capacity, "connection was interrupted");
        break;
    case Status::deadline_exceeded:
        std::snprintf(description, description_capacity, "connection timed out");
        break;
    case Status::redirect_error:
        std::snprintf(description, description_capacity, "provider returned an invalid redirect");
        break;
    case Status::cancelled:
        std::snprintf(description, description_capacity, "request was cancelled");
        break;
    case Status::http_status_error:
        break;
    }
}

#if defined(__PROSPERO__) || defined(__ORBIS__)

namespace
{

constexpr int kHttpVersion11 = 2;
constexpr int kHttpMethodGet = 0;
constexpr std::uint32_t kHeaderOverwrite = 0u;
constexpr std::uint32_t kStreamReceiveTimeoutUsec = 2000000u;
constexpr std::size_t kMaxResponseHeaderBytes = 64u * 1024u;

extern "C"
{
    extern int sceNetPoolCreate(const char *name, int size, int flags);
    extern int sceNetPoolDestroy(int mem_id);
    extern int sceSslInit(std::size_t pool_size);
    extern int sceSslTerm(int ssl_context_id);
    extern int sceHttpInit(int net_mem_id, int ssl_context_id, std::size_t pool_size);
    extern int sceHttpTerm(int http_context_id);
    extern int sceHttpCreateTemplate(int http_context_id, const char *user_agent, int version,
                                     int auto_proxy);
    extern int sceHttpDeleteTemplate(int template_id);
    extern int sceHttpCreateConnectionWithURL(int template_id, const char *url, int keep_alive);
    extern int sceHttpDeleteConnection(int connection_id);
    extern int sceHttpCreateRequestWithURL(int connection_id, int method, const char *url,
                                           std::uint64_t content_length);
    extern int sceHttpAbortRequest(int request_id);
    extern int sceHttpDeleteRequest(int request_id);
    extern int sceHttpAddRequestHeader(int request_id, const char *name, const char *value,
                                       std::uint32_t mode);
    extern int sceHttpSetAutoRedirect(int id, int enabled);
    extern int sceHttpSetConnectTimeOut(int id, std::uint32_t usec);
    extern int sceHttpSetRecvTimeOut(int id, std::uint32_t usec);
    extern int sceHttpSetSendTimeOut(int id, std::uint32_t usec);
    extern int sceHttpSetResolveTimeOut(int id, std::uint32_t usec);
    extern int sceHttpSendRequest(int request_id, const void *data, std::size_t size);
    extern int sceHttpGetStatusCode(int request_id, int *status_code);
    extern int sceHttpGetAllResponseHeaders(int request_id, char **headers, std::size_t *size);
    extern int sceHttpReadData(int request_id, void *data, std::size_t size);
}

int g_net_pool = -1;
int g_ssl_context = -1;
int g_http_context = -1;
int g_http_template = -1;
std::atomic_flag g_active_request_guard = ATOMIC_FLAG_INIT;
int g_active_playlist_request = -1;

void LockActiveRequest()
{
    while (g_active_request_guard.test_and_set(std::memory_order_acquire))
    {
    }
}

void UnlockActiveRequest()
{
    g_active_request_guard.clear(std::memory_order_release);
}

void TrackPlaylistRequest(int request)
{
    LockActiveRequest();
    g_active_playlist_request = request;
    UnlockActiveRequest();
}

void CloseRequest(int connection, int request)
{
    if (request >= 0)
    {
        LockActiveRequest();
        if (g_active_playlist_request == request)
            g_active_playlist_request = -1;
        sceHttpDeleteRequest(request);
        UnlockActiveRequest();
    }
    if (connection >= 0)
        sceHttpDeleteConnection(connection);
}

FetchResult Failure(Status status, int native_error = 0, int http_status = 0, std::size_t bytes = 0)
{
    return {status, bytes, http_status, native_error};
}

bool IsRedirectStatus(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool ExtractLocation(int request, char *location, std::size_t capacity)
{
    if (!location || capacity == 0)
        return false;
    location[0] = '\0';
    char *headers = nullptr;
    std::size_t size = 0;
    if (sceHttpGetAllResponseHeaders(request, &headers, &size) < 0 || !headers || size == 0 ||
        size > kMaxResponseHeaderBytes)
    {
        return false;
    }

    bool found = false;
    std::size_t position = 0;
    while (position < size)
    {
        const std::size_t line_start = position;
        while (position < size && headers[position] != '\n')
            ++position;
        std::size_t line_end = position;
        if (position < size)
            ++position;
        if (line_end > line_start && headers[line_end - 1u] == '\r')
            --line_end;

        std::size_t colon = line_start;
        while (colon < line_end && headers[colon] != ':')
            ++colon;
        constexpr char kLocation[] = "location";
        if (colon - line_start != sizeof(kLocation) - 1u ||
            !EqualsAsciiInsensitive(headers + line_start, kLocation, sizeof(kLocation) - 1u))
        {
            continue;
        }
        if (found)
            return false;
        std::size_t value_start = colon + 1u;
        while (value_start < line_end &&
               (headers[value_start] == ' ' || headers[value_start] == '\t'))
        {
            ++value_start;
        }
        while (line_end > value_start &&
               (headers[line_end - 1u] == ' ' || headers[line_end - 1u] == '\t'))
        {
            --line_end;
        }
        const std::size_t value_length = line_end - value_start;
        if (value_length == 0 || value_length > kMaxUrlBytes ||
            !CopyText(location, capacity, headers + value_start, value_length))
        {
            return false;
        }
        found = true;
    }
    return found;
}

std::size_t ReadErrorResponse(int request, char *response, std::size_t capacity)
{
    if (!response || capacity == 0)
        return 0;
    response[0] = '\0';
    if (request < 0 || capacity == 1)
        return 0;
    const int read = sceHttpReadData(request, response, capacity - 1u);
    if (read <= 0 || static_cast<std::size_t>(read) >= capacity)
        return 0;
    response[read] = '\0';
    return static_cast<std::size_t>(read);
}

std::uint64_t MonotonicUsec()
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * UINT64_C(1000000) +
           static_cast<std::uint64_t>(now.tv_nsec) / UINT64_C(1000);
}

struct RedirectHistory
{
    char urls[kMaxRedirects + 1u][kMaxUrlBytes + 1u] = {};
    std::size_t count = 0;

    bool Begin(const char *url)
    {
        const std::size_t length = BoundedLength(url, kMaxUrlBytes);
        if (length == 0 || length > kMaxUrlBytes ||
            !CopyText(urls[0], sizeof(urls[0]), url, length))
        {
            return false;
        }
        count = 1;
        return true;
    }

    const char *Current() const
    {
        return count == 0 ? "" : urls[count - 1u];
    }

    bool Follow(const char *location)
    {
        if (count == 0 || count > kMaxRedirects)
            return false;
        const char *visited[kMaxRedirects + 1u] = {};
        for (std::size_t i = 0; i < count; ++i)
            visited[i] = urls[i];
        char next[kMaxUrlBytes + 1u] = {};
        if (ResolveRedirectUrl(Current(), location, next, sizeof(next), visited, count) !=
            Status::ok)
        {
            return false;
        }
        const std::size_t length = BoundedLength(next, kMaxUrlBytes);
        if (!CopyText(urls[count], sizeof(urls[count]), next, length))
            return false;
        ++count;
        return true;
    }
};

bool SafeHeaderValue(const char *value)
{
    if (!value || !*value)
        return true;
    const std::size_t length = BoundedLength(value, 2048u);
    if (length == 0 || length > 2048u)
        return false;
    for (std::size_t index = 0; index < length; ++index)
    {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x20u || byte == 0x7fu)
            return false;
    }
    return true;
}

int ConfigureRequest(int request, const char *accept, std::uint32_t receive_timeout,
                     const RequestHeaders *headers)
{
    int result = sceHttpSetAutoRedirect(request, 0);
    if (result >= 0)
        result = sceHttpSetResolveTimeOut(request, kResolveTimeoutUsec);
    if (result >= 0)
        result = sceHttpSetConnectTimeOut(request, kConnectTimeoutUsec);
    if (result >= 0)
        result = sceHttpSetSendTimeOut(request, kSendTimeoutUsec);
    if (result >= 0)
        result = sceHttpSetRecvTimeOut(request, receive_timeout);
    if (result >= 0 && accept && *accept)
    {
        result = sceHttpAddRequestHeader(request, "Accept", accept, kHeaderOverwrite);
    }
    if (result >= 0 && headers && headers->user_agent && *headers->user_agent)
    {
        if (!SafeHeaderValue(headers->user_agent))
            return -1;
        result =
            sceHttpAddRequestHeader(request, "User-Agent", headers->user_agent, kHeaderOverwrite);
    }
    if (result >= 0 && headers && headers->referrer && *headers->referrer)
    {
        if (!SafeHeaderValue(headers->referrer))
            return -1;
        result = sceHttpAddRequestHeader(request, "Referer", headers->referrer, kHeaderOverwrite);
    }
    return result;
}

} // namespace

Status NetworkInit()
{
    if (g_http_template >= 0)
        return Status::ok;

    NetworkShutdown();
    g_net_pool = sceNetPoolCreate("ps5_iptv_http", static_cast<int>(kNetPoolSize), 0);
    if (g_net_pool < 0)
    {
        NetworkShutdown();
        return Status::network_init_failed;
    }
    g_ssl_context = sceSslInit(kSslPoolSize);
    if (g_ssl_context < 0)
    {
        NetworkShutdown();
        return Status::network_init_failed;
    }
    g_http_context = sceHttpInit(g_net_pool, g_ssl_context, kHttpPoolSize);
    if (g_http_context < 0)
    {
        NetworkShutdown();
        return Status::network_init_failed;
    }
    g_http_template = sceHttpCreateTemplate(g_http_context, "PS5IPTV/1.0", kHttpVersion11, 1);
    if (g_http_template < 0)
    {
        NetworkShutdown();
        return Status::network_init_failed;
    }
    if (sceHttpSetAutoRedirect(g_http_template, 0) < 0 ||
        sceHttpSetResolveTimeOut(g_http_template, kResolveTimeoutUsec) < 0 ||
        sceHttpSetConnectTimeOut(g_http_template, kConnectTimeoutUsec) < 0 ||
        sceHttpSetSendTimeOut(g_http_template, kSendTimeoutUsec) < 0 ||
        sceHttpSetRecvTimeOut(g_http_template, kReceiveTimeoutUsec) < 0)
    {
        NetworkShutdown();
        return Status::network_init_failed;
    }
    return Status::ok;
}

void NetworkShutdown()
{
    if (g_http_template >= 0)
        sceHttpDeleteTemplate(g_http_template);
    if (g_http_context >= 0)
        sceHttpTerm(g_http_context);
    if (g_ssl_context >= 0)
        sceSslTerm(g_ssl_context);
    if (g_net_pool >= 0)
        sceNetPoolDestroy(g_net_pool);
    g_http_template = -1;
    g_http_context = -1;
    g_ssl_context = -1;
    g_net_pool = -1;
}

void CancelActivePlaylistRequest()
{
    LockActiveRequest();
    const int request = g_active_playlist_request;
    if (request >= 0)
        (void)sceHttpAbortRequest(request);
    UnlockActiveRequest();
}

FetchResult GetM3u(const char *url, char *buffer, std::size_t buffer_capacity,
                   std::size_t max_bytes, const RequestHeaders *headers,
                   const RequestControl *control)
{
    char effective_url[kMaxUrlBytes + 1u] = {};
    return GetM3uResolved(url, buffer, buffer_capacity, effective_url, sizeof(effective_url),
                          max_bytes, headers, control);
}

FetchResult GetM3uResolved(const char *url, char *buffer, std::size_t buffer_capacity,
                           char *effective_url, std::size_t effective_url_capacity,
                           std::size_t max_bytes, const RequestHeaders *headers,
                           const RequestControl *control)
{
    if (effective_url && effective_url_capacity != 0)
        effective_url[0] = '\0';
    if (!IsSupportedUrl(url))
        return Failure(Status::unsupported_url);
    if (buffer == nullptr || max_bytes == 0 || max_bytes > kHardMaxPlaylistBytes ||
        buffer_capacity < max_bytes + 1 || !effective_url || effective_url_capacity == 0)
    {
        return Failure(Status::invalid_argument);
    }
    buffer[0] = '\0';
    RedirectHistory history;
    if (!history.Begin(url) || !CopyText(effective_url, effective_url_capacity, history.Current(),
                                         std::strlen(history.Current())))
    {
        return Failure(Status::invalid_argument);
    }
    if (g_http_template < 0)
        return Failure(Status::not_initialized);

    int connection = -1;
    int request = -1;
    int result = 0;
    int http_status = 0;
    for (;;)
    {
        if (control && control->cancelled && control->cancelled(control->context))
        {
            return Failure(Status::cancelled);
        }
        http_status = 0;
        connection = sceHttpCreateConnectionWithURL(g_http_template, history.Current(), 1);
        if (connection < 0)
            return Failure(Status::request_failed, connection);
        request = sceHttpCreateRequestWithURL(connection, kHttpMethodGet, history.Current(), 0);
        if (request < 0)
        {
            const int error = request;
            CloseRequest(connection, -1);
            return Failure(Status::request_failed, error);
        }
        TrackPlaylistRequest(request);
        if (control && control->cancelled && control->cancelled(control->context))
        {
            CloseRequest(connection, request);
            return Failure(Status::cancelled);
        }
        result = ConfigureRequest(request,
                                  "application/vnd.apple.mpegurl, application/x-mpegURL, "
                                  "audio/mpegurl, text/plain, */*",
                                  kReceiveTimeoutUsec, headers);
        if (result >= 0)
            result = sceHttpSendRequest(request, nullptr, 0);
        if (result >= 0)
            result = sceHttpGetStatusCode(request, &http_status);
        if (result < 0)
        {
            CloseRequest(connection, request);
            return Failure(Status::request_failed, result);
        }
        if (!IsRedirectStatus(http_status))
            break;

        char location[kMaxUrlBytes + 1u] = {};
        const bool followed =
            ExtractLocation(request, location, sizeof(location)) && history.Follow(location);
        CloseRequest(connection, request);
        connection = -1;
        request = -1;
        if (!followed || !CopyText(effective_url, effective_url_capacity, history.Current(),
                                   std::strlen(history.Current())))
        {
            return Failure(Status::redirect_error, 0, http_status);
        }
    }
    if (!CopyText(effective_url, effective_url_capacity, history.Current(),
                  std::strlen(history.Current())))
    {
        CloseRequest(connection, request);
        return Failure(Status::redirect_error, 0, http_status);
    }
    if (http_status < 200 || http_status >= 300)
    {
        const std::size_t error_capacity = buffer_capacity < kMaxErrorResponseBytes + 1u
                                               ? buffer_capacity
                                               : kMaxErrorResponseBytes + 1u;
        const std::size_t bytes = ReadErrorResponse(request, buffer, error_capacity);
        CloseRequest(connection, request);
        return Failure(Status::http_status_error, 0, http_status, bytes);
    }

    std::size_t bytes = 0;
    Status status = Status::ok;
    const std::uint64_t started = MonotonicUsec();
    while (bytes < max_bytes)
    {
        if (control && control->cancelled && control->cancelled(control->context))
        {
            status = Status::cancelled;
            result = 0;
            break;
        }
        const std::uint64_t now = MonotonicUsec();
        if (started != 0 && now >= started && now - started >= kPlaylistDeadlineUsec)
        {
            status = Status::deadline_exceeded;
            result = -1;
            break;
        }
        const std::size_t remaining = max_bytes - bytes;
        const int read = sceHttpReadData(request, buffer + bytes, remaining);
        if (read < 0)
        {
            status = Status::read_failed;
            result = read;
            break;
        }
        if (read == 0)
            break;
        if (static_cast<std::size_t>(read) > remaining)
        {
            status = Status::read_failed;
            result = -1;
            break;
        }
        bytes += static_cast<std::size_t>(read);
    }
    if (status == Status::ok && control && control->cancelled &&
        control->cancelled(control->context))
    {
        status = Status::cancelled;
        result = 0;
    }
    if (status == Status::ok && bytes == max_bytes)
    {
        char extra = '\0';
        const int read = sceHttpReadData(request, &extra, 1);
        if (read < 0)
        {
            status = Status::read_failed;
            result = read;
        }
        else if (read > 0)
        {
            status = Status::response_too_large;
            result = 0;
        }
    }
    buffer[bytes] = '\0';
    CloseRequest(connection, request);
    return {status, bytes, http_status, status == Status::ok ? 0 : result};
}

Status OpenStream(const char *url, const char *accept, StreamRequest *stream,
                  const RequestHeaders *headers)
{
    if (!stream)
        return Status::invalid_argument;
    *stream = {};
    if (!IsSupportedUrl(url))
        return Status::unsupported_url;
    RedirectHistory history;
    if (!history.Begin(url) || !CopyText(stream->effective_url, sizeof(stream->effective_url),
                                         history.Current(), std::strlen(history.Current())))
    {
        return Status::invalid_argument;
    }
    if (g_http_template < 0)
        return Status::not_initialized;

    const char *accepted =
        accept && *accept ? accept
                          : "video/mp2t, application/vnd.apple.mpegurl, application/x-mpegURL, */*";

    for (;;)
    {
        stream->http_status = 0;
        stream->connection = sceHttpCreateConnectionWithURL(g_http_template, history.Current(), 0);
        if (stream->connection < 0)
        {
            stream->native_error = stream->connection;
            return Status::request_failed;
        }
        stream->request =
            sceHttpCreateRequestWithURL(stream->connection, kHttpMethodGet, history.Current(), 0);
        if (stream->request < 0)
        {
            stream->native_error = stream->request;
            CloseStream(stream);
            return Status::request_failed;
        }
        int result =
            ConfigureRequest(stream->request, accepted, kStreamReceiveTimeoutUsec, headers);
        if (result >= 0)
            result = sceHttpSendRequest(stream->request, nullptr, 0);
        if (result >= 0)
            result = sceHttpGetStatusCode(stream->request, &stream->http_status);
        if (result < 0)
        {
            stream->native_error = result;
            CloseStream(stream);
            return Status::request_failed;
        }
        if (!IsRedirectStatus(stream->http_status))
        {
            if (stream->http_status < 200 || stream->http_status >= 300)
            {
                ReadErrorResponse(stream->request, stream->error_response,
                                  sizeof(stream->error_response));
                CloseStream(stream);
                return Status::http_status_error;
            }
            stream->open = true;
            return Status::ok;
        }

        char location[kMaxUrlBytes + 1u] = {};
        const bool followed = ExtractLocation(stream->request, location, sizeof(location)) &&
                              history.Follow(location);
        CloseStream(stream);
        if (!followed || !CopyText(stream->effective_url, sizeof(stream->effective_url),
                                   history.Current(), std::strlen(history.Current())))
        {
            return Status::redirect_error;
        }
    }
}

int ReadStream(StreamRequest *stream, void *buffer, std::size_t bytes)
{
    if (!stream || !stream->open || stream->request < 0 || !buffer || bytes == 0)
        return -1;
    const int result = sceHttpReadData(stream->request, buffer, bytes);
    if (result < 0)
        stream->native_error = result;
    return result;
}

void CloseStream(StreamRequest *stream)
{
    if (!stream)
        return;
    CloseRequest(stream->connection, stream->request);
    stream->connection = -1;
    stream->request = -1;
    stream->open = false;
}

#else

Status NetworkInit()
{
    return Status::platform_unavailable;
}

void NetworkShutdown()
{
}

void CancelActivePlaylistRequest()
{
}

FetchResult GetM3u(const char *url, char *buffer, std::size_t buffer_capacity,
                   std::size_t max_bytes, const RequestHeaders *headers,
                   const RequestControl *control)
{
    char effective_url[kMaxUrlBytes + 1u] = {};
    return GetM3uResolved(url, buffer, buffer_capacity, effective_url, sizeof(effective_url),
                          max_bytes, headers, control);
}

FetchResult GetM3uResolved(const char *url, char *buffer, std::size_t buffer_capacity,
                           char *effective_url, std::size_t effective_url_capacity,
                           std::size_t max_bytes, const RequestHeaders *,
                           const RequestControl *control)
{
    if (effective_url && effective_url_capacity != 0)
        effective_url[0] = '\0';
    if (!IsSupportedUrl(url))
        return {Status::unsupported_url, 0, 0, 0};
    if (control && control->cancelled && control->cancelled(control->context))
        return {Status::cancelled, 0, 0, 0};
    if (buffer == nullptr || max_bytes == 0 || max_bytes > kHardMaxPlaylistBytes ||
        buffer_capacity < max_bytes + 1 || !effective_url || effective_url_capacity == 0)
    {
        return {Status::invalid_argument, 0, 0, 0};
    }
    buffer[0] = '\0';
    const std::size_t url_length = BoundedLength(url, kMaxUrlBytes);
    if (!CopyText(effective_url, effective_url_capacity, url, url_length))
        return {Status::invalid_argument, 0, 0, 0};
    return {Status::platform_unavailable, 0, 0, 0};
}

Status OpenStream(const char *url, const char *, StreamRequest *stream, const RequestHeaders *)
{
    if (!stream)
        return Status::invalid_argument;
    *stream = {};
    if (!IsSupportedUrl(url))
        return Status::unsupported_url;
    const std::size_t length = BoundedLength(url, kMaxUrlBytes);
    if (!CopyText(stream->effective_url, sizeof(stream->effective_url), url, length))
        return Status::invalid_argument;
    return Status::platform_unavailable;
}

int ReadStream(StreamRequest *, void *, std::size_t)
{
    return -1;
}
void CloseStream(StreamRequest *stream)
{
    if (!stream)
        return;
    stream->connection = -1;
    stream->request = -1;
    stream->open = false;
}

#endif

} // namespace iptv::http
