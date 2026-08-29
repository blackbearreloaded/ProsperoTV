/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_NATIVE_BACKEND_H
#define IPTV_NATIVE_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPTV_NATIVE_BACKEND_STORAGE_BYTES (64u * 1024u)
#define IPTV_NATIVE_CHROMA_420 420u
#define IPTV_NATIVE_H264_PROFILE_BASELINE 66u
#define IPTV_NATIVE_H264_PROFILE_MAIN 77u
#define IPTV_NATIVE_H264_PROFILE_HIGH 100u
#define IPTV_NATIVE_HEVC_PROFILE_MAIN 1u
#define IPTV_NATIVE_VP9_PROFILE_0 0u

typedef enum iptv_native_codec {
    IPTV_NATIVE_CODEC_H264 = 1,
    IPTV_NATIVE_CODEC_HEVC_MAIN8 = 2,
    IPTV_NATIVE_CODEC_VP9_PROFILE0 = 3
} iptv_native_codec_t;

typedef enum iptv_native_state {
    IPTV_NATIVE_STATE_UNINITIALIZED = 0,
    IPTV_NATIVE_STATE_IDLE,
    IPTV_NATIVE_STATE_OPEN,
    IPTV_NATIVE_STATE_STOPPING,
    IPTV_NATIVE_STATE_CLOSED,
    IPTV_NATIVE_STATE_ERROR
} iptv_native_state_t;

typedef struct iptv_native_open_config {
    iptv_native_codec_t codec;
    uint32_t profile; /* AVC/HEVC profile id or VP9 profile number. */
    uint32_t level;   /* Codec level (for example 41, 50, 51 or 153). */
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t bit_depth;
    uint32_t chroma_format; /* IPTV_NATIVE_CHROMA_420 only. */
    uint32_t hdr;
    uint32_t enable_audio;
} iptv_native_open_config_t;

typedef struct iptv_native_telemetry {
    iptv_native_state_t state;
    iptv_native_codec_t codec;
    uint32_t profile;
    uint32_t level;
    int32_t last_result;
    int32_t last_native_result;
    int32_t cleanup_result;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t output_pitch;
    uint32_t output_surface_height;
    uint64_t input_slot_bytes;
    uint64_t frame_slot_bytes;
    uint64_t submitted_video_access_units;
    uint64_t submitted_video_bytes;
    uint64_t decoded_frames;
    uint64_t presented_frames;
    uint64_t hidden_decoded_frames;
    uint64_t buffered_video_access_units;
    uint64_t drained_video_frames;
    uint64_t dropped_delayed_frames;
    uint64_t decoder_flushes;
    uint64_t drain_flush_limit_hits;
    uint64_t decoder_errors;
    uint64_t rejected_video_access_units;
    uint64_t submitted_audio_frames;
    uint64_t decoded_audio_frames;
    uint64_t audio_output_grains;
    uint64_t audio_output_errors;
    uint64_t decode_total_us;
    uint64_t decode_max_us;
    uint64_t present_total_us;
    uint64_t present_max_us;
    uint64_t first_frame_latency_us;
    uint64_t last_video_pts_us;
    uint64_t last_presented_video_pts_us;
    uint64_t last_audio_pts_us;
    uint64_t unknown_video_timestamps;
    uint32_t pending_video_timestamps;
    uint64_t pacing_resets;
    uint64_t pacing_waits;
    uint64_t pacing_wait_total_us;
    uint64_t pacing_wait_max_us;
    uint64_t pacing_late_frames;
    uint64_t pacing_max_late_us;
    uintptr_t last_decoder_output;
    uintptr_t last_present_source;
    uint32_t decoder_output_in_frame_pool;
    uint32_t zero_copy_pointer_match;
    uint32_t hardware_validated;
    uint32_t stream_acceptance_validated;
    uint32_t stop_requested;
} iptv_native_telemetry_t;

typedef union iptv_native_backend {
    long double alignment;
    unsigned char storage[IPTV_NATIVE_BACKEND_STORAGE_BYTES];
} iptv_native_backend_t;

int32_t iptv_native_backend_init(iptv_native_backend_t *backend);
int32_t iptv_native_backend_open(iptv_native_backend_t *backend,
                                 const iptv_native_open_config_t *config);
int32_t iptv_native_backend_submit_video(iptv_native_backend_t *backend,
                                         const void *coded_packet,
                                         size_t access_unit_bytes,
                                         uint64_t pts_us); /* media PTS, usec */
int32_t iptv_native_backend_submit_audio(iptv_native_backend_t *backend,
                                         const void *adts_frame,
                                         size_t frame_bytes,
                                         uint64_t pts_us);
void iptv_native_backend_request_stop(iptv_native_backend_t *backend);
int iptv_native_backend_stop_requested(const iptv_native_backend_t *backend);
int32_t iptv_native_backend_drain(iptv_native_backend_t *backend);
int32_t iptv_native_backend_get_telemetry(
    const iptv_native_backend_t *backend, iptv_native_telemetry_t *telemetry);
int32_t iptv_native_backend_close(iptv_native_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif
