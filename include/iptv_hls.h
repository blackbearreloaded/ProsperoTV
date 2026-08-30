/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_HLS_H
#define IPTV_HLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPTV_HLS_API_VERSION UINT32_C(2)
#define IPTV_HLS_URL_BYTES 2048u
#define IPTV_HLS_MAX_VARIANTS 24u
#define IPTV_HLS_MAX_SEGMENTS 128u
#define IPTV_HLS_DEFAULT_MAX_INPUT_BYTES (256u * 1024u)
#define IPTV_HLS_DEFAULT_MAX_LINE_BYTES 2048u
#define IPTV_HLS_DEFAULT_MAX_LINES 2048u
#define IPTV_HLS_NO_VARIANT UINT32_C(0xffffffff)

typedef enum iptv_hls_result {
    IPTV_HLS_OK = 0,
    IPTV_HLS_INVALID_ARGUMENT = -1,
    IPTV_HLS_INPUT_LIMIT = -2,
    IPTV_HLS_LINE_LIMIT = -3,
    IPTV_HLS_OUTPUT_LIMIT = -4,
    IPTV_HLS_MALFORMED = -5,
    IPTV_HLS_INVALID_URL = -6,
    IPTV_HLS_URL_LIMIT = -7,
    IPTV_HLS_UNSUPPORTED_ENCRYPTION = -8,
    IPTV_HLS_UNSUPPORTED_BYTE_RANGE = -9,
    IPTV_HLS_UNSUPPORTED_FMP4 = -10,
    IPTV_HLS_UNSUPPORTED_CODEC = -11,
    IPTV_HLS_NO_VARIANT_WITHIN_LIMITS = -12,
    IPTV_HLS_UNSUPPORTED_FEATURE = -13
} iptv_hls_result_t;

typedef enum iptv_hls_kind {
    IPTV_HLS_KIND_NONE = 0,
    IPTV_HLS_KIND_MASTER,
    IPTV_HLS_KIND_MEDIA
} iptv_hls_kind_t;

typedef enum iptv_hls_codec {
    IPTV_HLS_CODEC_UNKNOWN = 0,
    IPTV_HLS_CODEC_AVC,
    IPTV_HLS_CODEC_HEVC,
    IPTV_HLS_CODEC_UNSUPPORTED
} iptv_hls_codec_t;

typedef enum iptv_hls_video_range {
    IPTV_HLS_VIDEO_RANGE_UNKNOWN = 0,
    IPTV_HLS_VIDEO_RANGE_SDR,
    IPTV_HLS_VIDEO_RANGE_PQ,
    IPTV_HLS_VIDEO_RANGE_HLG,
    IPTV_HLS_VIDEO_RANGE_UNSUPPORTED
} iptv_hls_video_range_t;

typedef struct iptv_hls_limits {
    uint32_t max_input_bytes;
    uint32_t max_line_bytes;
    uint32_t max_lines;
    uint32_t max_variants;
    uint32_t max_segments;
    uint32_t max_width;
    uint32_t max_height;
    uint64_t max_bandwidth;
    iptv_hls_codec_t preferred_codec;
} iptv_hls_limits_t;

typedef struct iptv_hls_variant {
    char url[IPTV_HLS_URL_BYTES];
    uint64_t bandwidth;
    uint64_t average_bandwidth;
    uint32_t width;
    uint32_t height;
    iptv_hls_codec_t codec;
    uint32_t profile;
    uint32_t level;
    uint32_t bit_depth;
    uint32_t high_tier;
    iptv_hls_video_range_t video_range;
    uint32_t compatible;
    uint32_t within_limits;
} iptv_hls_variant_t;

typedef struct iptv_hls_segment {
    char url[IPTV_HLS_URL_BYTES];
    uint64_t sequence;
    uint64_t discontinuity_sequence;
    uint32_t duration_ms;
    uint32_t discontinuity;
} iptv_hls_segment_t;

typedef struct iptv_hls_playlist {
    iptv_hls_kind_t kind;
    uint32_t error_line;

    uint32_t variant_count;
    uint32_t selected_variant;
    iptv_hls_variant_t variants[IPTV_HLS_MAX_VARIANTS];

    uint32_t is_live;
    uint32_t target_duration_ms;
    uint64_t media_sequence;
    uint64_t discontinuity_sequence;
    uint32_t segment_count;
    iptv_hls_segment_t segments[IPTV_HLS_MAX_SEGMENTS];
} iptv_hls_playlist_t;

void iptv_hls_default_limits(iptv_hls_limits_t *limits);

iptv_hls_result_t iptv_hls_resolve_url(
    const char *base_url, size_t base_url_bytes,
    const char *reference, size_t reference_bytes,
    char *output, size_t output_bytes);

iptv_hls_result_t iptv_hls_parse(
    const char *data, size_t data_bytes,
    const char *playlist_url, size_t playlist_url_bytes,
    const iptv_hls_limits_t *limits,
    iptv_hls_playlist_t *playlist);

/* Returns the best eligible master variant not present in excluded_variants.
 * Bit N excludes variants[N]; IPTV_HLS_MAX_VARIANTS is bounded to 24. */
uint32_t iptv_hls_select_variant(
    const iptv_hls_playlist_t *playlist,
    const iptv_hls_limits_t *limits,
    uint32_t excluded_variants);

const char *iptv_hls_result_name(iptv_hls_result_t result);

#ifdef __cplusplus
}
#endif

#endif
