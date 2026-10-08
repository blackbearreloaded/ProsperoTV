// ProsperoTV - Independent TS/HLS connection and native muted preview decoder.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/preview.hpp"
#include "tv/platform.hpp"
#include "tv/diag.hpp"
#include "tv_http.h"
#include "iptv_hls.h"
#include "iptv_stream.h"
#include "update_kit/console_curl.h"
#include <curl/curl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

namespace ptv::platform
{
namespace
{
bool cancelled(const iptv::http::RequestControl &control)
{
    return control.cancelled && control.cancelled(control.context);
}
struct Transfer
{
    const iptv::http::RequestControl &control;
    const std::function<bool(const char *, std::size_t)> &sink;
    long status = 0;
    std::size_t headers = 0;
};
std::size_t header(char *bytes, std::size_t size, std::size_t count, void *self)
{
    auto &t = *static_cast<Transfer *>(self);
    const auto length = size * count;
    if (length > 65536 - t.headers)
        return 0;
    t.headers += length;
    if (length >= 12 && std::memcmp(bytes, "HTTP/", 5) == 0)
    {
        // A callback line is not NUL-terminated.
        char line[64]{};
        std::memcpy(line, bytes, std::min(length, sizeof(line) - 1));
        if (std::sscanf(line, "HTTP/%*s %ld", &t.status) != 1)
            return 0;
    }
    return length;
}
std::size_t receive(char *bytes, std::size_t size, std::size_t count, void *self)
{
    auto &t = *static_cast<Transfer *>(self);
    const auto length = size * count;
    if (cancelled(t.control))
        return 0;
    if (t.status != 200)
        return length; // Redirect and error pages are never passed to the parser.
    return t.sink(bytes, length) ? length : 0;
}
int progress(void *self, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return cancelled(static_cast<Transfer *>(self)->control) ? 1 : 0;
}
bool download(const std::string &original, const iptv::http::RequestHeaders &headers,
              const iptv::http::RequestControl &control,
              const std::function<bool(const char *, std::size_t)> &sink, std::string *effective,
              long timeout_ms = 0)
{
    if (tv_http_init(0, 0, 0) < 0)
        return false;
    std::string url = original;
    for (unsigned redirects = 0; redirects <= iptv::http::kMaxRedirects; ++redirects)
    {
        if (cancelled(control) || !iptv::http::IsSupportedPlaylistUrl(url.c_str()))
            return false;
        auto *curl = curl_easy_init();
        if (!curl)
            return false;
        Transfer transfer{control, sink};
        const auto scoped = iptv::http::HeadersForUrl(original.c_str(), url.c_str(), headers);
        curl_slist *fields = nullptr;
        for (const auto &[name, value] :
             {std::pair{"Cookie", scoped.cookie}, std::pair{"Authorization", scoped.authorization}})
            if (value && *value)
                fields = curl_slist_append(fields, (std::string(name) + ": " + value).c_str());
        console_curl_setup(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         scoped.user_agent && *scoped.user_agent ? scoped.user_agent
                                                                 : "ProsperoTV");
        if (scoped.referrer && *scoped.referrer)
            curl_easy_setopt(curl, CURLOPT_REFERER, scoped.referrer);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, fields);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
        *effective = url;
        const auto result = curl_easy_perform(curl);
        char *redirect = nullptr;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
        const std::string next = redirect ? redirect : "";
        curl_easy_cleanup(curl);
        curl_slist_free_all(fields);
        if (result != CURLE_OK || cancelled(control))
            return false;
        if (transfer.status == 200)
            return true;
        if (transfer.status < 300 || transfer.status >= 400 || next.empty())
            return false;
        url = next;
    }
    return false;
}
iptv::http::FetchResult portal_fetch(const char *url, char *buffer, std::size_t capacity,
                                     std::size_t limit, const iptv::http::RequestControl *control,
                                     const iptv::http::RequestHeaders *headers)
{
    iptv::http::FetchResult result;
    if (!buffer || capacity <= limit || !control || !headers)
        return result;
    std::string effective;
    const bool ok = download(
        url, *headers, *control,
        [&](const char *bytes, std::size_t count)
        {
            if (count > limit - result.bytes)
                return false;
            std::memcpy(buffer + result.bytes, bytes, count);
            result.bytes += count;
            return true;
        },
        &effective, 8000);
    buffer[result.bytes] = 0;
    result.status = ok                    ? iptv::http::Status::ok
                    : cancelled(*control) ? iptv::http::Status::cancelled
                                          : iptv::http::Status::request_failed;
    return result;
}
struct Decoder
{
    iptv_native_backend_t native{};
    iptv_stream_session_t stream{};
    const iptv::http::RequestControl &control;
    void (*picture)(void *, const iptv_native_picture_t *);
    void *context;
    bool opened = false;
    Decoder(const iptv::http::RequestControl &c, void (*p)(void *, const iptv_native_picture_t *),
            void *ctx)
        : control(c), picture(p), context(ctx)
    {
        iptv_native_backend_init(&native);
        iptv_stream_init(&stream);
        iptv_stream_backend_t backend{};
        backend.context = this;
        backend.open = [](void *self, const iptv_stream_format_t *f)
        {
            auto &d = *static_cast<Decoder *>(self);
            if (cancelled(d.control) || (f->video_codec != IPTV_STREAM_VIDEO_H264 &&
                                         f->video_codec != IPTV_STREAM_VIDEO_HEVC))
                return -1;
            iptv_native_open_config_t config{};
            config.codec = f->video_codec == IPTV_STREAM_VIDEO_H264 ? IPTV_NATIVE_CODEC_H264
                                                                    : IPTV_NATIVE_CODEC_HEVC;
            config.profile = f->video_profile;
            config.level = f->video_level;
            config.coded_width = f->coded_width;
            config.coded_height = f->coded_height;
            config.visible_width = f->visible_width;
            config.visible_height = f->visible_height;
            config.bit_depth = f->video_bit_depth;
            config.chroma_format = IPTV_NATIVE_CHROMA_420;
            config.picture_context = self;
            config.picture_cancelled = [](void *self)
            { return cancelled(static_cast<Decoder *>(self)->control) ? 1 : 0; };
            config.picture = [](void *self, const iptv_native_picture_t *picture)
            {
                auto &d = *static_cast<Decoder *>(self);
                if (!cancelled(d.control))
                    d.picture(d.context, picture);
            };
            const auto result = iptv_native_backend_open(&d.native, &config);
            d.opened = result == 0;
            return static_cast<int>(result);
        };
        backend.submit_video =
            [](void *self, const std::uint8_t *bytes, std::size_t count, std::uint64_t pts)
        {
            auto &d = *static_cast<Decoder *>(self);
            return cancelled(d.control) ? -1
                                        : static_cast<int>(iptv_native_backend_submit_video(
                                              &d.native, bytes, count, pts));
        };
        backend.submit_audio = [](void *, const std::uint8_t *, std::size_t, std::uint64_t)
        { return 0; };
        backend.disable_audio = [](void *) { return 0; };
        backend.discontinuity = [](void *self)
        {
            return static_cast<int>(
                iptv_native_backend_discontinuity(&static_cast<Decoder *>(self)->native));
        };
        backend.drain = [](void *) { return 0; };
        backend.close = [](void *) {};
        if (iptv_stream_open(&stream, nullptr, &backend) == 0)
            iptv_stream_start(&stream);
    }
    ~Decoder()
    {
        if (opened)
        {
            iptv_native_backend_request_stop(&native);
            const auto result = iptv_native_backend_close(&native);
            iptv_native_telemetry_t telemetry{};
            iptv_native_backend_get_telemetry(&native, &telemetry);
            diag::event("preview decoded=%llu delivered=%llu audio=%llu cleanup=%d",
                        static_cast<unsigned long long>(telemetry.decoded_frames),
                        static_cast<unsigned long long>(telemetry.presented_frames),
                        static_cast<unsigned long long>(telemetry.decoded_audio_frames), result);
        }
        iptv_stream_cleanup(&stream);
    }
    bool push(const char *bytes, std::size_t count)
    {
        return !cancelled(control) && iptv_stream_push(&stream, bytes, count) == 0;
    }
};

void watch(std::string url, const PlayRequest &request, const iptv::http::RequestControl &control,
           void (*picture)(void *, const iptv_native_picture_t *), void *context)
{
    auto decoder = std::make_unique<Decoder>(control, picture, context);
    iptv::http::RequestHeaders headers{request.user_agent.c_str(), request.referrer.c_str()};
    headers.authorization = request.authorization.c_str();
    headers.credential_origin =
        request.credential_origin.empty() ? nullptr : request.credential_origin.c_str();
    bool first = true, have_sequence = false;
    std::uint64_t next_sequence = 0;
    unsigned masters = 0;
    while (!cancelled(control))
    {
        std::string probe, effective;
        bool transport = false;
        const bool ok = download(
            url, headers, control,
            [&](const char *bytes, std::size_t count)
            {
                if (transport)
                    return decoder->push(bytes, count);
                if (count > IPTV_HLS_DEFAULT_MAX_INPUT_BYTES - probe.size())
                    return false;
                probe.append(bytes, count);
                if (first && probe.size() >= 8 && probe.compare(0, 7, "#EXTM3U") != 0)
                {
                    transport = true;
                    const bool accepted = decoder->push(probe.data(), probe.size());
                    probe.clear();
                    return accepted;
                }
                return true;
            },
            &effective);
        if (transport && ok && decoder->opened)
            iptv_native_backend_drain(&decoder->native);
        if (!ok || transport)
            return;
        first = false;
        auto playlist = std::make_unique<iptv_hls_playlist_t>();
        iptv_hls_limits_t limits{};
        iptv_hls_default_limits(&limits);
        // A small rendition is sufficient for the television on the menu.
        limits.max_width = 1280;
        limits.max_height = 720;
        auto parsed = iptv_hls_parse(probe.data(), probe.size(), effective.c_str(),
                                     effective.size(), &limits, playlist.get());
        if (parsed == IPTV_HLS_NO_VARIANT_WITHIN_LIMITS)
        {
            iptv_hls_default_limits(&limits);
            parsed = iptv_hls_parse(probe.data(), probe.size(), effective.c_str(), effective.size(),
                                    &limits, playlist.get());
        }
        if (parsed != IPTV_HLS_OK)
            return;
        if (playlist->kind == IPTV_HLS_KIND_MASTER)
        {
            if (++masters > 4 || playlist->selected_variant >= playlist->variant_count)
                return;
            url = playlist->variants[playlist->selected_variant].url;
            continue;
        }
        if (playlist->kind != IPTV_HLS_KIND_MEDIA || !playlist->segment_count)
            return;
        if (!have_sequence)
        {
            const auto start =
                playlist->is_live && playlist->segment_count > 2 ? playlist->segment_count - 2 : 0;
            next_sequence = playlist->segments[start].sequence;
            have_sequence = true;
        }
        for (unsigned i = 0; i < playlist->segment_count && !cancelled(control); ++i)
        {
            const auto &segment = playlist->segments[i];
            if (segment.sequence < next_sequence)
                continue;
            if ((segment.discontinuity || segment.sequence > next_sequence) && decoder->opened)
                iptv_stream_discontinuity(&decoder->stream);
            std::string location;
            if (!download(
                    segment.url, headers, control, [&](const char *bytes, std::size_t count)
                    { return decoder->push(bytes, count); }, &location))
                return;
            next_sequence = segment.sequence + 1;
        }
        if (!playlist->is_live)
        {
            if (decoder->opened)
                iptv_native_backend_drain(&decoder->native);
            return;
        }
        const unsigned wait = std::clamp(playlist->target_duration_ms / 2, 250u, 2000u);
        for (unsigned slept = 0; slept < wait && !cancelled(control); slept += 20)
            sleep_ms(20);
        url = effective;
    }
}
} // namespace

void preview(const PlayRequest &request, const iptv::http::RequestControl &control,
             void (*picture)(void *, const iptv_native_picture_t *), void *context)
{
    auto urls = request.urls;
    if (!request.portal_command.empty())
    {
        PortalClient portal(request.portal, &control, portal_fetch);
        std::string url;
        if (!portal.sign_in() || !portal.resolve(request.portal_command, &url))
            return;
        urls = {std::move(url)};
    }
    diag::event("preview start");
    for (const auto &url : urls)
    {
        if (cancelled(control))
            break;
        watch(url, request, control, picture, context);
    }
    diag::event("preview stopped");
}
} // namespace ptv::platform
