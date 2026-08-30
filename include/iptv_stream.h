/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_STREAM_H
#define IPTV_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPTV_STREAM_API_VERSION UINT32_C(3)
#define IPTV_STREAM_TS_PACKET_BYTES 188u
#define IPTV_STREAM_ERROR_TEXT_BYTES 96u
#define IPTV_STREAM_DEFAULT_MAX_PES_BYTES UINT32_C(0x800000)
#define IPTV_STREAM_PTS_UNKNOWN UINT64_MAX

typedef enum iptv_stream_state {
    IPTV_STREAM_STATE_IDLE = 0,
    IPTV_STREAM_STATE_OPEN,
    IPTV_STREAM_STATE_BUFFERING,
    IPTV_STREAM_STATE_READY,
    IPTV_STREAM_STATE_PLAYING,
    IPTV_STREAM_STATE_STOPPED,
    IPTV_STREAM_STATE_ERROR
} iptv_stream_state_t;

typedef enum iptv_stream_result {
    IPTV_STREAM_OK = 0,
    IPTV_STREAM_INVALID_ARGUMENT = -1,
    IPTV_STREAM_INVALID_STATE = -2,
    IPTV_STREAM_UNSUPPORTED_FORMAT = -3,
    IPTV_STREAM_MALFORMED_TS = -4,
    IPTV_STREAM_BUFFER_LIMIT = -5,
    IPTV_STREAM_NATIVE_UNAVAILABLE = -6,
    IPTV_STREAM_NATIVE_ERROR = -7,
    IPTV_STREAM_REOPEN_REQUIRED = -8
} iptv_stream_result_t;

typedef enum iptv_stream_video_codec {
    IPTV_STREAM_VIDEO_UNKNOWN = 0,
    IPTV_STREAM_VIDEO_H264 = 1,
    IPTV_STREAM_VIDEO_HEVC = 2,
    IPTV_STREAM_VIDEO_VP9 = 3
} iptv_stream_video_codec_t;

typedef enum iptv_stream_chroma_format {
    IPTV_STREAM_CHROMA_UNKNOWN = 0,
    IPTV_STREAM_CHROMA_MONO = 1,
    IPTV_STREAM_CHROMA_420 = 2,
    IPTV_STREAM_CHROMA_422 = 3,
    IPTV_STREAM_CHROMA_444 = 4
} iptv_stream_chroma_format_t;

/* PIDs and codec are known from PAT/PMT. Profile and bit depth are refined
 * from an in-band SPS before the first video access unit is submitted. */
typedef struct iptv_stream_format {
    uint32_t program_number;
    uint32_t pmt_pid;
    uint32_t pcr_pid;
    uint32_t video_pid;
    uint32_t audio_pid;
    uint32_t video_stream_type;
    uint32_t audio_stream_type;
    uint32_t video_codec;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t video_profile;
    uint32_t video_level;
    uint32_t video_bit_depth;
    uint32_t video_chroma_format;
    uint32_t audio_sample_rate;
    uint32_t audio_channels;
} iptv_stream_format_t;

typedef struct iptv_stream_config {
    /* Upper bound for each active PES payload and retained elementary stream
     * assembly buffer. Zero selects IPTV_STREAM_DEFAULT_MAX_PES_BYTES. */
    uint32_t max_pes_bytes;
} iptv_stream_config_t;

/* The adapter owns decoder, presenter and audio resources. Video callbacks
 * receive one complete Annex-B access unit. Audio callbacks receive one ADTS
 * frame. A callback must consume or copy data before it returns. */
typedef struct iptv_stream_backend {
    void *context;
    int (*open)(void *context, const iptv_stream_format_t *format);
    int (*submit_video)(void *context, const uint8_t *data, size_t bytes,
                        uint64_t pts_us);
    int (*submit_audio)(void *context, const uint8_t *data, size_t bytes,
                        uint64_t pts_us);
    int (*disable_audio)(void *context);
    int (*drain)(void *context);
    void (*close)(void *context);
    uint32_t hardware_validated;
} iptv_stream_backend_t;

typedef struct iptv_stream_telemetry {
    iptv_stream_state_t state;
    int32_t last_result;
    char last_error[IPTV_STREAM_ERROR_TEXT_BYTES];

    iptv_stream_format_t format;
    uint32_t max_pes_bytes;
    uint32_t buffered_bytes;
    uint32_t buffered_bytes_max;
    uint32_t backend_open;
    uint32_t hardware_validated;
    uint32_t audio_disabled;
    char audio_warning[IPTV_STREAM_ERROR_TEXT_BYTES];

    uint64_t packets;
    uint64_t pat_sections;
    uint64_t pmt_sections;
    uint64_t video_pes;
    uint64_t audio_pes;
    uint64_t video_access_units;
    uint64_t audio_frames;
    uint64_t video_bytes;
    uint64_t audio_bytes;
    uint64_t video_submit_errors;
    uint64_t audio_submit_errors;
    uint64_t error_count;
    uint64_t continuity_errors;
    uint64_t discontinuities;
    uint64_t duplicate_packets;
    uint64_t dropped_payloads;
    uint64_t pts_wraps;
    uint64_t buffer_errors;
    uint64_t cleanup_count;
    uint64_t stop_count;
    int32_t last_cleanup_result;
    uint64_t last_video_pts_us;
    uint64_t last_audio_pts_us;
} iptv_stream_telemetry_t;

typedef struct iptv_stream_session {
    uint32_t _magic;
    void *_impl;
    iptv_stream_telemetry_t telemetry;
} iptv_stream_session_t;

void iptv_stream_init(iptv_stream_session_t *session);
int iptv_stream_open(iptv_stream_session_t *session,
                     const iptv_stream_config_t *config,
                     const iptv_stream_backend_t *backend);
int iptv_stream_start(iptv_stream_session_t *session);
int iptv_stream_push(iptv_stream_session_t *session,
                     const void *data, size_t bytes);
int iptv_stream_stop(iptv_stream_session_t *session);
int iptv_stream_cleanup(iptv_stream_session_t *session);

iptv_stream_state_t iptv_stream_state(
    const iptv_stream_session_t *session);
const iptv_stream_telemetry_t *iptv_stream_telemetry(
    const iptv_stream_session_t *session);
const char *iptv_stream_state_name(iptv_stream_state_t state);

#ifdef __cplusplus
}
#endif

#endif
