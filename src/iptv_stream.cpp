/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_stream.h"

#include <cstring>
#include <limits>
#include <new>

namespace
{

constexpr uint32_t kMagic = UINT32_C(0x49565354);
constexpr uint16_t kPatPid = 0;
constexpr uint16_t kNullPid = 0x1fffu;
constexpr size_t kPsiBytes = 1024u;
constexpr size_t kPesHeaderBytes = 264u;
constexpr size_t kContinuityEntries = 8u;
constexpr size_t kPtsMarkers = 64u;
constexpr size_t kSyncPackets = 3u;
constexpr size_t kPacketBufferBytes = IPTV_STREAM_TS_PACKET_BYTES * 4u;
constexpr size_t kNoOffset = static_cast<size_t>(-1);
constexpr uint64_t kPtsModulus = UINT64_C(1) << 33;
constexpr uint64_t kPtsHalf = UINT64_C(1) << 32;

struct buffer_t
{
    uint8_t *data;
    size_t size;
    size_t capacity;
};

struct psi_t
{
    uint8_t data[kPsiBytes];
    size_t size;
    size_t expected;
    bool active;
};

struct continuity_t
{
    uint16_t pid;
    uint8_t last;
    bool seen;
};

struct timestamp_t
{
    uint64_t last_raw;
    uint64_t epoch;
    bool seen;
};

struct pts_marker_t
{
    size_t offset;
    uint64_t pts_us;
};

struct marker_list_t
{
    pts_marker_t items[kPtsMarkers];
    size_t count;
    uint64_t base_pts;
};

struct pes_t
{
    uint8_t header[kPesHeaderBytes];
    size_t header_bytes;
    size_t header_size;
    uint64_t expected_payload;
    uint64_t payload_bytes;
    uint64_t pts_us;
    bool active;
    bool video;
    bool header_complete;
    bool unbounded;
    bool marker_added;
};

struct bit_reader_t
{
    const uint8_t *data;
    size_t bits;
    size_t at;
};

struct impl_t
{
    iptv_stream_config_t config;
    iptv_stream_backend_t backend;
    bool has_backend;
    bool started;
    bool backend_open;
    bool backend_ever_opened;
    bool pat_seen;
    bool pmt_seen;
    bool video_sps;
    bool video_pps;
    bool video_vps;

    uint8_t packet[kPacketBufferBytes];
    size_t packet_bytes;
    bool packet_sync;
    iptv_stream_format_t format;
    continuity_t continuity[kContinuityEntries];
    psi_t pat;
    psi_t pmt;
    pes_t video_pes;
    pes_t audio_pes;
    timestamp_t video_time;
    timestamp_t audio_time;
    buffer_t video_es;
    buffer_t audio_es;
    marker_list_t video_markers;
    marker_list_t audio_markers;
};

static bool valid_session(const iptv_stream_session_t *session)
{
    return session && session->_magic == kMagic;
}

static impl_t *get_impl(iptv_stream_session_t *session)
{
    return static_cast<impl_t *>(session ? session->_impl : nullptr);
}

static void copy_error(iptv_stream_telemetry_t *telemetry, const char *text)
{
    if (!text)
        text = "stream error";
    std::strncpy(telemetry->last_error, text, IPTV_STREAM_ERROR_TEXT_BYTES - 1u);
    telemetry->last_error[IPTV_STREAM_ERROR_TEXT_BYTES - 1u] = '\0';
}

static int fail(iptv_stream_session_t *session, int result, const char *text)
{
    session->telemetry.state = IPTV_STREAM_STATE_ERROR;
    session->telemetry.last_result = result;
    ++session->telemetry.error_count;
    copy_error(&session->telemetry, text);
    return result;
}

static bool buffer_init(buffer_t *buffer, size_t capacity)
{
    buffer->data = new (std::nothrow) uint8_t[capacity];
    buffer->size = 0;
    buffer->capacity = buffer->data ? capacity : 0;
    return buffer->data != nullptr;
}

static void buffer_release(buffer_t *buffer)
{
    delete[] buffer->data;
    buffer->data = nullptr;
    buffer->size = 0;
    buffer->capacity = 0;
}

static bool buffer_append(buffer_t *buffer, const uint8_t *data, size_t bytes)
{
    if (bytes > buffer->capacity - buffer->size)
        return false;
    if (bytes)
        std::memcpy(buffer->data + buffer->size, data, bytes);
    buffer->size += bytes;
    return true;
}

static void buffer_erase(buffer_t *buffer, size_t bytes)
{
    if (bytes >= buffer->size)
    {
        buffer->size = 0;
        return;
    }
    std::memmove(buffer->data, buffer->data + bytes, buffer->size - bytes);
    buffer->size -= bytes;
}

static uint64_t marker_at(const marker_list_t *markers, size_t offset)
{
    uint64_t result = markers->base_pts;
    for (size_t i = 0; i < markers->count && markers->items[i].offset <= offset; ++i)
        result = markers->items[i].pts_us;
    return result;
}

static bool marker_add(marker_list_t *markers, size_t offset, uint64_t pts_us)
{
    if (pts_us == IPTV_STREAM_PTS_UNKNOWN)
        return true;
    if (offset == 0 && markers->count == 0)
    {
        markers->base_pts = pts_us;
        return true;
    }
    if (markers->count && markers->items[markers->count - 1u].offset == offset)
    {
        markers->items[markers->count - 1u].pts_us = pts_us;
        return true;
    }
    if (markers->count >= kPtsMarkers)
        return false;
    markers->items[markers->count++] = {offset, pts_us};
    return true;
}

static void marker_erase(marker_list_t *markers, size_t bytes, uint64_t next_pts)
{
    uint64_t base = marker_at(markers, bytes);
    size_t out = 0;
    bool exact = false;
    for (size_t i = 0; i < markers->count; ++i)
    {
        if (markers->items[i].offset < bytes)
            continue;
        if (markers->items[i].offset == bytes)
        {
            base = markers->items[i].pts_us;
            exact = true;
            continue;
        }
        markers->items[out] = markers->items[i];
        markers->items[out].offset -= bytes;
        ++out;
    }
    markers->count = out;
    markers->base_pts = !exact && next_pts != IPTV_STREAM_PTS_UNKNOWN ? next_pts : base;
}

static void marker_clear(marker_list_t *markers)
{
    markers->count = 0;
    markers->base_pts = IPTV_STREAM_PTS_UNKNOWN;
}

static void update_buffered(iptv_stream_session_t *session, const impl_t *impl)
{
    const size_t total = impl->packet_bytes + impl->video_es.size + impl->audio_es.size;
    const uint32_t value = total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
    session->telemetry.buffered_bytes = value;
    if (value > session->telemetry.buffered_bytes_max)
        session->telemetry.buffered_bytes_max = value;
}

static uint32_t crc32_mpeg(const uint8_t *data, size_t bytes)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < bytes; ++i)
    {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & UINT32_C(0x80000000)) ? (crc << 1) ^ UINT32_C(0x04c11db7) : crc << 1;
    }
    return crc;
}

static bool read_bits(bit_reader_t *reader, unsigned count, uint32_t *value)
{
    if (count > 32u || reader->at + count > reader->bits)
        return false;
    uint32_t out = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        out = (out << 1) | ((reader->data[reader->at >> 3] >> (7u - (reader->at & 7u))) & 1u);
        ++reader->at;
    }
    *value = out;
    return true;
}

static bool skip_bits(bit_reader_t *reader, size_t count)
{
    if (reader->at + count > reader->bits)
        return false;
    reader->at += count;
    return true;
}

static bool read_ue(bit_reader_t *reader, uint32_t *value)
{
    unsigned zeros = 0;
    uint32_t bit = 0;
    while (zeros < 31u)
    {
        if (!read_bits(reader, 1, &bit))
            return false;
        if (bit)
            break;
        ++zeros;
    }
    uint32_t suffix = 0;
    if (zeros && !read_bits(reader, zeros, &suffix))
        return false;
    *value = ((UINT32_C(1) << zeros) - 1u) + suffix;
    return true;
}

static bool read_se(bit_reader_t *reader, int32_t *value)
{
    uint32_t code = 0;
    if (!read_ue(reader, &code))
        return false;
    *value =
        (code & 1u) ? static_cast<int32_t>((code + 1u) >> 1) : -static_cast<int32_t>(code >> 1);
    return true;
}

static size_t make_rbsp(const uint8_t *data, size_t bytes, uint8_t *out, size_t capacity)
{
    size_t written = 0;
    unsigned zeros = 0;
    for (size_t i = 0; i < bytes; ++i)
    {
        if (zeros >= 2u && data[i] == 0x03u)
        {
            zeros = 0;
            continue;
        }
        if (written >= capacity)
            return 0;
        out[written++] = data[i];
        zeros = data[i] == 0 ? zeros + 1u : 0u;
    }
    return written;
}

static uint32_t chroma_api(uint32_t chroma)
{
    switch (chroma)
    {
    case 0:
        return IPTV_STREAM_CHROMA_MONO;
    case 1:
        return IPTV_STREAM_CHROMA_420;
    case 2:
        return IPTV_STREAM_CHROMA_422;
    case 3:
        return IPTV_STREAM_CHROMA_444;
    default:
        return IPTV_STREAM_CHROMA_UNKNOWN;
    }
}

static bool parse_pts_raw(const uint8_t *data, uint64_t *raw)
{
    if ((data[0] & 1u) == 0 || (data[2] & 1u) == 0 || (data[4] & 1u) == 0)
        return false;
    *raw = ((static_cast<uint64_t>(data[0] >> 1) & 7u) << 30) |
           (static_cast<uint64_t>(data[1]) << 22) |
           ((static_cast<uint64_t>(data[2] >> 1) & 0x7fu) << 15) |
           (static_cast<uint64_t>(data[3]) << 7) | ((data[4] >> 1) & 0x7fu);
    return true;
}

static uint64_t extend_pts(iptv_stream_session_t *session, timestamp_t *time, uint64_t raw)
{
    if (time->seen && raw + kPtsHalf < time->last_raw)
    {
        time->epoch += kPtsModulus;
        ++session->telemetry.pts_wraps;
    }
    time->last_raw = raw;
    time->seen = true;
    const uint64_t ticks = time->epoch + raw;
    return (ticks / 90u) * 1000u + ((ticks % 90u) * 1000u) / 90u;
}

static bool h264_extended_profile(uint32_t profile)
{
    return profile == 100u || profile == 110u || profile == 122u || profile == 244u ||
           profile == 44u || profile == 83u || profile == 86u || profile == 118u ||
           profile == 128u || profile == 138u || profile == 139u || profile == 134u ||
           profile == 135u;
}

static int accept_video_format(iptv_stream_session_t *session, impl_t *impl, uint32_t profile,
                               uint32_t level, uint32_t coded_width, uint32_t coded_height,
                               uint32_t visible_width, uint32_t visible_height, uint32_t bit_depth,
                               uint32_t chroma)
{
    const iptv_stream_format_t &old = impl->format;
    if (impl->backend_open &&
        (old.video_profile != profile || old.video_level != level ||
         old.coded_width != coded_width || old.coded_height != coded_height ||
         old.visible_width != visible_width || old.visible_height != visible_height ||
         old.video_bit_depth != bit_depth || old.video_chroma_format != chroma))
        return fail(session, IPTV_STREAM_REOPEN_REQUIRED,
                    "video configuration changed; reopen required");

    impl->format.video_profile = profile;
    impl->format.video_level = level;
    impl->format.video_bit_depth = bit_depth;
    impl->format.video_chroma_format = chroma;
    impl->format.coded_width = coded_width;
    impl->format.coded_height = coded_height;
    impl->format.visible_width = visible_width;
    impl->format.visible_height = visible_height;
    impl->video_sps = true;
    session->telemetry.format = impl->format;
    return IPTV_STREAM_OK;
}

static int parse_h264_sps(iptv_stream_session_t *session, impl_t *impl, const uint8_t *nal,
                          size_t bytes)
{
    uint8_t rbsp[1024];
    if (bytes < 4u || (nal[0] & 0x1fu) != 7u)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 SPS");
    const size_t rbsp_bytes = make_rbsp(nal + 1u, bytes - 1u, rbsp, sizeof(rbsp));
    if (rbsp_bytes < 4u)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "truncated H.264 SPS");

    bit_reader_t bits{rbsp, rbsp_bytes * 8u, 0};
    uint32_t profile = 0, constraints = 0, level = 0, value = 0;
    if (!read_bits(&bits, 8, &profile) || !read_bits(&bits, 8, &constraints) ||
        !read_bits(&bits, 8, &level) || !read_ue(&bits, &value))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 SPS header");
    (void)constraints;

    uint32_t chroma = 1;
    uint32_t bit_depth_luma = 8;
    uint32_t bit_depth_chroma = 8;
    bool separate_colour_plane = false;
    if (h264_extended_profile(profile))
    {
        uint32_t separate = 0, transform = 0, scaling = 0;
        if (!read_ue(&bits, &chroma) || chroma > 3u ||
            (chroma == 3u && (!read_bits(&bits, 1, &separate))))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 chroma format");
        separate_colour_plane = separate != 0;
        if (!read_ue(&bits, &value))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 luma depth");
        bit_depth_luma = value + 8u;
        if (!read_ue(&bits, &value))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 chroma depth");
        bit_depth_chroma = value + 8u;
        if (!read_bits(&bits, 1, &transform) || !read_bits(&bits, 1, &scaling))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 SPS flags");
        if (scaling)
        {
            const uint32_t lists = chroma == 3u ? 12u : 8u;
            for (uint32_t i = 0; i < lists; ++i)
            {
                uint32_t present = 0;
                if (!read_bits(&bits, 1, &present))
                    return fail(session, IPTV_STREAM_MALFORMED_TS,
                                "truncated H.264 scaling-list flags");
                if (!present)
                    continue;
                int32_t last = 8;
                int32_t next = 8;
                const uint32_t count = i < 6u ? 16u : 64u;
                for (uint32_t j = 0; j < count; ++j)
                {
                    if (next != 0)
                    {
                        int32_t delta = 0;
                        if (!read_se(&bits, &delta))
                            return fail(session, IPTV_STREAM_MALFORMED_TS,
                                        "truncated H.264 scaling list");
                        next = (last + delta + 256) & 255;
                    }
                    if (next != 0)
                        last = next;
                }
            }
        }
    }
    if (bit_depth_luma != 8u || bit_depth_chroma != 8u || chroma != 1u || separate_colour_plane)
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT, "H.264 must be 8-bit 4:2:0");

    uint32_t pic_order_cnt_type = 0;
    if (!read_ue(&bits, &value) || !read_ue(&bits, &pic_order_cnt_type))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 SPS timing");
    if (pic_order_cnt_type == 0u)
    {
        if (!read_ue(&bits, &value))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 POC");
    }
    else if (pic_order_cnt_type == 1u)
    {
        uint32_t flag = 0, cycle = 0;
        int32_t signed_value = 0;
        if (!read_bits(&bits, 1, &flag) || !read_se(&bits, &signed_value) ||
            !read_se(&bits, &signed_value) || !read_ue(&bits, &cycle) || cycle > 255u)
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 POC cycle");
        for (uint32_t i = 0; i < cycle; ++i)
            if (!read_se(&bits, &signed_value))
                return fail(session, IPTV_STREAM_MALFORMED_TS, "truncated H.264 POC cycle");
    }
    else if (pic_order_cnt_type > 2u)
    {
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 POC type");
    }

    uint32_t gaps = 0, width_mbs = 0, height_maps = 0, frame_only = 0;
    if (!read_ue(&bits, &value) || !read_bits(&bits, 1, &gaps) || !read_ue(&bits, &width_mbs) ||
        !read_ue(&bits, &height_maps) || !read_bits(&bits, 1, &frame_only))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 dimensions");
    (void)gaps;
    if (!frame_only && !skip_bits(&bits, 1))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 field mode");
    uint32_t direct = 0, crop = 0;
    if (!read_bits(&bits, 1, &direct) || !read_bits(&bits, 1, &crop))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 crop flags");
    (void)direct;
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (crop && (!read_ue(&bits, &crop_left) || !read_ue(&bits, &crop_right) ||
                 !read_ue(&bits, &crop_top) || !read_ue(&bits, &crop_bottom)))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 crop");

    const uint64_t coded_width = static_cast<uint64_t>(width_mbs + 1u) * 16u;
    const uint64_t coded_height = static_cast<uint64_t>(2u - frame_only) * (height_maps + 1u) * 16u;
    const uint64_t crop_x = static_cast<uint64_t>(crop_left + crop_right) * 2u;
    const uint64_t crop_y = static_cast<uint64_t>(crop_top + crop_bottom) * 2u * (2u - frame_only);
    if (coded_width > UINT32_MAX || coded_height > UINT32_MAX || crop_x >= coded_width ||
        crop_y >= coded_height)
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT,
                    "H.264 dimensions exceed the stream contract");

    return accept_video_format(session, impl, profile, level, static_cast<uint32_t>(coded_width),
                               static_cast<uint32_t>(coded_height),
                               static_cast<uint32_t>(coded_width - crop_x),
                               static_cast<uint32_t>(coded_height - crop_y), 8, chroma_api(chroma));
}

static int parse_hevc_sps(iptv_stream_session_t *session, impl_t *impl, const uint8_t *nal,
                          size_t bytes)
{
    uint8_t rbsp[1024];
    if (bytes < 5u || ((nal[0] >> 1) & 0x3fu) != 33u)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid HEVC SPS");
    const size_t rbsp_bytes = make_rbsp(nal + 2u, bytes - 2u, rbsp, sizeof(rbsp));
    if (!rbsp_bytes)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "truncated HEVC SPS");
    bit_reader_t bits{rbsp, rbsp_bytes * 8u, 0};
    uint32_t value = 0, sublayers = 0, profile = 0, level = 0;
    if (!read_bits(&bits, 4, &value) || !read_bits(&bits, 3, &sublayers) || !skip_bits(&bits, 1) ||
        !skip_bits(&bits, 3) || !read_bits(&bits, 5, &profile) || !skip_bits(&bits, 32u + 48u) ||
        !read_bits(&bits, 8, &level))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid HEVC profile tier level");
    if (profile != 1u)
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT, "HEVC profile must be Main");

    uint32_t profile_present[7]{};
    uint32_t level_present[7]{};
    for (uint32_t i = 0; i < sublayers; ++i)
        if (!read_bits(&bits, 1, &profile_present[i]) || !read_bits(&bits, 1, &level_present[i]))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid HEVC sublayer flags");
    if (sublayers)
        for (uint32_t i = sublayers; i < 8u; ++i)
            if (!skip_bits(&bits, 2))
                return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid HEVC reserved bits");
    for (uint32_t i = 0; i < sublayers; ++i)
    {
        if (profile_present[i] && !skip_bits(&bits, 88u))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "truncated HEVC sublayer profile");
        if (level_present[i] && !skip_bits(&bits, 8u))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "truncated HEVC sublayer level");
    }

    uint32_t chroma = 0, separate = 0, width = 0, height = 0, crop = 0;
    if (!read_ue(&bits, &value) || !read_ue(&bits, &chroma) || chroma > 3u ||
        (chroma == 3u && !read_bits(&bits, 1, &separate)) || !read_ue(&bits, &width) ||
        !read_ue(&bits, &height) || !read_bits(&bits, 1, &crop))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid HEVC dimensions");
    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (crop && (!read_ue(&bits, &left) || !read_ue(&bits, &right) || !read_ue(&bits, &top) ||
                 !read_ue(&bits, &bottom)))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid HEVC crop");
    uint32_t luma_depth = 0, chroma_depth = 0;
    if (!read_ue(&bits, &luma_depth) || !read_ue(&bits, &chroma_depth))
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid HEVC bit depth");
    if (luma_depth != 0u || chroma_depth != 0u || chroma != 1u || separate)
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT, "HEVC must be Main 8-bit 4:2:0");
    const uint32_t sub_width = chroma == 1u || chroma == 2u ? 2u : 1u;
    const uint32_t sub_height = chroma == 1u ? 2u : 1u;
    const uint64_t crop_x = static_cast<uint64_t>(left + right) * sub_width;
    const uint64_t crop_y = static_cast<uint64_t>(top + bottom) * sub_height;
    if (!width || !height || crop_x >= width || crop_y >= height)
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT,
                    "HEVC dimensions exceed the stream contract");

    return accept_video_format(session, impl, profile, level, width, height,
                               width - static_cast<uint32_t>(crop_x),
                               height - static_cast<uint32_t>(crop_y), 8, chroma_api(chroma));
}

static bool video_config_ready(const impl_t *impl)
{
    if (impl->format.video_codec == IPTV_STREAM_VIDEO_H264)
        return impl->video_sps && impl->video_pps;
    if (impl->format.video_codec == IPTV_STREAM_VIDEO_HEVC)
        return impl->video_vps && impl->video_sps && impl->video_pps;
    return false;
}

static int maybe_activate(iptv_stream_session_t *session, impl_t *impl)
{
    if (!impl->pmt_seen || !video_config_ready(impl) || !impl->started || impl->backend_open)
        return IPTV_STREAM_OK;
    if (!impl->has_backend)
    {
        session->telemetry.state = IPTV_STREAM_STATE_READY;
        return IPTV_STREAM_OK;
    }
    if (impl->backend_ever_opened)
        return fail(session, IPTV_STREAM_INVALID_STATE,
                    "native backend cannot be reopened in one session");
    const int result = impl->backend.open(impl->backend.context, &impl->format);
    if (result != 0)
        return fail(session, IPTV_STREAM_NATIVE_ERROR, "native backend open failed");
    impl->backend_open = true;
    impl->backend_ever_opened = true;
    session->telemetry.backend_open = 1;
    session->telemetry.hardware_validated = impl->backend.hardware_validated;
    session->telemetry.format = impl->format;
    session->telemetry.state = IPTV_STREAM_STATE_PLAYING;
    return IPTV_STREAM_OK;
}

static bool same_program_format(const iptv_stream_format_t *a, const iptv_stream_format_t *b)
{
    return a->program_number == b->program_number && a->pmt_pid == b->pmt_pid &&
           a->pcr_pid == b->pcr_pid && a->video_pid == b->video_pid &&
           a->audio_pid == b->audio_pid && a->video_stream_type == b->video_stream_type &&
           a->audio_stream_type == b->audio_stream_type && a->video_codec == b->video_codec;
}

static int parse_pat_section(iptv_stream_session_t *session, impl_t *impl, const uint8_t *section,
                             size_t bytes)
{
    if (bytes < 12u || section[0] != 0x00u || (section[1] & 0x80u) == 0 || (section[5] & 1u) == 0 ||
        crc32_mpeg(section, bytes) != 0)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid PAT section");
    const size_t end = bytes - 4u;
    uint16_t selected_program = 0;
    uint16_t selected_pid = 0;
    uint16_t first_program = 0;
    uint16_t first_pid = 0;
    for (size_t at = 8u; at + 4u <= end; at += 4u)
    {
        const uint16_t program = static_cast<uint16_t>(section[at] << 8 | section[at + 1u]);
        const uint16_t pid =
            static_cast<uint16_t>((section[at + 2u] & 0x1fu) << 8 | section[at + 3u]);
        if (!program)
            continue;
        if (!first_program)
        {
            first_program = program;
            first_pid = pid;
        }
        if (impl->pat_seen && program == impl->format.program_number)
        {
            selected_program = program;
            selected_pid = pid;
        }
    }
    if (!selected_program)
    {
        selected_program = first_program;
        selected_pid = first_pid;
    }
    if (!selected_program || selected_pid == kPatPid || selected_pid == kNullPid)
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT, "PAT contains no usable program");
    if (impl->backend_ever_opened &&
        (selected_program != impl->format.program_number || selected_pid != impl->format.pmt_pid))
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT,
                    "program changed after native backend open");
    impl->format.program_number = selected_program;
    impl->format.pmt_pid = selected_pid;
    impl->pat_seen = true;
    ++session->telemetry.pat_sections;
    return IPTV_STREAM_OK;
}

static int parse_pmt_section(iptv_stream_session_t *session, impl_t *impl, const uint8_t *section,
                             size_t bytes)
{
    if (bytes < 16u || section[0] != 0x02u || (section[1] & 0x80u) == 0 || (section[5] & 1u) == 0 ||
        crc32_mpeg(section, bytes) != 0 ||
        static_cast<uint16_t>(section[3] << 8 | section[4]) != impl->format.program_number)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid PMT section");

    iptv_stream_format_t next{};
    next.program_number = impl->format.program_number;
    next.pmt_pid = impl->format.pmt_pid;
    next.pcr_pid = static_cast<uint16_t>((section[8] & 0x1fu) << 8 | section[9]);
    const size_t program_info = (static_cast<size_t>(section[10] & 0x0fu) << 8) | section[11];
    const size_t end = bytes - 4u;
    size_t at = 12u + program_info;
    if (at > end)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "PMT program descriptors exceed section");
    while (at + 5u <= end)
    {
        const uint8_t type = section[at];
        const uint16_t pid =
            static_cast<uint16_t>((section[at + 1u] & 0x1fu) << 8 | section[at + 2u]);
        const size_t info = (static_cast<size_t>(section[at + 3u] & 0x0fu) << 8) | section[at + 4u];
        at += 5u;
        if (at + info > end)
            return fail(session, IPTV_STREAM_MALFORMED_TS,
                        "PMT elementary descriptors exceed section");
        if (!next.video_pid && (type == 0x1bu || type == 0x24u))
        {
            next.video_pid = pid;
            next.video_stream_type = type;
            next.video_codec = type == 0x1bu ? IPTV_STREAM_VIDEO_H264 : IPTV_STREAM_VIDEO_HEVC;
        }
        else if (!next.audio_pid && type == 0x0fu)
        {
            next.audio_pid = pid;
            next.audio_stream_type = type;
        }
        at += info;
    }
    if (at != end || !next.video_pid || next.video_pid == kNullPid ||
        (next.audio_pid != 0 && (next.video_pid == next.audio_pid || next.audio_pid == kNullPid)))
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT,
                    "PMT requires H.264 or HEVC video with optional AAC ADTS audio");
    next.video_bit_depth = 8;
    next.video_chroma_format = IPTV_STREAM_CHROMA_420;

    const bool same = impl->pmt_seen && same_program_format(&impl->format, &next);
    if (impl->pmt_seen && !same)
    {
        if (impl->backend_ever_opened)
            return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT,
                        "PMT format changed after native backend open");
        impl->video_sps = false;
        impl->video_pps = false;
        impl->video_vps = false;
        impl->video_es.size = 0;
        impl->audio_es.size = 0;
        marker_clear(&impl->video_markers);
        marker_clear(&impl->audio_markers);
        impl->video_pes = {};
        impl->audio_pes = {};
    }
    if (same)
    {
        next.coded_width = impl->format.coded_width;
        next.coded_height = impl->format.coded_height;
        next.visible_width = impl->format.visible_width;
        next.visible_height = impl->format.visible_height;
        next.video_profile = impl->format.video_profile;
        next.video_level = impl->format.video_level;
        next.audio_sample_rate = impl->format.audio_sample_rate;
        next.audio_channels = impl->format.audio_channels;
    }
    impl->format = next;
    impl->pmt_seen = true;
    session->telemetry.format = impl->format;
    ++session->telemetry.pmt_sections;
    return IPTV_STREAM_OK;
}

static int process_section(iptv_stream_session_t *session, impl_t *impl, bool pat,
                           const uint8_t *section, size_t bytes)
{
    return pat ? parse_pat_section(session, impl, section, bytes)
               : parse_pmt_section(session, impl, section, bytes);
}

static int psi_bytes(iptv_stream_session_t *session, impl_t *impl, psi_t *psi, bool pat,
                     const uint8_t *data, size_t bytes)
{
    while (bytes)
    {
        if (!psi->active)
        {
            if (*data == 0xffu)
                return IPTV_STREAM_OK;
            psi->active = true;
            psi->size = 0;
            psi->expected = 0;
        }
        size_t need =
            psi->expected ? psi->expected - psi->size : (psi->size < 3u ? 3u - psi->size : 0u);
        if (!need && !psi->expected)
        {
            const size_t section_length =
                (static_cast<size_t>(psi->data[1] & 0x0fu) << 8) | psi->data[2];
            psi->expected = 3u + section_length;
            if (section_length < 4u || psi->expected > kPsiBytes)
            {
                psi->active = false;
                return fail(session, IPTV_STREAM_MALFORMED_TS, "PSI section length exceeds bounds");
            }
            need = psi->expected - psi->size;
        }
        const size_t copy = bytes < need ? bytes : need;
        std::memcpy(psi->data + psi->size, data, copy);
        psi->size += copy;
        data += copy;
        bytes -= copy;
        if (!psi->expected && psi->size == 3u)
            continue;
        if (psi->expected && psi->size == psi->expected)
        {
            const int result = process_section(session, impl, pat, psi->data, psi->size);
            psi->active = false;
            psi->size = 0;
            psi->expected = 0;
            if (result != IPTV_STREAM_OK)
                return result;
        }
    }
    return IPTV_STREAM_OK;
}

static int feed_psi(iptv_stream_session_t *session, impl_t *impl, psi_t *psi, bool pat,
                    const uint8_t *payload, size_t bytes, bool pusi)
{
    if (pusi)
    {
        if (!bytes)
            return fail(session, IPTV_STREAM_MALFORMED_TS,
                        "PSI payload start has no pointer field");
        const size_t pointer = payload[0];
        ++payload;
        --bytes;
        if (pointer > bytes)
            return fail(session, IPTV_STREAM_MALFORMED_TS, "PSI pointer exceeds payload");
        if (psi->active && pointer)
        {
            const int result = psi_bytes(session, impl, psi, pat, payload, pointer);
            if (result != IPTV_STREAM_OK)
                return result;
        }
        else if (psi->active)
        {
            ++session->telemetry.dropped_payloads;
            psi->active = false;
            psi->size = 0;
            psi->expected = 0;
        }
        payload += pointer;
        bytes -= pointer;
        return psi_bytes(session, impl, psi, pat, payload, bytes);
    }
    if (!psi->active)
        return IPTV_STREAM_OK;
    return psi_bytes(session, impl, psi, pat, payload, bytes);
}

static size_t find_start_code(const uint8_t *data, size_t bytes, size_t from, size_t *prefix_bytes)
{
    for (size_t i = from; i + 3u <= bytes; ++i)
    {
        if (data[i] != 0 || data[i + 1u] != 0)
            continue;
        if (data[i + 2u] == 1u)
        {
            *prefix_bytes = 3u;
            return i;
        }
        if (i + 4u <= bytes && data[i + 2u] == 0 && data[i + 3u] == 1u)
        {
            *prefix_bytes = 4u;
            return i;
        }
    }
    return kNoOffset;
}

static bool h264_first_slice(const uint8_t *nal, size_t bytes)
{
    uint8_t rbsp[64];
    if (bytes < 2u)
        return false;
    const size_t count = make_rbsp(nal + 1u, bytes - 1u, rbsp, sizeof(rbsp));
    bit_reader_t bits{rbsp, count * 8u, 0};
    uint32_t first_mb = 1;
    return count && read_ue(&bits, &first_mb) && first_mb == 0u;
}

static bool hevc_first_slice(const uint8_t *nal, size_t bytes)
{
    return bytes >= 3u && (nal[2] & 0x80u) != 0;
}

static int inspect_video_nal(iptv_stream_session_t *session, impl_t *impl, const uint8_t *nal,
                             size_t bytes, bool *vcl, bool *first, bool *aud, bool *prefix)
{
    *vcl = false;
    *first = false;
    *aud = false;
    *prefix = false;
    if (impl->format.video_codec == IPTV_STREAM_VIDEO_H264)
    {
        if (!bytes || (nal[0] & 0x80u))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid H.264 NAL");
        const uint8_t type = nal[0] & 0x1fu;
        *vcl = type >= 1u && type <= 5u;
        *first = *vcl && h264_first_slice(nal, bytes);
        *aud = type == 9u;
        *prefix = type == 6u || type == 7u || type == 8u;
        if (type == 7u)
            return parse_h264_sps(session, impl, nal, bytes);
        if (type == 8u)
            impl->video_pps = true;
        return IPTV_STREAM_OK;
    }
    if (impl->format.video_codec == IPTV_STREAM_VIDEO_HEVC)
    {
        if (bytes < 2u || (nal[0] & 0x80u) || (nal[1] & 7u) == 0)
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid HEVC NAL");
        const uint8_t type = (nal[0] >> 1) & 0x3fu;
        *vcl = type <= 31u;
        *first = *vcl && hevc_first_slice(nal, bytes);
        *aud = type == 35u;
        *prefix = type == 32u || type == 33u || type == 34u || type == 39u;
        if (type == 32u)
            impl->video_vps = true;
        else if (type == 33u)
            return parse_hevc_sps(session, impl, nal, bytes);
        else if (type == 34u)
            impl->video_pps = true;
        return IPTV_STREAM_OK;
    }
    return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT, "video codec is not selected");
}

static int process_audio(iptv_stream_session_t *session, impl_t *impl);

static int emit_video(iptv_stream_session_t *session, impl_t *impl, size_t bytes)
{
    if (!bytes || bytes > impl->video_es.size)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid video access-unit boundary");
    if (!video_config_ready(impl))
    {
        ++session->telemetry.dropped_payloads;
        marker_erase(&impl->video_markers, bytes, IPTV_STREAM_PTS_UNKNOWN);
        buffer_erase(&impl->video_es, bytes);
        return IPTV_STREAM_OK;
    }
    int result = maybe_activate(session, impl);
    if (result != IPTV_STREAM_OK)
        return result;
    if (impl->backend_open)
    {
        result = impl->backend.submit_video(impl->backend.context, impl->video_es.data, bytes,
                                            impl->video_markers.base_pts);
        if (result != 0)
        {
            ++session->telemetry.video_submit_errors;
            return fail(session, IPTV_STREAM_NATIVE_ERROR, "native video submit failed");
        }
    }
    ++session->telemetry.video_access_units;
    session->telemetry.video_bytes += bytes;
    session->telemetry.last_video_pts_us = impl->video_markers.base_pts;
    marker_erase(&impl->video_markers, bytes, IPTV_STREAM_PTS_UNKNOWN);
    buffer_erase(&impl->video_es, bytes);
    update_buffered(session, impl);
    if (impl->backend_open)
        return process_audio(session, impl);
    return IPTV_STREAM_OK;
}

static int process_video(iptv_stream_session_t *session, impl_t *impl, bool flush)
{
    for (;;)
    {
        size_t first_prefix = 0;
        size_t first = find_start_code(impl->video_es.data, impl->video_es.size, 0, &first_prefix);
        if (first == kNoOffset)
        {
            if (flush && impl->video_es.size)
            {
                ++session->telemetry.dropped_payloads;
                impl->video_es.size = 0;
                marker_clear(&impl->video_markers);
            }
            else if (impl->video_es.size > 4u)
            {
                const size_t drop = impl->video_es.size - 4u;
                ++session->telemetry.dropped_payloads;
                buffer_erase(&impl->video_es, drop);
                marker_erase(&impl->video_markers, drop, IPTV_STREAM_PTS_UNKNOWN);
            }
            return IPTV_STREAM_OK;
        }
        if (first)
        {
            ++session->telemetry.dropped_payloads;
            buffer_erase(&impl->video_es, first);
            marker_erase(&impl->video_markers, first, IPTV_STREAM_PTS_UNKNOWN);
            continue;
        }

        bool seen_vcl = false;
        size_t pending_prefix = kNoOffset;
        size_t at = 0;
        bool need_more = false;
        while (at < impl->video_es.size)
        {
            size_t prefix_bytes = 0;
            const size_t start =
                find_start_code(impl->video_es.data, impl->video_es.size, at, &prefix_bytes);
            if (start == kNoOffset)
                break;
            size_t next_prefix = 0;
            size_t next = find_start_code(impl->video_es.data, impl->video_es.size,
                                          start + prefix_bytes, &next_prefix);
            if (next == kNoOffset && !flush)
            {
                need_more = true;
                break;
            }
            if (next == kNoOffset)
                next = impl->video_es.size;
            const size_t nal_at = start + prefix_bytes;
            if (nal_at >= next)
                return fail(session, IPTV_STREAM_MALFORMED_TS, "empty Annex-B NAL unit");

            bool vcl = false, first_slice = false, aud = false, prefix = false;
            const int inspect = inspect_video_nal(session, impl, impl->video_es.data + nal_at,
                                                  next - nal_at, &vcl, &first_slice, &aud, &prefix);
            if (inspect != IPTV_STREAM_OK)
                return inspect;

            size_t boundary = kNoOffset;
            if (seen_vcl && aud)
                boundary = start;
            else if (seen_vcl && vcl && first_slice)
                boundary = pending_prefix != kNoOffset ? pending_prefix : start;
            if (boundary != kNoOffset)
            {
                const int result = emit_video(session, impl, boundary);
                if (result != IPTV_STREAM_OK)
                    return result;
                break;
            }
            if (seen_vcl && prefix && pending_prefix == kNoOffset)
                pending_prefix = start;
            if (vcl)
                seen_vcl = true;
            at = next;
            if (at == impl->video_es.size)
            {
                if (flush && seen_vcl)
                {
                    const size_t emit =
                        pending_prefix != kNoOffset ? pending_prefix : impl->video_es.size;
                    const int result = emit_video(session, impl, emit);
                    if (result != IPTV_STREAM_OK)
                        return result;
                    if (emit == 0)
                        return IPTV_STREAM_OK;
                }
                break;
            }
        }
        if (need_more || !flush)
            return IPTV_STREAM_OK;
        if (!impl->video_es.size)
            return IPTV_STREAM_OK;
        if (!seen_vcl)
        {
            ++session->telemetry.dropped_payloads;
            impl->video_es.size = 0;
            marker_clear(&impl->video_markers);
            return IPTV_STREAM_OK;
        }
    }
}

static const uint32_t kAdtsRates[16] = {96000u, 88200u, 64000u, 48000u, 44100u, 32000u,
                                        24000u, 22050u, 16000u, 12000u, 11025u, 8000u,
                                        7350u,  0u,     0u,     0u};

static bool adts_sync(const uint8_t *data, size_t bytes)
{
    return bytes >= 2u && data[0] == 0xffu && (data[1] & 0xf6u) == 0xf0u;
}

static int process_audio(iptv_stream_session_t *session, impl_t *impl)
{
    while (impl->audio_es.size >= 2u)
    {
        if (!adts_sync(impl->audio_es.data, impl->audio_es.size))
        {
            size_t next = 1u;
            while (next + 1u < impl->audio_es.size &&
                   !adts_sync(impl->audio_es.data + next, impl->audio_es.size - next))
                ++next;
            if (next + 1u >= impl->audio_es.size)
                next = impl->audio_es.size - 1u;
            ++session->telemetry.dropped_payloads;
            buffer_erase(&impl->audio_es, next);
            marker_erase(&impl->audio_markers, next, IPTV_STREAM_PTS_UNKNOWN);
            continue;
        }
        if (impl->audio_es.size < 7u)
            return IPTV_STREAM_OK;
        const uint8_t *data = impl->audio_es.data;
        const uint32_t object_type = (data[2] >> 6) + 1u;
        const uint32_t rate_index = (data[2] >> 2) & 0x0fu;
        const uint32_t channels = ((data[2] & 1u) << 2) | (data[3] >> 6);
        const size_t header = (data[1] & 1u) ? 7u : 9u;
        const size_t frame_bytes = (static_cast<size_t>(data[3] & 3u) << 11) |
                                   (static_cast<size_t>(data[4]) << 3) | (data[5] >> 5);
        if (object_type != 2u || !kAdtsRates[rate_index] || channels < 1u || channels > 2u ||
            (data[6] & 3u) != 0u)
            return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT,
                        "AAC must be LC with one raw block and one or two channels");
        if (frame_bytes < header || frame_bytes > impl->audio_es.capacity)
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid ADTS frame length");
        if (frame_bytes > impl->audio_es.size)
            return IPTV_STREAM_OK;

        if (impl->format.audio_sample_rate &&
            (impl->format.audio_sample_rate != kAdtsRates[rate_index] ||
             impl->format.audio_channels != channels))
            return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT,
                        "AAC format changed during stream");
        impl->format.audio_sample_rate = kAdtsRates[rate_index];
        impl->format.audio_channels = channels;
        session->telemetry.format = impl->format;

        /* Keep complete frames bounded until video parameter sets permit the
         * backend to open. Parser-only sessions consume them immediately. */
        if (impl->has_backend && !impl->backend_open)
            return IPTV_STREAM_OK;

        const uint64_t pts = impl->audio_markers.base_pts;
        if (impl->backend_open)
        {
            const int result =
                impl->backend.submit_audio(impl->backend.context, data, frame_bytes, pts);
            if (result != 0)
            {
                ++session->telemetry.audio_submit_errors;
                return fail(session, IPTV_STREAM_NATIVE_ERROR, "native audio submit failed");
            }
        }
        ++session->telemetry.audio_frames;
        session->telemetry.audio_bytes += frame_bytes;
        session->telemetry.last_audio_pts_us = pts;
        const uint64_t next_pts = pts == IPTV_STREAM_PTS_UNKNOWN
                                      ? IPTV_STREAM_PTS_UNKNOWN
                                      : pts + UINT64_C(1024000000) / kAdtsRates[rate_index];
        buffer_erase(&impl->audio_es, frame_bytes);
        marker_erase(&impl->audio_markers, frame_bytes, next_pts);
        update_buffered(session, impl);
    }
    return IPTV_STREAM_OK;
}

static int append_es(iptv_stream_session_t *session, impl_t *impl, bool video, pes_t *pes,
                     const uint8_t *data, size_t bytes)
{
    buffer_t *buffer = video ? &impl->video_es : &impl->audio_es;
    marker_list_t *markers = video ? &impl->video_markers : &impl->audio_markers;
    if (!pes->marker_added)
    {
        if (!marker_add(markers, buffer->size, pes->pts_us))
        {
            ++session->telemetry.buffer_errors;
            return fail(session, IPTV_STREAM_BUFFER_LIMIT,
                        "too many PTS markers in one access unit");
        }
        pes->marker_added = true;
    }
    if (!buffer_append(buffer, data, bytes))
    {
        ++session->telemetry.buffer_errors;
        return fail(session, IPTV_STREAM_BUFFER_LIMIT,
                    video ? "video access unit exceeds buffer limit"
                          : "AAC assembly exceeds buffer limit");
    }
    update_buffered(session, impl);
    return video ? process_video(session, impl, false) : process_audio(session, impl);
}

static void reset_pes(pes_t *pes, bool video)
{
    *pes = {};
    pes->video = video;
    pes->pts_us = IPTV_STREAM_PTS_UNKNOWN;
}

static int complete_pes(iptv_stream_session_t *session, pes_t *pes)
{
    if (pes->video)
        ++session->telemetry.video_pes;
    else
        ++session->telemetry.audio_pes;
    reset_pes(pes, pes->video);
    return IPTV_STREAM_OK;
}

static int parse_pes_header(iptv_stream_session_t *session, impl_t *impl, pes_t *pes)
{
    const uint8_t *data = pes->header;
    if (pes->header_size < 9u || data[0] != 0 || data[1] != 0 || data[2] != 1u ||
        (data[6] & 0xc0u) != 0x80u)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid PES header");
    if ((pes->video && (data[3] & 0xf0u) != 0xe0u) || (!pes->video && (data[3] & 0xe0u) != 0xc0u))
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT, "PES stream id does not match PMT");
    const uint32_t packet_length = static_cast<uint32_t>(data[4] << 8 | data[5]);
    if (!packet_length && !pes->video)
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT,
                    "zero-length audio PES is unsupported");
    if (packet_length && 6u + packet_length < pes->header_size)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "PES header exceeds declared packet length");
    pes->unbounded = packet_length == 0;
    pes->expected_payload = pes->unbounded ? UINT64_MAX : 6u + packet_length - pes->header_size;
    if (!pes->unbounded && pes->expected_payload > impl->config.max_pes_bytes)
        return fail(session, IPTV_STREAM_BUFFER_LIMIT, "PES payload exceeds configured limit");

    const uint8_t pts_flags = (data[7] >> 6) & 3u;
    if (pts_flags == 1u)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid PES PTS flags");
    if (pts_flags >= 2u)
    {
        if (data[8] < 5u)
            return fail(session, IPTV_STREAM_MALFORMED_TS, "truncated PES PTS");
        uint64_t raw = 0;
        if (!parse_pts_raw(data + 9u, &raw))
            return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid PES PTS");
        pes->pts_us = extend_pts(session, pes->video ? &impl->video_time : &impl->audio_time, raw);
    }
    pes->header_complete = true;
    return IPTV_STREAM_OK;
}

static int consume_pes(iptv_stream_session_t *session, impl_t *impl, pes_t *pes,
                       const uint8_t *data, size_t bytes)
{
    while (bytes)
    {
        if (!pes->header_complete)
        {
            size_t wanted = pes->header_size ? pes->header_size : 9u;
            const size_t copy =
                bytes < wanted - pes->header_bytes ? bytes : wanted - pes->header_bytes;
            std::memcpy(pes->header + pes->header_bytes, data, copy);
            pes->header_bytes += copy;
            data += copy;
            bytes -= copy;
            if (pes->header_bytes < wanted)
                return IPTV_STREAM_OK;
            if (!pes->header_size)
            {
                pes->header_size = 9u + pes->header[8];
                if (pes->header_size > kPesHeaderBytes)
                    return fail(session, IPTV_STREAM_MALFORMED_TS,
                                "PES optional header exceeds bounds");
                if (pes->header_bytes < pes->header_size)
                    continue;
            }
            const int parsed = parse_pes_header(session, impl, pes);
            if (parsed != IPTV_STREAM_OK)
                return parsed;
            if (!pes->unbounded && pes->expected_payload == 0u)
                return complete_pes(session, pes);
        }

        size_t take = bytes;
        if (!pes->unbounded)
        {
            const uint64_t remaining = pes->expected_payload - pes->payload_bytes;
            if (take > remaining)
                take = static_cast<size_t>(remaining);
        }
        if (pes->payload_bytes + take > impl->config.max_pes_bytes)
            return fail(session, IPTV_STREAM_BUFFER_LIMIT,
                        "live PES payload exceeds configured limit");
        const int result = append_es(session, impl, pes->video, pes, data, take);
        if (result != IPTV_STREAM_OK)
            return result;
        pes->payload_bytes += take;
        data += take;
        bytes -= take;
        if (!pes->unbounded && pes->payload_bytes == pes->expected_payload)
        {
            const int completed = complete_pes(session, pes);
            if (completed != IPTV_STREAM_OK)
                return completed;
            while (bytes && *data == 0xffu)
            {
                ++data;
                --bytes;
            }
            if (bytes)
                return fail(session, IPTV_STREAM_MALFORMED_TS,
                            "bytes follow completed PES without a payload start");
        }
    }
    return IPTV_STREAM_OK;
}

static int feed_pes(iptv_stream_session_t *session, impl_t *impl, pes_t *pes,
                    const uint8_t *payload, size_t bytes, bool pusi, bool video)
{
    if (pusi)
    {
        if (pes->active)
        {
            if (pes->unbounded && pes->video)
                complete_pes(session, pes);
            else
            {
                ++session->telemetry.dropped_payloads;
                reset_pes(pes, video);
            }
        }
        reset_pes(pes, video);
        pes->active = true;
    }
    else if (!pes->active)
    {
        ++session->telemetry.dropped_payloads;
        return IPTV_STREAM_OK;
    }
    return consume_pes(session, impl, pes, payload, bytes);
}

static continuity_t *continuity_for(impl_t *impl, uint16_t pid)
{
    for (size_t i = 0; i < kContinuityEntries; ++i)
        if (impl->continuity[i].seen && impl->continuity[i].pid == pid)
            return &impl->continuity[i];
    for (size_t i = 0; i < kContinuityEntries; ++i)
        if (!impl->continuity[i].seen)
        {
            impl->continuity[i].pid = pid;
            return &impl->continuity[i];
        }
    return nullptr;
}

static void reset_pid(impl_t *impl, uint16_t pid)
{
    if (pid == kPatPid)
        impl->pat = {};
    else if (impl->pat_seen && pid == impl->format.pmt_pid)
        impl->pmt = {};
    else if (impl->pmt_seen && pid == impl->format.video_pid)
    {
        reset_pes(&impl->video_pes, true);
        impl->video_es.size = 0;
        marker_clear(&impl->video_markers);
        impl->video_time = {};
    }
    else if (impl->pmt_seen && pid == impl->format.audio_pid)
    {
        reset_pes(&impl->audio_pes, false);
        impl->audio_es.size = 0;
        marker_clear(&impl->audio_markers);
        impl->audio_time = {};
    }
}

/* Returns 1 to consume, 0 for a duplicate, and -1 for a recovered gap. */
static int continuity_check(iptv_stream_session_t *session, impl_t *impl, uint16_t pid,
                            uint8_t counter, bool discontinuity)
{
    continuity_t *entry = continuity_for(impl, pid);
    if (!entry)
        return 1;
    if (discontinuity)
    {
        ++session->telemetry.discontinuities;
        reset_pid(impl, pid);
        entry->seen = true;
        entry->last = counter;
        return 1;
    }
    if (entry->seen)
    {
        if (counter == entry->last)
        {
            ++session->telemetry.duplicate_packets;
            return 0;
        }
        if (counter != static_cast<uint8_t>((entry->last + 1u) & 0x0fu))
        {
            ++session->telemetry.continuity_errors;
            ++session->telemetry.dropped_payloads;
            reset_pid(impl, pid);
            entry->last = counter;
            return -1;
        }
    }
    entry->seen = true;
    entry->last = counter;
    return 1;
}

static bool relevant_pid(const impl_t *impl, uint16_t pid)
{
    return pid == kPatPid || (impl->pat_seen && pid == impl->format.pmt_pid) ||
           (impl->pmt_seen && (pid == impl->format.video_pid || pid == impl->format.audio_pid));
}

static int process_packet(iptv_stream_session_t *session, impl_t *impl, const uint8_t *packet)
{
    if (packet[0] != 0x47u || (packet[3] & 0xc0u) != 0)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "invalid MPEG-TS packet header");
    const bool pusi = (packet[1] & 0x40u) != 0;
    const bool transport_error = (packet[1] & 0x80u) != 0;
    const uint16_t pid = static_cast<uint16_t>((packet[1] & 0x1fu) << 8 | packet[2]);
    const uint8_t adaptation_control = (packet[3] >> 4) & 3u;
    if (!adaptation_control)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "reserved TS adaptation mode");
    const bool has_payload = adaptation_control == 1u || adaptation_control == 3u;
    size_t payload_at = 4u;
    bool discontinuity = false;
    if (adaptation_control == 2u || adaptation_control == 3u)
    {
        const size_t adaptation = packet[4];
        if (adaptation > 183u || 5u + adaptation > IPTV_STREAM_TS_PACKET_BYTES)
            return fail(session, IPTV_STREAM_MALFORMED_TS, "TS adaptation field exceeds packet");
        if (adaptation)
            discontinuity = (packet[5] & 0x80u) != 0;
        payload_at += 1u + adaptation;
    }
    if (payload_at > IPTV_STREAM_TS_PACKET_BYTES)
        return fail(session, IPTV_STREAM_MALFORMED_TS, "TS payload offset exceeds packet");

    ++session->telemetry.packets;
    if (pid == kNullPid || !relevant_pid(impl, pid))
        return IPTV_STREAM_OK;
    if ((packet[3] & 0xc0u) != 0)
        return fail(session, IPTV_STREAM_UNSUPPORTED_FORMAT, "scrambled MPEG-TS is unsupported");
    if (transport_error)
    {
        ++session->telemetry.continuity_errors;
        ++session->telemetry.dropped_payloads;
        reset_pid(impl, pid);
        return IPTV_STREAM_OK;
    }
    if (!has_payload || payload_at == IPTV_STREAM_TS_PACKET_BYTES)
    {
        if (discontinuity)
        {
            ++session->telemetry.discontinuities;
            reset_pid(impl, pid);
            continuity_t *entry = continuity_for(impl, pid);
            if (entry)
                entry->seen = false;
        }
        return IPTV_STREAM_OK;
    }

    const int continuity = continuity_check(session, impl, pid, packet[3] & 0x0fu, discontinuity);
    if (continuity == 0)
        return IPTV_STREAM_OK;
    if (continuity < 0 && !pusi)
        return IPTV_STREAM_OK;

    const uint8_t *payload = packet + payload_at;
    const size_t bytes = IPTV_STREAM_TS_PACKET_BYTES - payload_at;
    if (pid == kPatPid)
        return feed_psi(session, impl, &impl->pat, true, payload, bytes, pusi);
    if (impl->pat_seen && pid == impl->format.pmt_pid)
        return feed_psi(session, impl, &impl->pmt, false, payload, bytes, pusi);
    if (impl->pmt_seen && pid == impl->format.video_pid)
        return feed_pes(session, impl, &impl->video_pes, payload, bytes, pusi, true);
    if (impl->pmt_seen && pid == impl->format.audio_pid)
        return feed_pes(session, impl, &impl->audio_pes, payload, bytes, pusi, false);
    return IPTV_STREAM_OK;
}

static void discard_packet_bytes(impl_t *impl, size_t bytes)
{
    if (bytes < impl->packet_bytes)
        std::memmove(impl->packet, impl->packet + bytes, impl->packet_bytes - bytes);
    impl->packet_bytes -= bytes;
}

static bool acquire_packet_sync(impl_t *impl)
{
    const size_t span = (kSyncPackets - 1u) * IPTV_STREAM_TS_PACKET_BYTES;
    for (size_t at = 0; at + span < impl->packet_bytes; ++at)
    {
        bool match = true;
        for (size_t packet = 0; packet < kSyncPackets; ++packet)
        {
            if (impl->packet[at + packet * IPTV_STREAM_TS_PACKET_BYTES] != 0x47u)
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            discard_packet_bytes(impl, at);
            impl->packet_sync = true;
            return true;
        }
    }

    if (impl->packet_bytes > span)
        discard_packet_bytes(impl, impl->packet_bytes - span);
    return false;
}

static int process_packet_buffer(iptv_stream_session_t *session, impl_t *impl, bool final)
{
    for (;;)
    {
        if (!impl->packet_sync && !acquire_packet_sync(impl))
        {
            if (final && impl->packet_bytes)
                return fail(session, IPTV_STREAM_MALFORMED_TS,
                            "unable to acquire MPEG-TS packet sync");
            return IPTV_STREAM_OK;
        }
        if (impl->packet_bytes < IPTV_STREAM_TS_PACKET_BYTES)
            return IPTV_STREAM_OK;
        if (impl->packet[0] != 0x47u)
        {
            impl->packet_sync = false;
            continue;
        }

        const int result = process_packet(session, impl, impl->packet);
        discard_packet_bytes(impl, IPTV_STREAM_TS_PACKET_BYTES);
        update_buffered(session, impl);
        if (result != IPTV_STREAM_OK)
            return result;
    }
}

static int flush_streams(iptv_stream_session_t *session, impl_t *impl)
{
    if (impl->video_pes.active)
    {
        if (impl->video_pes.unbounded)
            complete_pes(session, &impl->video_pes);
        else
        {
            ++session->telemetry.dropped_payloads;
            reset_pes(&impl->video_pes, true);
        }
    }
    if (impl->audio_pes.active)
    {
        ++session->telemetry.dropped_payloads;
        reset_pes(&impl->audio_pes, false);
    }
    int result = process_video(session, impl, true);
    if (result != IPTV_STREAM_OK)
        return result;
    if (impl->audio_es.size)
    {
        result = process_audio(session, impl);
        if (result != IPTV_STREAM_OK)
            return result;
        if (impl->audio_es.size)
        {
            ++session->telemetry.dropped_payloads;
            impl->audio_es.size = 0;
            marker_clear(&impl->audio_markers);
        }
    }
    return IPTV_STREAM_OK;
}

} // namespace

void iptv_stream_init(iptv_stream_session_t *session)
{
    if (!session)
        return;
    std::memset(session, 0, sizeof(*session));
    session->_magic = kMagic;
    session->telemetry.state = IPTV_STREAM_STATE_IDLE;
    session->telemetry.last_video_pts_us = IPTV_STREAM_PTS_UNKNOWN;
    session->telemetry.last_audio_pts_us = IPTV_STREAM_PTS_UNKNOWN;
}

int iptv_stream_open(iptv_stream_session_t *session, const iptv_stream_config_t *config,
                     const iptv_stream_backend_t *backend)
{
    if (!valid_session(session))
        return IPTV_STREAM_INVALID_ARGUMENT;
    if (session->_impl || (session->telemetry.state != IPTV_STREAM_STATE_IDLE &&
                           session->telemetry.state != IPTV_STREAM_STATE_STOPPED))
        return IPTV_STREAM_INVALID_STATE;
    if (backend && (!backend->open || !backend->submit_video || !backend->submit_audio ||
                    !backend->drain || !backend->close))
        return IPTV_STREAM_INVALID_ARGUMENT;

    impl_t *impl = new (std::nothrow) impl_t{};
    if (!impl)
        return IPTV_STREAM_BUFFER_LIMIT;
    impl->config.max_pes_bytes =
        config && config->max_pes_bytes ? config->max_pes_bytes : IPTV_STREAM_DEFAULT_MAX_PES_BYTES;
    if (impl->config.max_pes_bytes < 256u)
    {
        delete impl;
        return IPTV_STREAM_INVALID_ARGUMENT;
    }
    if (!buffer_init(&impl->video_es, impl->config.max_pes_bytes))
    {
        delete impl;
        return IPTV_STREAM_BUFFER_LIMIT;
    }
    if (!buffer_init(&impl->audio_es, impl->config.max_pes_bytes))
    {
        buffer_release(&impl->video_es);
        delete impl;
        return IPTV_STREAM_BUFFER_LIMIT;
    }
    marker_clear(&impl->video_markers);
    marker_clear(&impl->audio_markers);
    reset_pes(&impl->video_pes, true);
    reset_pes(&impl->audio_pes, false);
    if (backend)
    {
        impl->backend = *backend;
        impl->has_backend = true;
    }
    session->_impl = impl;
    session->telemetry = {};
    session->telemetry.state = IPTV_STREAM_STATE_OPEN;
    session->telemetry.last_result = IPTV_STREAM_OK;
    session->telemetry.max_pes_bytes = impl->config.max_pes_bytes;
    session->telemetry.last_video_pts_us = IPTV_STREAM_PTS_UNKNOWN;
    session->telemetry.last_audio_pts_us = IPTV_STREAM_PTS_UNKNOWN;
    return IPTV_STREAM_OK;
}

int iptv_stream_start(iptv_stream_session_t *session)
{
    if (!valid_session(session) || !get_impl(session))
        return IPTV_STREAM_INVALID_ARGUMENT;
    impl_t *impl = get_impl(session);
    if (session->telemetry.state != IPTV_STREAM_STATE_OPEN &&
        session->telemetry.state != IPTV_STREAM_STATE_READY)
        return IPTV_STREAM_INVALID_STATE;
    impl->started = true;
    session->telemetry.state = IPTV_STREAM_STATE_BUFFERING;
    return maybe_activate(session, impl);
}

int iptv_stream_push(iptv_stream_session_t *session, const void *data, size_t bytes)
{
    if (!valid_session(session) || !get_impl(session) || (!data && bytes))
        return IPTV_STREAM_INVALID_ARGUMENT;
    impl_t *impl = get_impl(session);
    if (session->telemetry.state != IPTV_STREAM_STATE_OPEN &&
        session->telemetry.state != IPTV_STREAM_STATE_BUFFERING &&
        session->telemetry.state != IPTV_STREAM_STATE_READY &&
        session->telemetry.state != IPTV_STREAM_STATE_PLAYING)
        return IPTV_STREAM_INVALID_STATE;

    const uint8_t *input = static_cast<const uint8_t *>(data);
    while (bytes)
    {
        const size_t room = kPacketBufferBytes - impl->packet_bytes;
        const size_t copy = bytes < room ? bytes : room;
        std::memcpy(impl->packet + impl->packet_bytes, input, copy);
        impl->packet_bytes += copy;
        input += copy;
        bytes -= copy;
        const int result = process_packet_buffer(session, impl, false);
        if (result != IPTV_STREAM_OK)
            return result;
    }
    return IPTV_STREAM_OK;
}

int iptv_stream_stop(iptv_stream_session_t *session)
{
    if (!valid_session(session) || !get_impl(session))
        return IPTV_STREAM_INVALID_ARGUMENT;
    impl_t *impl = get_impl(session);
    if (session->telemetry.state == IPTV_STREAM_STATE_STOPPED)
        return IPTV_STREAM_OK;
    if (session->telemetry.state != IPTV_STREAM_STATE_OPEN &&
        session->telemetry.state != IPTV_STREAM_STATE_BUFFERING &&
        session->telemetry.state != IPTV_STREAM_STATE_READY &&
        session->telemetry.state != IPTV_STREAM_STATE_PLAYING &&
        session->telemetry.state != IPTV_STREAM_STATE_ERROR)
        return IPTV_STREAM_INVALID_STATE;

    int result = IPTV_STREAM_OK;
    if (session->telemetry.state != IPTV_STREAM_STATE_ERROR)
    {
        result = process_packet_buffer(session, impl, true);
        if (result == IPTV_STREAM_OK)
            result = flush_streams(session, impl);
    }
    if (impl->backend_open)
    {
        const int drain = impl->backend.drain(impl->backend.context);
        if (drain != 0 && result == IPTV_STREAM_OK)
        {
            result = IPTV_STREAM_NATIVE_ERROR;
            fail(session, result, "native backend drain failed");
        }
        impl->backend.close(impl->backend.context);
        impl->backend_open = false;
        session->telemetry.backend_open = 0;
    }
    ++session->telemetry.stop_count;
    session->telemetry.state =
        result == IPTV_STREAM_OK ? IPTV_STREAM_STATE_STOPPED : IPTV_STREAM_STATE_ERROR;
    return result;
}

int iptv_stream_cleanup(iptv_stream_session_t *session)
{
    if (!valid_session(session) || !get_impl(session))
        return IPTV_STREAM_INVALID_ARGUMENT;
    const int stop_result = iptv_stream_stop(session);
    impl_t *impl = get_impl(session);
    if (impl->backend_open)
    {
        impl->backend.close(impl->backend.context);
        impl->backend_open = false;
    }
    buffer_release(&impl->video_es);
    buffer_release(&impl->audio_es);
    delete impl;
    session->_impl = nullptr;
    ++session->telemetry.cleanup_count;
    session->telemetry.last_cleanup_result = stop_result;
    session->telemetry.buffered_bytes = 0;
    if (stop_result == IPTV_STREAM_OK)
        session->telemetry.state = IPTV_STREAM_STATE_STOPPED;
    return stop_result;
}

iptv_stream_state_t iptv_stream_state(const iptv_stream_session_t *session)
{
    return valid_session(session) ? session->telemetry.state : IPTV_STREAM_STATE_ERROR;
}

const iptv_stream_telemetry_t *iptv_stream_telemetry(const iptv_stream_session_t *session)
{
    return valid_session(session) ? &session->telemetry : nullptr;
}

const char *iptv_stream_state_name(iptv_stream_state_t state)
{
    switch (state)
    {
    case IPTV_STREAM_STATE_IDLE:
        return "idle";
    case IPTV_STREAM_STATE_OPEN:
        return "open";
    case IPTV_STREAM_STATE_BUFFERING:
        return "buffering";
    case IPTV_STREAM_STATE_READY:
        return "ready";
    case IPTV_STREAM_STATE_PLAYING:
        return "playing";
    case IPTV_STREAM_STATE_STOPPED:
        return "stopped";
    case IPTV_STREAM_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
