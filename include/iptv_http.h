/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_HTTP_H
#define IPTV_HTTP_H

#include <memory>
#include <new>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace iptv::http
{

inline constexpr std::size_t kDefaultMaxPlaylistBytes = 8u * 1024u * 1024u;
// A provider with tens of thousands of channels answers with tens of megabytes.
inline constexpr std::size_t kHardMaxPlaylistBytes = 64u * 1024u * 1024u;
// A list read as it arrives (GetList) is held nowhere, so it may be far larger:
// a quarter of a million channels are 70 to 150 MiB of playlist or account answer.
inline constexpr std::size_t kMaxListBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t kListPieceBytes = 64u * 1024u;
inline constexpr std::size_t kMaxUrlBytes = 4096u;
inline constexpr std::size_t kMaxRedirects = 5u;
inline constexpr std::size_t kMaxErrorResponseBytes = 511u;

// Video requests need more receive headroom than the original radio-sized pools.
inline constexpr std::size_t kNetPoolSize = 1024u * 1024u;
inline constexpr std::size_t kSslPoolSize = 304u * 1024u;
inline constexpr std::size_t kHttpPoolSize = 4u * 1024u * 1024u;
inline constexpr std::uint32_t kResolveTimeoutUsec = 5000000u;
inline constexpr std::uint32_t kConnectTimeoutUsec = 5000000u;
inline constexpr std::uint32_t kSendTimeoutUsec = 5000000u;
inline constexpr std::uint32_t kReceiveTimeoutUsec = 5000000u;
// The whole download; a stalled one ends sooner, at the receive timeout. Ten
// minutes: the largest lists are over a hundred megabytes from a slow panel.
inline constexpr std::uint64_t kPlaylistDeadlineUsec = 600000000u;
inline constexpr bool kAutoRedirect = false;

enum class Status : std::uint8_t
{
    ok = 0,
    invalid_argument,
    unsupported_url,
    not_initialized,
    platform_unavailable,
    network_init_failed,
    request_failed,
    http_status_error,
    response_too_large,
    read_failed,
    deadline_exceeded,
    redirect_error,
    cancelled,
    stopped, // the receiver of a list had enough (GetList)
};

struct FetchResult
{
    Status status = Status::invalid_argument;
    std::size_t bytes = 0;
    int http_status = 0;
    int native_error = 0;
};

struct StreamRequest
{
    int connection = -1;
    int request = -1;
    int http_status = 0;
    int native_error = 0;
    bool open = false;
    std::int64_t size = -1; // complete resource length, when supplied by the server
    char effective_url[kMaxUrlBytes + 1u] = {};
    char error_response[kMaxErrorResponseBytes + 1u] = {};
};

struct RequestHeaders
{
    const char *user_agent = nullptr;
    const char *referrer = nullptr;
    const char *cookie = nullptr;
    const char *authorization = nullptr;
    std::int64_t byte_offset = -1; // -1: ordinary GET; otherwise request bytes=<offset>-
    // When provided, credentials stay on this origin even when a playlist
    // points its segments or streams at another host.
    const char *credential_origin = nullptr;
};

// Validate a ranged response before the demuxer interprets its first byte.
bool ParseStreamRange(std::string_view headers, int status, std::int64_t offset,
                      std::int64_t *size);

// Portal credentials belong only to the origin that was asked. Redirects
// may still reach a CDN, but must not forward a MAC cookie or bearer token.
RequestHeaders HeadersForUrl(const char *original, const char *target,
                             const RequestHeaders &headers);

struct RequestControl
{
    bool (*cancelled)(void *context) = nullptr;
    void *context = nullptr;
};

// Input policy only; it performs no network access.
bool IsSupportedPlaylistUrl(const char *url);

// Resolves an HTTP redirect Location against base_url and normalizes dot
// segments. This helper performs no network access.
Status ResolveRedirectUrl(const char *base_url, const char *location, char *resolved_url,
                          std::size_t resolved_capacity, const char *const *visited_urls = nullptr,
                          std::size_t visited_count = 0);

// A provider is reported as geographically blocked only when its response
// says so, or when it uses the standard HTTP 451 status.
bool ResponseIndicatesGeographicBlock(const char *response, std::size_t bytes);
void DescribeFailure(Status status, int http_status, int native_error, const char *response,
                     char *description, std::size_t description_capacity);

Status NetworkInit();
void NetworkShutdown();

// Storage for one channel list answer: room for kHardMaxPlaylistBytes when the
// memory is there, otherwise half of it, and so on down to `least`. The bytes
// are not cleared, so a short answer touches little of it. max_bytes is what
// to pass to GetM3u; it is 0 when not even `least` could be had.
struct ListBuffer
{
    std::unique_ptr<char[]> bytes;
    std::size_t max_bytes = 0;

    char *data()
    {
        return bytes.get();
    }
    std::size_t size() const
    {
        return bytes ? max_bytes + 1u : 0u;
    }
};
inline ListBuffer AllocateListBuffer(std::size_t wanted = kHardMaxPlaylistBytes,
                                     std::size_t least = 1024u * 1024u)
{
    ListBuffer buffer;
    for (std::size_t room = wanted < least ? least : wanted;;
         room = room / 2u < least ? least : room / 2u)
    {
        buffer.bytes.reset(new (std::nothrow) char[room + 1u]);
        if (buffer.bytes)
        {
            buffer.bytes[0] = '\0';
            buffer.max_bytes = room;
            return buffer;
        }
        if (room == least)
            return buffer;
    }
}
void CancelActivePlaylistRequest();

// Writes at most max_bytes into buffer and always NUL-terminates it on return.
// buffer_capacity must be at least max_bytes + 1. No storage is allocated here.
FetchResult GetM3u(const char *url, char *buffer, std::size_t buffer_capacity,
                   std::size_t max_bytes = kDefaultMaxPlaylistBytes,
                   const RequestHeaders *headers = nullptr,
                   const RequestControl *control = nullptr);

// As above, and writes the final requested URL (including explicit redirects).
// effective_url is always NUL-terminated when its capacity is nonzero.
FetchResult GetM3uResolved(const char *url, char *buffer, std::size_t buffer_capacity,
                           char *effective_url, std::size_t effective_url_capacity,
                           std::size_t max_bytes = kDefaultMaxPlaylistBytes,
                           const RequestHeaders *headers = nullptr,
                           const RequestControl *control = nullptr);

// Takes a list as it arrives: every piece, in order, as soon as it is read.
// Returning false ends the download there with Status::stopped.
struct ListSink
{
    bool (*write)(void *context, const char *data, std::size_t bytes) = nullptr;
    void *context = nullptr;
};

// As GetM3u, but the answer is handed to `sink` piece by piece and kept
// nowhere, so its size is bounded by max_bytes alone (kMaxListBytes at most).
// The result's bytes are how many the sink was given.
FetchResult GetList(const char *url, const ListSink &sink, std::size_t max_bytes = kMaxListBytes,
                    const RequestHeaders *headers = nullptr,
                    const RequestControl *control = nullptr);

// Opens a bounded-time streaming response. NetworkInit must have succeeded.
Status OpenStream(const char *url, const char *accept, StreamRequest *stream,
                  const RequestHeaders *headers = nullptr);
int ReadStream(StreamRequest *stream, void *buffer, std::size_t bytes);
void CloseStream(StreamRequest *stream);

} // namespace iptv::http

#endif
