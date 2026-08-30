/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_player.h"

#include "iptv_hls.h"
#include "iptv_http.h"
#include "iptv_input.h"
#include "iptv_native_agc_present.h"
#include "iptv_native_backend.h"
#include "iptv_stream.h"
#include "iptv_webm.h"

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <time.h>

extern "C" int sceKernelSendNotificationRequest(std::uint32_t device, void *request,
                                                std::size_t size, int blocking);
extern "C" int sceKernelUsleep(std::uint32_t microseconds);
extern "C" int scePthreadCreate(void **thread, const void *attributes, void *(*entry)(void *),
                                void *argument, const char *name);
extern "C" int scePthreadJoin(void *thread, void **result);
extern "C" long write(int descriptor, const void *buffer, std::size_t bytes);

namespace
{

constexpr std::size_t kReadBytes = 64u * 1024u;
constexpr std::size_t kReadAheadBytes = 16u * 1024u * 1024u;
constexpr std::size_t kReadAheadMinimumBytes = 64u * 1024u;
constexpr std::size_t kReadAheadHighWaterBytes = 12u * 1024u * 1024u;
constexpr std::uint64_t kReadAheadInitialUsec = UINT64_C(2000000);
constexpr std::uint64_t kReadAheadRebufferUsec = UINT64_C(1000000);
constexpr std::uint64_t kReadAheadRebufferStepUsec = UINT64_C(500000);
constexpr std::uint64_t kReadAheadMaximumUsec = UINT64_C(3000000);
constexpr std::uint64_t kHlsMinimumInitialUsec = UINT64_C(6000000);
constexpr std::uint64_t kHlsMaximumInitialUsec = UINT64_C(15000000);
constexpr std::uint64_t kHlsMinimumRebufferUsec = UINT64_C(4000000);
constexpr std::uint64_t kHlsMaximumRebufferUsec = UINT64_C(8000000);
constexpr std::size_t kPlaylistBytes = IPTV_HLS_DEFAULT_MAX_INPUT_BYTES;
constexpr std::uint64_t kVideoProgressTimeoutUsec = UINT64_C(15000000);
constexpr char kReceiptPath[] = "/download0/iptv-last-receipt.txt";

constexpr std::uint64_t ClampUsec(std::uint64_t value, std::uint64_t minimum, std::uint64_t maximum)
{
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

constexpr std::uint64_t HlsInitialUsec(std::uint32_t target_duration_ms)
{
    return ClampUsec(static_cast<std::uint64_t>(target_duration_ms) * 2000u, kHlsMinimumInitialUsec,
                     kHlsMaximumInitialUsec);
}

constexpr std::uint64_t HlsRebufferUsec(std::uint32_t target_duration_ms)
{
    return ClampUsec(static_cast<std::uint64_t>(target_duration_ms) * 1000u,
                     kHlsMinimumRebufferUsec, kHlsMaximumRebufferUsec);
}

constexpr std::uint64_t ReadAheadPrimeUsec(unsigned underruns, std::uint64_t initial_us,
                                           std::uint64_t rebuffer_us, std::uint64_t maximum_us)
{
    const std::uint64_t requested =
        underruns == 0 ? initial_us : rebuffer_us + underruns * kReadAheadRebufferStepUsec;
    return requested < maximum_us ? requested : maximum_us;
}

constexpr bool ReadAheadPrimed(std::size_t available, std::uint64_t elapsed_us,
                               std::uint64_t required_us)
{
    return available >= kReadAheadHighWaterBytes ||
           (available >= kReadAheadMinimumBytes && elapsed_us >= required_us);
}

static_assert(HlsInitialUsec(5000) == UINT64_C(10000000));
static_assert(HlsInitialUsec(6000) == UINT64_C(12000000));
static_assert(HlsRebufferUsec(5000) == UINT64_C(5000000));
static_assert(ReadAheadPrimeUsec(0, kReadAheadInitialUsec, kReadAheadRebufferUsec,
                                 kReadAheadMaximumUsec) == UINT64_C(2000000));
static_assert(ReadAheadPrimeUsec(1, kReadAheadInitialUsec, kReadAheadRebufferUsec,
                                 kReadAheadMaximumUsec) == UINT64_C(1500000));
static_assert(!ReadAheadPrimed(kReadAheadMinimumBytes, UINT64_C(1000000), kReadAheadInitialUsec));
static_assert(ReadAheadPrimed(kReadAheadMinimumBytes, UINT64_C(2000000), kReadAheadInitialUsec));
char gLastPlaybackError[192]{};

void SetLastPlaybackError(const char *format, ...)
{
    va_list arguments;

    if (!format)
    {
        gLastPlaybackError[0] = '\0';
        return;
    }
    va_start(arguments, format);
    std::vsnprintf(gLastPlaybackError, sizeof(gLastPlaybackError), format, arguments);
    va_end(arguments);
}

void SetRequestFailure(const char *context, iptv::http::Status status, int http_status,
                       int native_error, const char *response)
{
    char reason[160]{};
    iptv::http::DescribeFailure(status, http_status, native_error, response, reason,
                                sizeof(reason));
    SetLastPlaybackError("%s: %s.", context, reason);
}

std::uint64_t MonotonicUsec()
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * UINT64_C(1000000) +
           static_cast<std::uint64_t>(now.tv_nsec) / UINT64_C(1000);
}

struct NotificationRequest
{
    std::uint8_t reserved[45];
    char message[3075];
};

void WriteStdout(const char *text, std::size_t bytes)
{
    while (text && bytes)
    {
        const long written = write(1, text, bytes);
        if (written <= 0)
            return;
        text += static_cast<std::size_t>(written);
        bytes -= static_cast<std::size_t>(written);
    }
}

void Notify(const char *message)
{
    NotificationRequest request{};
    if (message)
    {
        std::snprintf(request.message, sizeof(request.message), "%s", message);
    }
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

void NotifyLastPlaybackError()
{
    char message[192]{};
    std::snprintf(message, sizeof(message), "IPTV: %.180s",
                  gLastPlaybackError[0] ? gLastPlaybackError : "The channel could not be played.");
    Notify(message);
}

void SaveReceipt(int result, const iptv_stream_telemetry_t &stream,
                 const iptv_native_telemetry_t &native, std::uint32_t native_telemetry_available,
                 std::uint32_t playback_stop_requested, std::uint64_t player_cleanup_count,
                 int player_cleanup_result)
{
    char temporary[96]{};
    std::snprintf(temporary, sizeof(temporary), "%s.tmp", kReceiptPath);
    std::FILE *file = std::fopen(temporary, "wb");
    if (!file)
        return;
    std::fprintf(
        file,
        "IPTV_RECEIPT_V2\n"
        "result=%d\n"
        "playback_error=%s\n"
        "stream_state=%d\n"
        "stream_result=%d\n"
        "stream_error=%s\n"
        "stream_audio_disabled=%u\n"
        "stream_audio_warning=%s\n"
        "codec=%u\nprofile=%u\nlevel=%u\n"
        "coded=%ux%u\nvisible=%ux%u\nbit_depth=%u\nchroma=%u\n"
        "video_access_units=%llu\naudio_frames=%llu\n"
        "native_state=%d\nnative_result=%d\nnative_cleanup=%d\n"
        "decoder_surface_pitch=%u\ndecoder_surface_height=%u\n"
        "native_telemetry_available=%u\n"
        "stream_cleanup_count=%llu\nstream_stop_count=%llu\nstream_cleanup_result=%d\n"
        "player_cleanup_count=%llu\nplayer_cleanup_result=%d\n"
        "decoded_frames=%llu\npresented_frames=%llu\nhidden_decoded_frames=%llu\n"
        "last_video_access_unit_bytes=%llu\nlast_video_nal_mask=0x%08x\n"
        "decoder_output_reject_flags=0x%08x\n"
        "decoder_output_valid=%u\ndecoder_output_error=%u\n"
        "decoder_output_picture_count=%u\ndecoder_output_codec=%u\n"
        "decoder_output_width=%u\ndecoder_output_height=%u\ndecoder_output_pitch=%u\n"
        "decoder_frame_accepted=%u\n"
        "buffered_video_access_units=%llu\ndrained_video_frames=%llu\n"
        "dropped_delayed_frames=%llu\ndecoder_flushes=%llu\n"
        "drain_flush_limit_hits=%llu\npending_video_timestamps=%u\n"
        "audio_decoded_frames=%llu\naudio_output_grains=%llu\n"
        "audio_output_total_us=%llu\naudio_output_max_us=%llu\n"
        "audio_queue_max_frames=%u\naudio_queue_underruns=%llu\n"
        "video_queue_max_frames=%u\nvideo_queue_max_bytes=%llu\n"
        "video_queue_underruns=%llu\nactual_frame_rate_x100=%u\nbitrate_kbps=%u\n"
        "native_audio_disabled=%u\nnative_audio_result=%d\n"
        "decoder_output_in_frame_pool=%u\nzero_copy_pointer_match=%u\n"
        "last_decoder_output=0x%llx\nlast_present_source=0x%llx\n"
        "first_frame_latency_us=%llu\ndecode_max_us=%llu\n"
        "present_max_us=%llu\npacing_resets=%llu\n"
        "pacing_waits=%llu\npacing_late_frames=%llu\n"
        "dropped_late_video_frames=%llu\n"
        "hardware_validated=%u\nstream_acceptance_validated=%u\n"
        "playback_stop_requested=%u\n",
        result, gLastPlaybackError, static_cast<int>(stream.state), stream.last_result,
        stream.last_error, stream.audio_disabled, stream.audio_warning, stream.format.video_codec,
        stream.format.video_profile, stream.format.video_level, stream.format.coded_width,
        stream.format.coded_height, stream.format.visible_width, stream.format.visible_height,
        stream.format.video_bit_depth, stream.format.video_chroma_format,
        static_cast<unsigned long long>(stream.video_access_units),
        static_cast<unsigned long long>(stream.audio_frames), static_cast<int>(native.state),
        native.last_result, native.cleanup_result, native.output_pitch,
        native.output_surface_height, native_telemetry_available,
        static_cast<unsigned long long>(stream.cleanup_count),
        static_cast<unsigned long long>(stream.stop_count), stream.last_cleanup_result,
        static_cast<unsigned long long>(player_cleanup_count), player_cleanup_result,
        static_cast<unsigned long long>(native.decoded_frames),
        static_cast<unsigned long long>(native.presented_frames),
        static_cast<unsigned long long>(native.hidden_decoded_frames),
        static_cast<unsigned long long>(native.last_video_access_unit_bytes),
        native.last_video_nal_mask, native.decoder_output_reject_flags, native.decoder_output_valid,
        native.decoder_output_error, native.decoder_output_picture_count,
        native.decoder_output_codec, native.decoder_output_width, native.decoder_output_height,
        native.decoder_output_pitch, native.decoder_frame_accepted,
        static_cast<unsigned long long>(native.buffered_video_access_units),
        static_cast<unsigned long long>(native.drained_video_frames),
        static_cast<unsigned long long>(native.dropped_delayed_frames),
        static_cast<unsigned long long>(native.decoder_flushes),
        static_cast<unsigned long long>(native.drain_flush_limit_hits),
        native.pending_video_timestamps,
        static_cast<unsigned long long>(native.decoded_audio_frames),
        static_cast<unsigned long long>(native.audio_output_grains),
        static_cast<unsigned long long>(native.audio_output_total_us),
        static_cast<unsigned long long>(native.audio_output_max_us), native.audio_queue_max_frames,
        static_cast<unsigned long long>(native.audio_queue_underruns),
        native.video_queue_max_frames,
        static_cast<unsigned long long>(native.video_queue_max_bytes),
        static_cast<unsigned long long>(native.video_queue_underruns),
        native.actual_frame_rate_x100, native.bitrate_kbps, native.audio_disabled,
        native.last_audio_result, native.decoder_output_in_frame_pool,
        native.zero_copy_pointer_match, static_cast<unsigned long long>(native.last_decoder_output),
        static_cast<unsigned long long>(native.last_present_source),
        static_cast<unsigned long long>(native.first_frame_latency_us),
        static_cast<unsigned long long>(native.decode_max_us),
        static_cast<unsigned long long>(native.present_max_us),
        static_cast<unsigned long long>(native.pacing_resets),
        static_cast<unsigned long long>(native.pacing_waits),
        static_cast<unsigned long long>(native.pacing_late_frames),
        static_cast<unsigned long long>(native.dropped_late_video_frames),
        native.hardware_validated, native.stream_acceptance_validated, playback_stop_requested);
    const int write_result = std::ferror(file) ? -1 : std::fflush(file);
    const int close_result = std::fclose(file);
    if (write_result != 0 || close_result != 0)
    {
        std::remove(temporary);
        return;
    }
    std::remove(kReceiptPath);
    if (std::rename(temporary, kReceiptPath) != 0)
    {
        std::remove(temporary);
        return;
    }
    char summary[384]{};
    const int summary_bytes = std::snprintf(
        summary, sizeof(summary),
        "IPTV_RECEIPT result=%d codec=%u decoded=%llu presented=%llu "
        "audio=%llu buffered=%llu drained=%llu flushes=%llu "
        "pool=%u zero_copy=%u cleanup=%d\n",
        result, stream.format.video_codec, static_cast<unsigned long long>(native.decoded_frames),
        static_cast<unsigned long long>(native.presented_frames),
        static_cast<unsigned long long>(native.decoded_audio_frames),
        static_cast<unsigned long long>(native.buffered_video_access_units),
        static_cast<unsigned long long>(native.drained_video_frames),
        static_cast<unsigned long long>(native.decoder_flushes),
        native.decoder_output_in_frame_pool, native.zero_copy_pointer_match, native.cleanup_result);
    if (summary_bytes > 0)
        WriteStdout(summary, static_cast<std::size_t>(summary_bytes) < sizeof(summary)
                                 ? static_cast<std::size_t>(summary_bytes)
                                 : sizeof(summary) - 1u);
}

bool UrlLooksLikeHls(const char *url)
{
    if (!url)
        return false;
    for (const char *at = url; *at && *at != '?' && *at != '#'; ++at)
    {
        if (at[0] != '.')
            continue;
        if (!at[1] || !at[2] || !at[3] || !at[4])
            continue;
        const char m = static_cast<char>(at[1] | 0x20);
        const char u = static_cast<char>(at[3] | 0x20);
        if (m == 'm' && at[2] == '3' && u == 'u' && at[4] == '8')
            return true;
    }
    return false;
}

bool UrlLooksLikeWebm(const char *url)
{
    if (!url)
        return false;
    const char *end = url;
    while (*end && *end != '?' && *end != '#')
        ++end;
    if (end - url < 5)
        return false;
    const char *extension = end - 5;
    return extension[0] == '.' && static_cast<char>(extension[1] | 0x20) == 'w' &&
           static_cast<char>(extension[2] | 0x20) == 'e' &&
           static_cast<char>(extension[3] | 0x20) == 'b' &&
           static_cast<char>(extension[4] | 0x20) == 'm';
}

bool BufferLooksLikeHls(const std::uint8_t *data, std::size_t bytes)
{
    if (!data)
        return false;
    std::size_t at = 0;
    if (bytes >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
        at = 3;
    while (at < bytes &&
           (data[at] == ' ' || data[at] == '\t' || data[at] == '\r' || data[at] == '\n'))
    {
        ++at;
    }
    static constexpr char kHeader[] = "#EXTM3U";
    return bytes - at >= sizeof(kHeader) - 1u &&
           std::memcmp(data + at, kHeader, sizeof(kHeader) - 1u) == 0;
}

bool BufferLooksLikeWebm(const std::uint8_t *data, std::size_t bytes)
{
    static constexpr std::uint8_t kEbml[] = {0x1a, 0x45, 0xdf, 0xa3};
    return data && bytes >= sizeof(kEbml) && std::memcmp(data, kEbml, sizeof(kEbml)) == 0;
}

int ReadInitialProbe(iptv::http::StreamRequest *request, std::uint8_t *buffer, std::size_t capacity)
{
    if (!request || !buffer || capacity < IPTV_STREAM_TS_PACKET_BYTES)
        return -1;
    std::size_t total = 0;
    while (total < IPTV_STREAM_TS_PACKET_BYTES)
    {
        const int read =
            iptv::http::ReadStream(request, buffer + total, IPTV_STREAM_TS_PACKET_BYTES - total);
        if (read < 0)
            return -1;
        if (read == 0)
            break;
        total += static_cast<std::size_t>(read);
        if (BufferLooksLikeHls(buffer, total))
            break;
    }
    return static_cast<int>(total);
}

struct HlsVariantCandidates
{
    std::uint32_t count = 0;
    char urls[IPTV_HLS_MAX_VARIANTS][IPTV_HLS_URL_BYTES]{};
};

void BuildNativeCandidates(iptv_hls_playlist_t *master, const iptv_hls_limits_t *limits,
                           HlsVariantCandidates *candidates)
{
    if (!master || !limits || !candidates)
        return;

    std::uint32_t excluded = 0;
    while (candidates->count < master->variant_count)
    {
        const std::uint32_t selected = iptv_hls_select_variant(master, limits, excluded);
        if (selected == IPTV_HLS_NO_VARIANT)
            break;
        std::snprintf(candidates->urls[candidates->count], IPTV_HLS_URL_BYTES, "%s",
                      master->variants[selected].url);
        ++candidates->count;
        excluded |= UINT32_C(1) << selected;
    }
}

struct NativeAdapter
{
    iptv_native_backend_t backend{};
    bool initialized = false;
    bool opened = false;
};

int AdapterOpen(void *context, const iptv_stream_format_t *format)
{
    auto *adapter = static_cast<NativeAdapter *>(context);
    if (!adapter || !format || !adapter->initialized)
        return -1;

    iptv_native_open_config_t config{};
    if (format->video_codec == IPTV_STREAM_VIDEO_H264)
    {
        config.codec = IPTV_NATIVE_CODEC_H264;
    }
    else if (format->video_codec == IPTV_STREAM_VIDEO_HEVC)
    {
        config.codec = IPTV_NATIVE_CODEC_HEVC_MAIN8;
    }
    else if (format->video_codec == IPTV_STREAM_VIDEO_VP9)
    {
        config.codec = IPTV_NATIVE_CODEC_VP9_PROFILE0;
    }
    else
    {
        return -1;
    }
    config.profile = format->video_profile;
    config.level = format->video_level;
    config.coded_width = format->coded_width;
    config.coded_height = format->coded_height;
    config.visible_width = format->visible_width;
    config.visible_height = format->visible_height;
    config.bit_depth = format->video_bit_depth;
    config.chroma_format = IPTV_NATIVE_CHROMA_420;
    config.hdr = 0;
    config.enable_audio = format->audio_pid != 0;
    iptv_native_agc_loading_stop();
    const int handoff = iptv_native_agc_present_shutdown();
    if (handoff != 0)
        return handoff;
    const int result = iptv_native_backend_open(&adapter->backend, &config);
    adapter->opened = result == 0;
    return result;
}

int AdapterVideo(void *context, const std::uint8_t *data, std::size_t bytes, std::uint64_t pts_us)
{
    auto *adapter = static_cast<NativeAdapter *>(context);
    return adapter && adapter->opened
               ? iptv_native_backend_submit_video(&adapter->backend, data, bytes, pts_us)
               : -1;
}

int AdapterAudio(void *context, const std::uint8_t *data, std::size_t bytes, std::uint64_t pts_us)
{
    auto *adapter = static_cast<NativeAdapter *>(context);
    return adapter && adapter->opened
               ? iptv_native_backend_submit_audio(&adapter->backend, data, bytes, pts_us)
               : -1;
}

int AdapterDisableAudio(void *context)
{
    auto *adapter = static_cast<NativeAdapter *>(context);
    return adapter && adapter->opened ? iptv_native_backend_disable_audio(&adapter->backend) : 0;
}

int AdapterDrain(void *context)
{
    auto *adapter = static_cast<NativeAdapter *>(context);
    return adapter && adapter->opened ? iptv_native_backend_drain(&adapter->backend) : 0;
}

void AdapterClose(void *context)
{
    auto *adapter = static_cast<NativeAdapter *>(context);
    if (!adapter || !adapter->initialized)
        return;
    (void)iptv_native_backend_close(&adapter->backend);
    adapter->opened = false;
    adapter->initialized = false;
}

std::uint32_t Vp9LevelForDimensions(std::uint32_t width, std::uint32_t height)
{
    if (width <= 1920u && height <= 1080u)
        return 41u;
    if (width <= 2560u && height <= 1440u)
        return 50u;
    if (width <= 3840u && height <= 2160u)
        return 51u;
    return 0u;
}

enum class RunnerMode
{
    none,
    transport_stream,
    webm
};

class StreamRunner
{
  public:
    void SetStopAfter(unsigned milliseconds)
    {
        stop_deadline_usec_ = 0;
        if (!milliseconds)
            return;
        const std::uint64_t now = MonotonicUsec();
        if (now)
            stop_deadline_usec_ = now + static_cast<std::uint64_t>(milliseconds) * UINT64_C(1000);
    }

    bool Start()
    {
        if (Close() != 0)
            return false;
        if (iptv_native_backend_init(&adapter_.backend) != 0)
            return false;
        adapter_.initialized = true;

        iptv_stream_init(&session_);
        iptv_stream_backend_t backend{};
        backend.context = &adapter_;
        backend.open = AdapterOpen;
        backend.submit_video = AdapterVideo;
        backend.submit_audio = AdapterAudio;
        backend.disable_audio = AdapterDisableAudio;
        backend.drain = AdapterDrain;
        backend.close = AdapterClose;
        backend.hardware_validated = 0;

        if (iptv_stream_open(&session_, nullptr, &backend) != IPTV_STREAM_OK ||
            iptv_stream_start(&session_) != IPTV_STREAM_OK)
        {
            Close();
            return false;
        }
        active_ = true;
        mode_ = RunnerMode::transport_stream;
        if (!StartReadAhead())
        {
            Close();
            return false;
        }
        return true;
    }

    bool StartWebm()
    {
        if (Close() != 0)
            return false;
        if (iptv_native_backend_init(&adapter_.backend) != 0)
            return false;
        adapter_.initialized = true;
        iptv_stream_init(&session_);
        session_.telemetry.state = IPTV_STREAM_STATE_OPEN;
        session_.telemetry.last_result = IPTV_STREAM_OK;
        active_ = true;
        mode_ = RunnerMode::webm;
        webm_finished_ = false;
        return true;
    }

    bool IsWebm() const
    {
        return active_ && mode_ == RunnerMode::webm;
    }

    int Push(const void *data, std::size_t bytes)
    {
        if (!active_ || mode_ != RunnerMode::transport_stream || !data || !bytes ||
            !read_ahead_buffer_ || !read_ahead_thread_)
            return IPTV_STREAM_INVALID_STATE;

        const auto *source = static_cast<const std::uint8_t *>(data);
        while (bytes)
        {
            const int worker_result = read_ahead_result_.load(std::memory_order_acquire);
            if (worker_result != IPTV_STREAM_OK)
                return worker_result;
            const std::uint64_t read = read_ahead_read_.load(std::memory_order_acquire);
            const std::uint64_t write = read_ahead_write_.load(std::memory_order_relaxed);
            const std::size_t used = static_cast<std::size_t>(write - read);
            if (used >= kReadAheadBytes)
            {
                if (StopRequested())
                    return IPTV_STREAM_OK;
                sceKernelUsleep(1000u);
                continue;
            }
            const std::size_t offset = static_cast<std::size_t>(write % kReadAheadBytes);
            std::size_t chunk = kReadAheadBytes - used;
            if (chunk > kReadAheadBytes - offset)
                chunk = kReadAheadBytes - offset;
            if (chunk > bytes)
                chunk = bytes;
            std::memcpy(read_ahead_buffer_ + offset, source, chunk);
            read_ahead_write_.store(write + chunk, std::memory_order_release);
            source += chunk;
            bytes -= chunk;
        }
        return IPTV_STREAM_OK;
    }

    bool FlushInput()
    {
        if (!read_ahead_thread_)
            return true;
        read_ahead_flush_.store(true, std::memory_order_release);
        while (read_ahead_read_.load(std::memory_order_acquire) !=
               read_ahead_write_.load(std::memory_order_acquire))
        {
            if (read_ahead_result_.load(std::memory_order_acquire) != IPTV_STREAM_OK ||
                StopRequested())
            {
                read_ahead_flush_.store(false, std::memory_order_release);
                return false;
            }
            sceKernelUsleep(1000u);
        }
        read_ahead_flush_.store(false, std::memory_order_release);
        return read_ahead_result_.load(std::memory_order_acquire) == IPTV_STREAM_OK;
    }

    int PushWebm(const iptv_webm_video_info_t &video, const iptv_webm_block_t &block)
    {
        if (!active_ || mode_ != RunnerMode::webm || !block.data || block.bytes == 0)
            return IPTV_STREAM_INVALID_STATE;
        if (!adapter_.opened)
        {
            const std::uint32_t level =
                Vp9LevelForDimensions(video.pixel_width, video.pixel_height);
            if (!level)
                return FailWebm(IPTV_STREAM_UNSUPPORTED_FORMAT, "unsupported VP9 WebM dimensions");

            iptv_stream_format_t format{};
            format.video_codec = IPTV_STREAM_VIDEO_VP9;
            format.video_stream_type = UINT32_C(0x56503930); // "VP90"
            format.video_profile = video.profile;
            format.video_level = level;
            format.coded_width = video.pixel_width;
            format.coded_height = video.pixel_height;
            format.visible_width = video.pixel_width;
            format.visible_height = video.pixel_height;
            format.video_bit_depth = 8u;
            format.video_chroma_format = IPTV_STREAM_CHROMA_420;
            const int opened = AdapterOpen(&adapter_, &format);
            if (opened != 0)
                return FailWebm(IPTV_STREAM_NATIVE_UNAVAILABLE, "native VP9 backend open failed");
            session_.telemetry.format = format;
            session_.telemetry.backend_open = 1u;
            session_.telemetry.state = IPTV_STREAM_STATE_READY;
        }

        const int submitted = AdapterVideo(&adapter_, block.data, block.bytes, block.pts_us);
        if (submitted != 0)
        {
            ++session_.telemetry.video_submit_errors;
            return FailWebm(IPTV_STREAM_NATIVE_ERROR, "native VP9 submit failed");
        }
        ++session_.telemetry.video_access_units;
        session_.telemetry.video_bytes += block.bytes;
        session_.telemetry.last_video_pts_us = block.pts_us;
        session_.telemetry.state = IPTV_STREAM_STATE_PLAYING;
        return IPTV_STREAM_OK;
    }

    void RecordWebmFailure(int result)
    {
        if (result >= 0 || session_.telemetry.last_error[0])
            return;
        char message[IPTV_STREAM_ERROR_TEXT_BYTES]{};
        std::snprintf(message, sizeof(message), "WebM: %s",
                      iptv_webm_result_name(static_cast<iptv_webm_result_t>(result)));
        FailWebm(IPTV_STREAM_UNSUPPORTED_FORMAT, message);
    }

    bool StopRequested()
    {
        if (stop_deadline_usec_)
        {
            const std::uint64_t now = MonotonicUsec();
            if (now >= stop_deadline_usec_)
            {
                playback_stop_requested_ = true;
                if (adapter_.initialized)
                    iptv_native_backend_request_stop(&adapter_.backend);
                return true;
            }
        }
        iptv_input_poll();
        const bool overlay_chord =
            iptv_input_pressed(IPTV_INPUT_TOUCHPAD) && iptv_input_pressed(IPTV_INPUT_R1);
        if (overlay_chord && !overlay_chord_down_)
            iptv_native_agc_set_overlay_enabled(!iptv_native_agc_overlay_enabled());
        overlay_chord_down_ = overlay_chord;
        iptv_input_event_t event{};
        while (iptv_input_next(&event))
        {
            if (event.pressed &&
                (event.action == IPTV_INPUT_CIRCLE || event.action == IPTV_INPUT_OPTIONS))
            {
                playback_stop_requested_ = true;
                if (adapter_.initialized)
                {
                    iptv_native_backend_request_stop(&adapter_.backend);
                }
                return true;
            }
        }
        if (adapter_.initialized && iptv_native_backend_stop_requested(&adapter_.backend) != 0)
        {
            playback_stop_requested_ = true;
            return true;
        }
        return false;
    }

    const iptv_stream_telemetry_t *Telemetry() const
    {
        return iptv_stream_telemetry(&session_);
    }

    bool NativeTelemetry(iptv_native_telemetry_t *telemetry) const
    {
        return telemetry && iptv_native_backend_get_telemetry(&adapter_.backend, telemetry) == 0;
    }

    bool HasPresentedVideo() const
    {
        return PresentedFrames() != 0;
    }

    void ConfigureHlsBuffering(std::uint32_t target_duration_ms)
    {
        if (!target_duration_ms)
            return;
        const std::uint64_t initial = HlsInitialUsec(target_duration_ms);
        read_ahead_initial_us_.store(initial, std::memory_order_release);
        read_ahead_rebuffer_us_.store(HlsRebufferUsec(target_duration_ms),
                                      std::memory_order_release);
        read_ahead_maximum_us_.store(initial, std::memory_order_release);
    }

    std::uint64_t PresentedFrames() const
    {
        return iptv_native_backend_presented_frames(&adapter_.backend);
    }

    int Finish()
    {
        if (!active_)
            return IPTV_STREAM_INVALID_STATE;
        if (mode_ == RunnerMode::transport_stream)
        {
            const int read_ahead_result = StopReadAhead(true);
            if (read_ahead_result != IPTV_STREAM_OK)
                return read_ahead_result;
            const int stop_result = iptv_stream_stop(&session_);
            const iptv_stream_telemetry_t *telemetry = Telemetry();
            const bool drain_only =
                telemetry &&
                std::strncmp(telemetry->last_error, "native backend drain failed", 27u) == 0;
            if (stop_result != IPTV_STREAM_OK && drain_only && HasPresentedVideo())
                return IPTV_STREAM_OK;
            return stop_result;
        }
        if (mode_ != RunnerMode::webm)
            return IPTV_STREAM_INVALID_STATE;
        if (webm_finished_)
            return session_.telemetry.last_result;
        if (session_.telemetry.state == IPTV_STREAM_STATE_ERROR)
        {
            ++session_.telemetry.stop_count;
            webm_finished_ = true;
            return session_.telemetry.last_result;
        }

        const int drained = adapter_.opened ? AdapterDrain(&adapter_) : -1;
        ++session_.telemetry.stop_count;
        webm_finished_ = true;
        if (drained != 0 || !HasPresentedVideo())
            return FailWebm(IPTV_STREAM_NATIVE_ERROR, "native VP9 drain failed");
        session_.telemetry.last_result = IPTV_STREAM_OK;
        session_.telemetry.state = IPTV_STREAM_STATE_STOPPED;
        return IPTV_STREAM_OK;
    }

    int Close()
    {
        if (active_)
        {
            if (mode_ == RunnerMode::transport_stream)
            {
                (void)StopReadAhead(false);
                const int stop_result = iptv_stream_stop(&session_);
                const int cleanup_result = iptv_stream_cleanup(&session_);
                ++player_cleanup_count_;
                (void)stop_result;
                RecordCleanupResult(cleanup_result);
            }
            else if (mode_ == RunnerMode::webm)
            {
                const int stop_result = Finish();
                (void)stop_result;
                if (adapter_.initialized)
                    AdapterClose(&adapter_);
                iptv_native_telemetry_t native{};
                const int cleanup_result =
                    NativeTelemetry(&native) ? native.cleanup_result : IPTV_STREAM_NATIVE_ERROR;
                ++session_.telemetry.cleanup_count;
                session_.telemetry.last_cleanup_result = cleanup_result;
                session_.telemetry.backend_open = 0u;
                ++player_cleanup_count_;
                RecordCleanupResult(cleanup_result);
            }
            active_ = false;
            mode_ = RunnerMode::none;
        }
        if (adapter_.initialized)
            AdapterClose(&adapter_);
        iptv_native_telemetry_t native{};
        if (NativeTelemetry(&native))
            RecordCleanupResult(native.cleanup_result);
        return player_cleanup_result_;
    }

    std::uint64_t PlayerCleanupCount() const
    {
        return player_cleanup_count_;
    }
    int PlayerCleanupResult() const
    {
        return player_cleanup_result_;
    }
    bool PlaybackStopRequested() const
    {
        return playback_stop_requested_;
    }

    ~StreamRunner()
    {
        Close();
    }

  private:
    static void *ReadAheadEntry(void *context)
    {
        static_cast<StreamRunner *>(context)->ReadAheadLoop();
        return nullptr;
    }

    bool StartReadAhead()
    {
        read_ahead_buffer_ = new (std::nothrow) std::uint8_t[kReadAheadBytes];
        if (!read_ahead_buffer_)
            return false;
        read_ahead_read_.store(0, std::memory_order_relaxed);
        read_ahead_write_.store(0, std::memory_order_relaxed);
        read_ahead_result_.store(IPTV_STREAM_OK, std::memory_order_relaxed);
        read_ahead_stop_.store(false, std::memory_order_relaxed);
        read_ahead_finished_.store(false, std::memory_order_relaxed);
        read_ahead_flush_.store(false, std::memory_order_relaxed);
        if (scePthreadCreate(&read_ahead_thread_, nullptr, ReadAheadEntry, this,
                             "prosperotv-buffer") != 0)
        {
            delete[] read_ahead_buffer_;
            read_ahead_buffer_ = nullptr;
            read_ahead_thread_ = nullptr;
            return false;
        }
        return true;
    }

    void ReadAheadLoop()
    {
        bool primed = false;
        unsigned underruns = 0;
        std::uint64_t prime_started_us = 0;
        while (!read_ahead_stop_.load(std::memory_order_acquire))
        {
            const std::uint64_t read = read_ahead_read_.load(std::memory_order_relaxed);
            const std::uint64_t write = read_ahead_write_.load(std::memory_order_acquire);
            const std::size_t available = static_cast<std::size_t>(write - read);
            const bool finished = read_ahead_finished_.load(std::memory_order_acquire);
            const bool flushing = read_ahead_flush_.load(std::memory_order_acquire);
            if (!primed)
            {
                if (available == 0 && finished)
                    break;
                if (available == 0)
                {
                    prime_started_us = 0;
                    sceKernelUsleep(1000u);
                    continue;
                }
                const std::uint64_t now = MonotonicUsec();
                if (prime_started_us == 0)
                    prime_started_us = now;
                const std::uint64_t elapsed = now >= prime_started_us ? now - prime_started_us : 0;
                const std::uint64_t required = ReadAheadPrimeUsec(
                    underruns, read_ahead_initial_us_.load(std::memory_order_acquire),
                    read_ahead_rebuffer_us_.load(std::memory_order_acquire),
                    read_ahead_maximum_us_.load(std::memory_order_acquire));
                if (!finished && !flushing && !ReadAheadPrimed(available, elapsed, required))
                {
                    sceKernelUsleep(1000u);
                    continue;
                }
                primed = true;
            }
            if (available == 0)
            {
                if (finished)
                    break;
                primed = false;
                ++underruns;
                prime_started_us = 0;
                sceKernelUsleep(1000u);
                continue;
            }

            const std::size_t offset = static_cast<std::size_t>(read % kReadAheadBytes);
            std::size_t chunk = available;
            if (chunk > kReadBytes)
                chunk = kReadBytes;
            if (chunk > kReadAheadBytes - offset)
                chunk = kReadAheadBytes - offset;
            const int result = iptv_stream_push(&session_, read_ahead_buffer_ + offset, chunk);
            if (result != IPTV_STREAM_OK)
            {
                read_ahead_result_.store(result, std::memory_order_release);
                break;
            }
            read_ahead_read_.store(read + chunk, std::memory_order_release);
        }
    }

    int StopReadAhead(bool drain)
    {
        if (!read_ahead_thread_)
            return read_ahead_result_.load(std::memory_order_acquire);
        if (drain)
        {
            read_ahead_finished_.store(true, std::memory_order_release);
        }
        else
        {
            read_ahead_stop_.store(true, std::memory_order_release);
        }
        const int join_result = scePthreadJoin(read_ahead_thread_, nullptr);
        read_ahead_thread_ = nullptr;
        delete[] read_ahead_buffer_;
        read_ahead_buffer_ = nullptr;
        if (join_result != 0)
            return IPTV_STREAM_NATIVE_ERROR;
        return read_ahead_result_.load(std::memory_order_acquire);
    }

    int FailWebm(int result, const char *message)
    {
        if (session_.telemetry.state == IPTV_STREAM_STATE_ERROR)
            return session_.telemetry.last_result;
        session_.telemetry.last_result = result;
        session_.telemetry.state = IPTV_STREAM_STATE_ERROR;
        ++session_.telemetry.error_count;
        std::snprintf(session_.telemetry.last_error, sizeof(session_.telemetry.last_error), "%s",
                      message ? message : "VP9 WebM playback failed");
        return result;
    }

    void RecordCleanupResult(int result)
    {
        if (player_cleanup_result_ == 0 && result != 0)
            player_cleanup_result_ = result;
    }

    NativeAdapter adapter_{};
    iptv_stream_session_t session_{};
    bool active_ = false;
    RunnerMode mode_ = RunnerMode::none;
    bool webm_finished_ = false;
    std::uint64_t stop_deadline_usec_ = 0;
    std::uint64_t player_cleanup_count_ = 0;
    int player_cleanup_result_ = 0;
    bool playback_stop_requested_ = false;
    bool overlay_chord_down_ = false;
    std::uint8_t *read_ahead_buffer_ = nullptr;
    std::atomic<std::uint64_t> read_ahead_initial_us_{kReadAheadInitialUsec};
    std::atomic<std::uint64_t> read_ahead_rebuffer_us_{kReadAheadRebufferUsec};
    std::atomic<std::uint64_t> read_ahead_maximum_us_{kReadAheadMaximumUsec};
    void *read_ahead_thread_ = nullptr;
    std::atomic<std::uint64_t> read_ahead_read_{0};
    std::atomic<std::uint64_t> read_ahead_write_{0};
    std::atomic<int> read_ahead_result_{IPTV_STREAM_OK};
    std::atomic<bool> read_ahead_stop_{false};
    std::atomic<bool> read_ahead_finished_{false};
    std::atomic<bool> read_ahead_flush_{false};
};

enum class FeedResult
{
    ok,
    stopped,
    reopen,
    failed
};

FeedResult FeedRequest(iptv::http::StreamRequest *request, StreamRunner *runner,
                       std::uint8_t *buffer, std::size_t buffer_bytes)
{
    if (!request || !runner || !buffer || !buffer_bytes)
        return FeedResult::failed;
    unsigned read_failures = 0;
    std::uint64_t last_presented = runner->PresentedFrames();
    std::uint64_t last_progress = MonotonicUsec();
    while (!runner->StopRequested())
    {
        const int read = iptv::http::ReadStream(request, buffer, buffer_bytes);
        if (read == 0)
            return FeedResult::ok;
        if (read < 0)
        {
            if (++read_failures >= 3u)
            {
                SetLastPlaybackError("The channel connection was interrupted while streaming.");
                return FeedResult::failed;
            }
            sceKernelUsleep(100000u);
            continue;
        }
        read_failures = 0;
        const int pushed = runner->Push(buffer, static_cast<std::size_t>(read));
        if (pushed == IPTV_STREAM_REOPEN_REQUIRED)
            return FeedResult::reopen;
        if (pushed != IPTV_STREAM_OK)
        {
            const iptv_stream_telemetry_t *telemetry = runner->Telemetry();
            SetLastPlaybackError("%s", telemetry && telemetry->last_error[0]
                                           ? telemetry->last_error
                                           : "The channel uses an unsupported media format.");
            return FeedResult::failed;
        }
        const std::uint64_t presented = runner->PresentedFrames();
        const std::uint64_t now = MonotonicUsec();
        if (presented > last_presented)
        {
            last_presented = presented;
            last_progress = now;
        }
        else if (last_progress != 0 && now >= last_progress &&
                 now - last_progress >= kVideoProgressTimeoutUsec)
        {
            SetLastPlaybackError("The channel stalled without producing video for 15 seconds.");
            return FeedResult::failed;
        }
    }
    return FeedResult::stopped;
}

int SubmitWebmVideo(void *context, const iptv_webm_video_info_t *video,
                    const iptv_webm_block_t *block)
{
    auto *runner = static_cast<StreamRunner *>(context);
    return runner && video && block && runner->PushWebm(*video, *block) == IPTV_STREAM_OK ? 0 : -1;
}

int RunWebm(iptv::http::StreamRequest *request, StreamRunner *runner,
            const std::uint8_t *initial_data, std::size_t initial_bytes, std::uint8_t *read_buffer)
{
    if (!request || !runner || !initial_data || !initial_bytes || !read_buffer)
        return -1;

    iptv_webm_parser_t parser{};
    iptv_webm_init(&parser);
    iptv_webm_limits_t limits{};
    iptv_webm_default_limits(&limits);
    int result = iptv_webm_open(&parser, &limits, SubmitWebmVideo, runner);
    if (result == IPTV_WEBM_OK)
        result = iptv_webm_push(&parser, initial_data, initial_bytes);

    unsigned read_failures = 0;
    std::uint64_t last_presented = runner->PresentedFrames();
    std::uint64_t last_progress = MonotonicUsec();
    while (result == IPTV_WEBM_OK && !runner->StopRequested())
    {
        const int read = iptv::http::ReadStream(request, read_buffer, kReadBytes);
        if (read == 0)
        {
            result = iptv_webm_finish(&parser);
            if (result == IPTV_WEBM_OK)
                result = runner->Finish();
            break;
        }
        if (read < 0)
        {
            if (++read_failures >= 3u)
                result = IPTV_WEBM_TRUNCATED;
            else
                sceKernelUsleep(100000u);
            continue;
        }
        read_failures = 0;
        result = iptv_webm_push(&parser, read_buffer, static_cast<std::size_t>(read));
        const std::uint64_t presented = runner->PresentedFrames();
        const std::uint64_t now = MonotonicUsec();
        if (presented > last_presented)
        {
            last_presented = presented;
            last_progress = now;
        }
        else if (last_progress != 0 && now >= last_progress &&
                 now - last_progress >= kVideoProgressTimeoutUsec)
        {
            result = IPTV_WEBM_CALLBACK_ERROR;
        }
    }
    if (result == IPTV_WEBM_OK && runner->StopRequested())
        result = 1;
    runner->RecordWebmFailure(result);
    const int cleanup = iptv_webm_cleanup(&parser);
    if (cleanup != IPTV_WEBM_OK && result == IPTV_WEBM_OK)
        result = cleanup;
    if (result != IPTV_WEBM_OK)
        return result;
    return runner->HasPresentedVideo() ? 0 : -1;
}

bool WaitForRefresh(StreamRunner *runner, std::uint32_t milliseconds)
{
    std::uint32_t remaining = milliseconds;
    while (remaining)
    {
        if (runner->StopRequested())
            return false;
        const std::uint32_t slice = remaining > 100u ? 100u : remaining;
        sceKernelUsleep(slice * 1000u);
        remaining -= slice;
    }
    return !runner->StopRequested();
}

int FetchPlaylist(const char *url, char *data, std::size_t capacity, std::size_t *bytes,
                  char *effective_url, std::size_t effective_url_capacity,
                  const iptv::http::RequestHeaders *headers)
{
    if (!url || !data || capacity < kPlaylistBytes + 1u || !bytes || !effective_url ||
        effective_url_capacity == 0)
    {
        return -1;
    }
    const auto result = iptv::http::GetM3uResolved(url, data, capacity, effective_url,
                                                   effective_url_capacity, kPlaylistBytes, headers);
    *bytes = result.bytes;
    if (result.status != iptv::http::Status::ok)
        SetRequestFailure("Playlist request failed", result.status, result.http_status,
                          result.native_error, data);
    return result.status == iptv::http::Status::ok ? 0 : -static_cast<int>(result.status);
}

int OpenAndFeedSegment(const char *url, StreamRunner *runner, std::uint8_t *buffer,
                       const iptv::http::RequestHeaders *headers)
{
    for (unsigned attempt = 0; attempt < 3u; ++attempt)
    {
        iptv::http::StreamRequest request{};
        const auto status = iptv::http::OpenStream(url, "video/mp2t, */*", &request, headers);
        if (status != iptv::http::Status::ok)
        {
            if (attempt == 2u)
            {
                SetRequestFailure("Channel segment request failed", status, request.http_status,
                                  request.native_error, request.error_response);
                return -static_cast<int>(status);
            }
            sceKernelUsleep(100000u);
            continue;
        }
        const FeedResult fed = FeedRequest(&request, runner, buffer, kReadBytes);
        iptv::http::CloseStream(&request);
        if (fed == FeedResult::stopped)
            return 1;
        if (fed == FeedResult::ok)
            return 0;
        if (attempt == 2u || !runner->Start())
        {
            if (!gLastPlaybackError[0])
                SetLastPlaybackError("The channel segment could not be decoded.");
            return -1;
        }
        sceKernelUsleep(100000u);
    }
    return -1;
}

int RunHlsMedia(const char *source_url, StreamRunner *runner, std::uint8_t *read_buffer,
                char *playlist_data, const iptv_hls_limits_t *limits, iptv_hls_playlist_t *playlist,
                const iptv::http::RequestHeaders *headers)
{
    char media_url[IPTV_HLS_URL_BYTES]{};
    if (std::strlen(source_url) >= sizeof(media_url))
        return IPTV_HLS_URL_LIMIT;
    std::snprintf(media_url, sizeof(media_url), "%s", source_url);
    char effective_url[IPTV_HLS_URL_BYTES]{};
    std::size_t playlist_bytes = 0;
    std::uint64_t next_sequence = 0;
    std::uint64_t last_discontinuity_sequence = 0;
    bool have_sequence = false;
    bool have_discontinuity_sequence = false;
    bool session_has_data = false;
    unsigned stale_refreshes = 0;
    unsigned consecutive_segment_failures = 0;
    std::uint64_t last_presented = runner->PresentedFrames();
    for (;;)
    {
        if (runner->StopRequested())
            return 1;
        int result = FetchPlaylist(media_url, playlist_data, kPlaylistBytes + 1u, &playlist_bytes,
                                   effective_url, sizeof(effective_url), headers);
        if (result != 0)
            return result;
        const iptv_hls_result_t parsed =
            iptv_hls_parse(playlist_data, playlist_bytes, effective_url, std::strlen(effective_url),
                           limits, playlist);
        if (parsed != IPTV_HLS_OK || playlist->kind != IPTV_HLS_KIND_MEDIA)
        {
            if (iptv::http::ResponseIndicatesGeographicBlock(playlist_data, playlist_bytes))
                SetLastPlaybackError(
                    "Playlist request failed: unavailable in your region (GeoIP blocked).");
            else
                SetLastPlaybackError(
                    "HLS playlist is not playable: %s.",
                    iptv_hls_result_name(parsed == IPTV_HLS_OK ? IPTV_HLS_MALFORMED : parsed));
            return parsed == IPTV_HLS_OK ? IPTV_HLS_MALFORMED : parsed;
        }
        runner->ConfigureHlsBuffering(playlist->target_duration_ms);
        std::snprintf(media_url, sizeof(media_url), "%s", effective_url);

        if (have_sequence && playlist->segment_count)
        {
            const std::uint64_t first = playlist->segments[0].sequence;
            const std::uint64_t last = playlist->segments[playlist->segment_count - 1u].sequence;
            if (first > next_sequence || last + 1u < next_sequence)
            {
                if (!runner->Start())
                {
                    return -1;
                }
                have_sequence = false;
                have_discontinuity_sequence = false;
                session_has_data = false;
                last_presented = 0;
            }
        }

        std::uint32_t start = 0;
        if (!have_sequence && playlist->is_live && playlist->segment_count > 3u)
        {
            start = playlist->segment_count - 3u;
        }
        for (std::uint32_t i = start; i < playlist->segment_count; ++i)
        {
            const iptv_hls_segment_t &segment = playlist->segments[i];
            if (have_sequence && segment.sequence < next_sequence)
                continue;
            const bool changed_timeline =
                have_discontinuity_sequence &&
                segment.discontinuity_sequence != last_discontinuity_sequence;
            if ((segment.discontinuity || changed_timeline) && session_has_data)
            {
                if (!runner->FlushInput() || !runner->Start())
                {
                    return -1;
                }
                session_has_data = false;
                last_presented = 0;
            }
            result = OpenAndFeedSegment(segment.url, runner, read_buffer, headers);
            if (result != 0)
            {
                if (result == 1 || !playlist->is_live || ++consecutive_segment_failures >= 3u)
                    return result;
                // Public live playlists frequently retain a segment for a moment
                // after its CDN object expires. Skip that object and reopen the
                // decoder at the next segment instead of abandoning the channel.
                if (!runner->Start())
                    return -1;
                have_sequence = true;
                next_sequence = segment.sequence + 1u;
                have_discontinuity_sequence = true;
                last_discontinuity_sequence = segment.discontinuity_sequence;
                session_has_data = false;
                stale_refreshes = 0;
                last_presented = 0;
                continue;
            }
            consecutive_segment_failures = 0;
            have_sequence = true;
            next_sequence = segment.sequence + 1u;
            have_discontinuity_sequence = true;
            last_discontinuity_sequence = segment.discontinuity_sequence;
            session_has_data = true;
        }

        if (!playlist->is_live)
        {
            const int finished = runner->Finish();
            return finished == IPTV_STREAM_OK && runner->HasPresentedVideo() ? 0 : -1;
        }
        const std::uint64_t presented = runner->PresentedFrames();
        if (presented > last_presented)
        {
            stale_refreshes = 0;
            last_presented = presented;
        }
        else if (presented < last_presented)
        {
            // A segment retry reopened the stream session and reset native telemetry.
            stale_refreshes = 0;
            last_presented = presented;
        }
        else if (++stale_refreshes >= 6u)
        {
            SetLastPlaybackError("The live channel stopped producing video.");
            return -1;
        }
        std::uint32_t refresh_ms = playlist->target_duration_ms / 2u;
        if (refresh_ms < 500u)
            refresh_ms = 500u;
        if (!WaitForRefresh(runner, refresh_ms))
        {
            return 1;
        }
    }
}

int RunHls(const char *source_url, StreamRunner *runner, std::uint8_t *read_buffer,
           char *playlist_data, const iptv::http::RequestHeaders *headers)
{
    auto *playlist = new (std::nothrow) iptv_hls_playlist_t{};
    if (!playlist)
        return -1;

    iptv_hls_limits_t limits{};
    iptv_hls_default_limits(&limits);

    char effective_url[IPTV_HLS_URL_BYTES]{};
    std::size_t playlist_bytes = 0;
    int result = FetchPlaylist(source_url, playlist_data, kPlaylistBytes + 1u, &playlist_bytes,
                               effective_url, sizeof(effective_url), headers);
    if (result != 0)
    {
        delete playlist;
        return result;
    }

    const iptv_hls_result_t parsed = iptv_hls_parse(playlist_data, playlist_bytes, effective_url,
                                                    std::strlen(effective_url), &limits, playlist);
    if (parsed != IPTV_HLS_OK)
    {
        if (iptv::http::ResponseIndicatesGeographicBlock(playlist_data, playlist_bytes))
            SetLastPlaybackError(
                "Playlist request failed: unavailable in your region (GeoIP blocked).");
        else
            SetLastPlaybackError("HLS playlist is not playable: %s.", iptv_hls_result_name(parsed));
        delete playlist;
        return parsed;
    }
    if (playlist->kind == IPTV_HLS_KIND_MEDIA)
    {
        result = RunHlsMedia(effective_url, runner, read_buffer, playlist_data, &limits, playlist,
                             headers);
        delete playlist;
        return result;
    }
    if (playlist->kind != IPTV_HLS_KIND_MASTER)
    {
        delete playlist;
        return IPTV_HLS_MALFORMED;
    }

    auto *candidates = new (std::nothrow) HlsVariantCandidates{};
    if (!candidates)
    {
        delete playlist;
        return -1;
    }
    BuildNativeCandidates(playlist, &limits, candidates);
    if (!candidates->count)
    {
        SetLastPlaybackError("No H.264 or HEVC HLS quality is supported by this PS5 decoder.");
        delete candidates;
        delete playlist;
        return IPTV_HLS_NO_VARIANT_WITHIN_LIMITS;
    }

    for (std::uint32_t index = 0; index < candidates->count; ++index)
    {
        if (index && !runner->Start())
        {
            result = -1;
            break;
        }
        result = RunHlsMedia(candidates->urls[index], runner, read_buffer, playlist_data, &limits,
                             playlist, headers);
        if (result >= 0)
            break;
    }

    delete candidates;
    delete playlist;
    return result;
}

int RunDirect(const char *url, StreamRunner *runner, std::uint8_t *read_buffer, char *playlist_data,
              const iptv::http::RequestHeaders *headers)
{
    for (unsigned attempt = 0; attempt < 3u; ++attempt)
    {
        iptv::http::StreamRequest request{};
        const auto status = iptv::http::OpenStream(
            url, "video/mp2t, video/webm, application/vnd.apple.mpegurl, */*", &request, headers);
        if (status != iptv::http::Status::ok)
        {
            if (attempt == 2u)
            {
                SetRequestFailure("Channel request failed", status, request.http_status,
                                  request.native_error, request.error_response);
                return -static_cast<int>(status);
            }
            sceKernelUsleep(100000u);
            continue;
        }

        const int first = ReadInitialProbe(&request, read_buffer, kReadBytes);
        if (first <= 0)
        {
            iptv::http::CloseStream(&request);
            if (attempt == 2u)
            {
                SetLastPlaybackError("The channel opened but returned no media data.");
                return -1;
            }
            sceKernelUsleep(100000u);
            continue;
        }
        if (iptv::http::ResponseIndicatesGeographicBlock(
                reinterpret_cast<const char *>(read_buffer), static_cast<std::size_t>(first)))
        {
            iptv::http::CloseStream(&request);
            SetLastPlaybackError(
                "Channel request failed: unavailable in your region (GeoIP blocked).");
            return -static_cast<int>(iptv::http::Status::http_status_error);
        }
        if (BufferLooksLikeHls(read_buffer, static_cast<std::size_t>(first)))
        {
            iptv::http::CloseStream(&request);
            return RunHls(request.effective_url, runner, read_buffer, playlist_data, headers);
        }
        if (UrlLooksLikeWebm(request.effective_url) ||
            BufferLooksLikeWebm(read_buffer, static_cast<std::size_t>(first)))
        {
            if ((attempt != 0u || !runner->IsWebm()) && !runner->StartWebm())
            {
                iptv::http::CloseStream(&request);
                SetLastPlaybackError("The VP9 decoder could not start for this channel.");
                return -1;
            }
            const int webm = RunWebm(&request, runner, read_buffer, static_cast<std::size_t>(first),
                                     read_buffer);
            iptv::http::CloseStream(&request);
            if (webm >= 0)
                return webm;
            if (attempt == 2u)
                return webm;
            sceKernelUsleep(100000u);
            continue;
        }
        const int pushed = runner->Push(read_buffer, static_cast<std::size_t>(first));
        if (pushed != IPTV_STREAM_OK && pushed != IPTV_STREAM_REOPEN_REQUIRED)
        {
            const iptv_stream_telemetry_t *telemetry = runner->Telemetry();
            if (telemetry && telemetry->last_error[0])
                SetLastPlaybackError("%s", telemetry->last_error);
        }
        FeedResult fed = pushed == IPTV_STREAM_REOPEN_REQUIRED ? FeedResult::reopen
                         : pushed == IPTV_STREAM_OK
                             ? FeedRequest(&request, runner, read_buffer, kReadBytes)
                             : FeedResult::failed;
        iptv::http::CloseStream(&request);
        if (fed == FeedResult::stopped)
            return 1;
        if (fed == FeedResult::ok)
        {
            const int finished = runner->Finish();
            if (finished == IPTV_STREAM_OK && runner->HasPresentedVideo())
                return 0;
        }
        if (attempt == 2u || !runner->Start())
        {
            if (!gLastPlaybackError[0])
                SetLastPlaybackError("The channel media could not be decoded.");
            return -1;
        }
        sceKernelUsleep(100000u);
    }
    return -1;
}

} // namespace

static int RunPlayer(const char *url, const char *channel_name, const char *user_agent,
                     const char *referrer, unsigned stop_after_ms)
{
    SetLastPlaybackError(nullptr);
    if (!url || !*url)
    {
        SetLastPlaybackError("The channel has no stream URL.");
        return -1;
    }

    char message[192]{};
    std::snprintf(message, sizeof(message), "IPTV: opening %.150s",
                  channel_name && *channel_name ? channel_name : "channel");
    Notify(message);

    const auto network = iptv::http::NetworkInit();
    if (network != iptv::http::Status::ok)
    {
        SetRequestFailure("Network initialization failed", network, 0, 0, nullptr);
        NotifyLastPlaybackError();
        return -1;
    }
    const bool input_ready = iptv_input_init();
    if (!input_ready)
    {
        SetLastPlaybackError("Controller initialization failed.");
        Notify("IPTV: controller initialization failed");
        iptv::http::NetworkShutdown();
        return -1;
    }

    const iptv::http::RequestHeaders headers{user_agent, referrer};
    auto *runner = new (std::nothrow) StreamRunner{};
    auto *read_buffer = new (std::nothrow) std::uint8_t[kReadBytes];
    auto *playlist_data = new (std::nothrow) char[kPlaylistBytes + 1u];
    int result = -1;
    if (!runner || !read_buffer || !playlist_data)
    {
        SetLastPlaybackError("Not enough memory to open this channel.");
        Notify("IPTV: out of memory");
    }
    else
    {
        runner->SetStopAfter(stop_after_ms);
        if (!(UrlLooksLikeWebm(url) ? runner->StartWebm() : runner->Start()))
        {
            SetLastPlaybackError("The hardware decoder session could not be initialized.");
            Notify("IPTV: decoder session initialization failed");
        }
        else
        {
            (void)iptv_native_agc_loading_start();
            result = UrlLooksLikeHls(url)
                         ? RunHls(url, runner, read_buffer, playlist_data, &headers)
                         : RunDirect(url, runner, read_buffer, playlist_data, &headers);
            if (stop_after_ms && result >= 0 && runner->PlaybackStopRequested() &&
                !runner->HasPresentedVideo())
            {
                result = -1;
                SetLastPlaybackError(
                    "The channel did not produce a decodable video frame within the test window.");
            }
            if (result < 0)
            {
                const iptv_stream_telemetry_t *telemetry = runner->Telemetry();
                iptv_native_telemetry_t native{};
                const bool have_native = runner->NativeTelemetry(&native);
                const int native_result = have_native && native.last_native_result != 0
                                              ? native.last_native_result
                                              : native.last_result;
                const std::uint32_t native_code = static_cast<std::uint32_t>(native_result);
                if (telemetry &&
                    std::strstr(telemetry->last_error, "unable to acquire MPEG-TS packet sync"))
                    SetLastPlaybackError(
                        "The channel did not provide a valid MPEG-TS stream (packet sync failed).");
                else if (native_result == -5)
                    SetLastPlaybackError(
                        "PS5 VideoOut timed out while presenting a decoded frame.");
                else if (native_result == -1004)
                    SetLastPlaybackError(
                        "The PS5 hardware decoder did not produce a valid frame for this stream.");
                else if (native_result == -1003)
                    SetLastPlaybackError(
                        "The channel sent a video access unit the PS5 decoder cannot accept.");
                else if (native_result == -1002)
                    SetLastPlaybackError(
                        "This channel's codec profile or resolution is not supported.");
                else if (native_code == UINT32_C(0x811D0301))
                    SetLastPlaybackError(
                        "The channel sent an invalid video access unit (VideoDec2 0x811D0301).");
                else if (native_code == UINT32_C(0x811D0302))
                    SetLastPlaybackError("The channel exceeds the PS5 decoder instance capacity "
                                         "(VideoDec2 0x811D0302).");
                else if (native_code == UINT32_C(0x811D0303))
                    SetLastPlaybackError("The channel did not provide a valid video sequence "
                                         "(VideoDec2 0x811D0303).");
                else if (native_code == UINT32_C(0x811D0304))
                    SetLastPlaybackError("The channel contains a fatal video bitstream error "
                                         "(VideoDec2 0x811D0304).");
                else if (native_result != 0 && native_result != -125)
                    SetLastPlaybackError("The PS5 video backend failed (native error 0x%08X).",
                                         static_cast<unsigned>(native_result));
                else if (telemetry && telemetry->last_error[0] && !gLastPlaybackError[0])
                    SetLastPlaybackError("%s", telemetry->last_error);
                if (!gLastPlaybackError[0])
                    SetLastPlaybackError("The channel could not be played.");
                NotifyLastPlaybackError();
            }
        }
    }

    if (runner)
    {
        iptv_native_agc_loading_stop();
        runner->Close();
        const iptv_stream_telemetry_t *current = runner->Telemetry();
        iptv_native_telemetry_t native{};
        const bool have_native_telemetry = runner->NativeTelemetry(&native);
        if (current)
        {
            SaveReceipt(result, *current, native, have_native_telemetry ? 1u : 0u,
                        runner->PlaybackStopRequested() ? 1u : 0u, runner->PlayerCleanupCount(),
                        runner->PlayerCleanupResult());
        }
    }
    (void)iptv_native_agc_present_shutdown();
    delete runner;
    delete[] playlist_data;
    delete[] read_buffer;
    iptv_input_shutdown();
    iptv::http::NetworkShutdown();
    return result;
}

int iptv_player_run_with_headers(const char *url, const char *channel_name, const char *user_agent,
                                 const char *referrer)
{
    return RunPlayer(url, channel_name, user_agent, referrer, 0);
}

const char *iptv_player_last_error(void)
{
    return gLastPlaybackError;
}

int iptv_player_run(const char *url, const char *channel_name)
{
    return iptv_player_run_with_headers(url, channel_name, nullptr, nullptr);
}

int iptv_player_run_controlled(const char *url, const char *channel_name, unsigned stop_after_ms)
{
    return RunPlayer(url, channel_name, nullptr, nullptr, stop_after_ms);
}
