/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_WEBM_H
#define IPTV_WEBM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define IPTV_WEBM_API_VERSION UINT32_C(1)
#define IPTV_WEBM_DEFAULT_MAX_ELEMENT_BYTES (8u * 1024u * 1024u)
#define IPTV_WEBM_DEFAULT_MAX_RETAINED_BYTES (IPTV_WEBM_DEFAULT_MAX_ELEMENT_BYTES + 16u)
#define IPTV_WEBM_DEFAULT_MAX_NESTING 8u
#define IPTV_WEBM_HARD_MAX_NESTING 16u
#define IPTV_WEBM_DEFAULT_MAX_WIDTH 3840u
#define IPTV_WEBM_DEFAULT_MAX_HEIGHT 2160u

    typedef enum iptv_webm_result
    {
        IPTV_WEBM_OK = 0,
        IPTV_WEBM_INVALID_ARGUMENT = -1,
        IPTV_WEBM_INVALID_STATE = -2,
        IPTV_WEBM_OUT_OF_MEMORY = -3,
        IPTV_WEBM_RETAINED_LIMIT = -4,
        IPTV_WEBM_ELEMENT_LIMIT = -5,
        IPTV_WEBM_NESTING_LIMIT = -6,
        IPTV_WEBM_DIMENSION_LIMIT = -7,
        IPTV_WEBM_MALFORMED = -8,
        IPTV_WEBM_TRUNCATED = -9,
        IPTV_WEBM_UNSUPPORTED_DOCUMENT = -10,
        IPTV_WEBM_UNSUPPORTED_CODEC = -11,
        IPTV_WEBM_UNSUPPORTED_PROFILE = -12,
        IPTV_WEBM_UNSUPPORTED_CONTENT_ENCODING = -13,
        IPTV_WEBM_UNSUPPORTED_LACING = -14,
        IPTV_WEBM_MISSING_VIDEO_TRACK = -15,
        IPTV_WEBM_TIMESTAMP_OVERFLOW = -16,
        IPTV_WEBM_CALLBACK_ERROR = -17
    } iptv_webm_result_t;

    typedef enum iptv_webm_block_kind
    {
        IPTV_WEBM_SIMPLE_BLOCK = 1,
        IPTV_WEBM_BLOCK = 2
    } iptv_webm_block_kind_t;

    typedef struct iptv_webm_limits
    {
        uint32_t max_retained_bytes;
        uint32_t max_element_bytes;
        uint32_t max_nesting;
        uint32_t max_width;
        uint32_t max_height;
    } iptv_webm_limits_t;

    typedef struct iptv_webm_video_info
    {
        uint64_t track_number;
        uint64_t timestamp_scale_ns;
        uint32_t profile;
        uint32_t profile_present;
        uint32_t pixel_width;
        uint32_t pixel_height;
        uint32_t display_width;
        uint32_t display_height;
        uint32_t display_unit;
    } iptv_webm_video_info_t;

    typedef struct iptv_webm_block
    {
        const uint8_t *data;
        size_t bytes;
        uint64_t pts_us;
        uint64_t track_number;
        iptv_webm_block_kind_t kind;
        uint32_t keyframe;
        uint32_t invisible;
        uint32_t discardable;
    } iptv_webm_block_t;

    /* Block data borrows parser storage and is valid only during the callback. */
    typedef int (*iptv_webm_video_callback_t)(void *context, const iptv_webm_video_info_t *video,
                                              const iptv_webm_block_t *block);

    typedef struct iptv_webm_parser
    {
        uint32_t _magic;
        void *_impl;
        iptv_webm_video_info_t video;
        iptv_webm_result_t last_result;
    } iptv_webm_parser_t;

    void iptv_webm_default_limits(iptv_webm_limits_t *limits);
    void iptv_webm_init(iptv_webm_parser_t *parser);
    iptv_webm_result_t iptv_webm_open(iptv_webm_parser_t *parser, const iptv_webm_limits_t *limits,
                                      iptv_webm_video_callback_t callback, void *context);
    iptv_webm_result_t iptv_webm_push(iptv_webm_parser_t *parser, const void *data, size_t bytes);
    iptv_webm_result_t iptv_webm_finish(iptv_webm_parser_t *parser);
    iptv_webm_result_t iptv_webm_cleanup(iptv_webm_parser_t *parser);
    const iptv_webm_video_info_t *iptv_webm_video(const iptv_webm_parser_t *parser);
    const char *iptv_webm_result_name(iptv_webm_result_t result);

#ifdef __cplusplus
}
#endif

#endif
