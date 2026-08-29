/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_webm.h"

#include <cstring>
#include <new>

namespace
{

constexpr uint32_t kMagic = UINT32_C(0x5745424d);
constexpr uint64_t kDefaultTimestampScaleNs = UINT64_C(1000000);
constexpr uint64_t kMaxTimestampScaleNs = UINT64_C(1000000000);
constexpr size_t kMaxCodecPrivateBytes = 64u;

constexpr uint32_t kEbml = UINT32_C(0x1a45dfa3);
constexpr uint32_t kDocType = UINT32_C(0x4282);
constexpr uint32_t kSegment = UINT32_C(0x18538067);
constexpr uint32_t kInfo = UINT32_C(0x1549a966);
constexpr uint32_t kTimestampScale = UINT32_C(0x2ad7b1);
constexpr uint32_t kTracks = UINT32_C(0x1654ae6b);
constexpr uint32_t kTrackEntry = UINT32_C(0xae);
constexpr uint32_t kTrackNumber = UINT32_C(0xd7);
constexpr uint32_t kTrackType = UINT32_C(0x83);
constexpr uint32_t kCodecId = UINT32_C(0x86);
constexpr uint32_t kCodecPrivate = UINT32_C(0x63a2);
constexpr uint32_t kVideo = UINT32_C(0xe0);
constexpr uint32_t kPixelWidth = UINT32_C(0xb0);
constexpr uint32_t kPixelHeight = UINT32_C(0xba);
constexpr uint32_t kDisplayWidth = UINT32_C(0x54b0);
constexpr uint32_t kDisplayHeight = UINT32_C(0x54ba);
constexpr uint32_t kDisplayUnit = UINT32_C(0x54b2);
constexpr uint32_t kContentEncodings = UINT32_C(0x6d80);
constexpr uint32_t kContentEncoding = UINT32_C(0x6240);
constexpr uint32_t kContentCompression = UINT32_C(0x5034);
constexpr uint32_t kContentEncryption = UINT32_C(0x5035);
constexpr uint32_t kCluster = UINT32_C(0x1f43b675);
constexpr uint32_t kClusterTimestamp = UINT32_C(0xe7);
constexpr uint32_t kSimpleBlock = UINT32_C(0xa3);
constexpr uint32_t kBlockGroup = UINT32_C(0xa0);
constexpr uint32_t kBlock = UINT32_C(0xa1);
constexpr uint32_t kEncryptedBlock = UINT32_C(0xaf);
constexpr uint32_t kCodecState = UINT32_C(0xa4);

struct vint_t
{
    uint64_t value;
    size_t bytes;
    bool unknown;
};

struct element_t
{
    uint32_t id;
    uint64_t size;
    size_t header_bytes;
    bool unknown_size;
    bool master;
    int level;
};

struct frame_t
{
    uint32_t id;
    uint64_t end;
    int level;
    bool unknown;
};

struct track_t
{
    uint64_t number;
    uint64_t type;
    uint64_t pixel_width;
    uint64_t pixel_height;
    uint64_t display_width;
    uint64_t display_height;
    uint64_t display_unit;
    bool number_seen;
    bool type_seen;
    bool codec_seen;
    bool codec_vp9;
    bool private_seen;
    bool content_encoding;
    bool pixel_width_seen;
    bool pixel_height_seen;
    bool display_width_seen;
    bool display_height_seen;
    bool display_unit_seen;
    bool private_too_large;
    uint8_t private_data[kMaxCodecPrivateBytes];
    size_t private_bytes;
};

struct impl_t
{
    iptv_webm_parser_t *owner;
    iptv_webm_limits_t limits;
    iptv_webm_video_callback_t callback;
    void *callback_context;
    uint8_t *buffer;
    size_t buffer_start;
    size_t buffer_bytes;
    uint64_t position;
    frame_t stack[IPTV_WEBM_HARD_MAX_NESTING];
    uint32_t depth;
    track_t track;
    uint64_t cluster_timestamp;
    bool cluster_timestamp_seen;
    bool doc_type_seen;
    bool doc_type_valid;
    bool ebml_seen;
    bool segment_seen;
    bool info_seen;
    bool tracks_seen;
    bool timestamp_scale_seen;
    bool video_ready;
    bool finished;
    bool failed;
};

static iptv_webm_result_t fail(impl_t *impl, iptv_webm_result_t result)
{
    impl->failed = true;
    impl->owner->last_result = result;
    return result;
}

static int read_vint(const uint8_t *data, size_t available, bool id, vint_t *vint)
{
    if (!available)
        return 0;
    uint8_t marker = 0x80u;
    size_t bytes = 1u;
    while (!(data[0] & marker))
    {
        marker >>= 1u;
        ++bytes;
        if (!marker || (id && bytes > 4u))
            return -1;
    }
    if (available < bytes)
        return 0;

    uint64_t value = id ? data[0] : data[0] & static_cast<uint8_t>(marker - 1u);
    for (size_t index = 1; index < bytes; ++index)
        value = (value << 8u) | data[index];
    const uint64_t unknown_value = (UINT64_C(1) << (7u * bytes)) - 1u;
    vint->value = value;
    vint->bytes = bytes;
    vint->unknown = !id && value == unknown_value;
    return 1;
}

static bool master_info(uint32_t id, int *level)
{
    switch (id)
    {
    case kEbml:
    case kSegment:
        *level = 0;
        return true;
    case kInfo:
    case kTracks:
    case kCluster:
        *level = 1;
        return true;
    case kTrackEntry:
    case kBlockGroup:
        *level = 2;
        return true;
    case kVideo:
    case kContentEncodings:
        *level = 3;
        return true;
    case kContentEncoding:
        *level = 4;
        return true;
    case kContentCompression:
    case kContentEncryption:
        *level = 5;
        return true;
    default:
        *level = -1;
        return false;
    }
}

static int read_element(const uint8_t *data, size_t available, element_t *element)
{
    vint_t id{};
    const int id_result = read_vint(data, available, true, &id);
    if (id_result <= 0)
        return id_result;
    vint_t size{};
    const int size_result = read_vint(data + id.bytes, available - id.bytes, false, &size);
    if (size_result <= 0)
        return size_result;
    if (id.value > UINT32_MAX)
        return -1;
    element->id = static_cast<uint32_t>(id.value);
    element->size = size.value;
    element->header_bytes = id.bytes + size.bytes;
    element->unknown_size = size.unknown;
    element->master = master_info(element->id, &element->level);
    return 1;
}

static uint32_t top_id(const impl_t *impl)
{
    return impl->depth ? impl->stack[impl->depth - 1u].id : 0u;
}

static bool inside(const impl_t *impl, uint32_t id)
{
    for (uint32_t index = impl->depth; index; --index)
    {
        if (impl->stack[index - 1u].id == id)
            return true;
    }
    return false;
}

static bool add_position(uint64_t left, uint64_t right, uint64_t *sum)
{
    if (left > UINT64_MAX - right)
        return false;
    *sum = left + right;
    return true;
}

static void consume(impl_t *impl, size_t bytes)
{
    impl->buffer_start += bytes;
    impl->buffer_bytes -= bytes;
    impl->position += bytes;
    if (!impl->buffer_bytes)
        impl->buffer_start = 0;
}

static bool unsigned_value(const uint8_t *data, size_t bytes, uint64_t *value)
{
    if (!bytes || bytes > 8u)
        return false;
    uint64_t number = 0;
    for (size_t index = 0; index < bytes; ++index)
        number = (number << 8u) | data[index];
    *value = number;
    return true;
}

static iptv_webm_result_t parse_codec_private(impl_t *impl, uint32_t *profile,
                                              uint32_t *profile_present)
{
    if (impl->track.private_too_large)
        return fail(impl, IPTV_WEBM_UNSUPPORTED_PROFILE);

    bool features[5]{};
    uint32_t parsed_profile = 0u;
    uint32_t bit_depth = 8u;
    uint32_t chroma = 1u;
    size_t offset = 0;
    while (offset < impl->track.private_bytes)
    {
        if (impl->track.private_bytes - offset < 2u)
            return fail(impl, IPTV_WEBM_MALFORMED);
        const uint8_t id = impl->track.private_data[offset++];
        const size_t bytes = impl->track.private_data[offset++];
        if ((id & 0x80u) || bytes > impl->track.private_bytes - offset)
            return fail(impl, IPTV_WEBM_MALFORMED);
        if (id >= 1u && id <= 4u)
        {
            if (features[id] || bytes != 1u)
                return fail(impl, IPTV_WEBM_MALFORMED);
            features[id] = true;
            const uint32_t value = impl->track.private_data[offset];
            if (id == 1u)
                parsed_profile = value;
            else if (id == 3u)
                bit_depth = value;
            else if (id == 4u)
                chroma = value;
        }
        offset += bytes;
    }
    if (parsed_profile != 0u || bit_depth != 8u || chroma > 1u)
        return fail(impl, IPTV_WEBM_UNSUPPORTED_PROFILE);
    *profile = parsed_profile;
    *profile_present = features[1] ? 1u : 0u;
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t finish_track(impl_t *impl)
{
    if (!impl->track.type_seen || !impl->track.number_seen || !impl->track.codec_seen)
        return fail(impl, IPTV_WEBM_MALFORMED);
    if (impl->track.type != 1u)
        return IPTV_WEBM_OK;
    if (impl->video_ready || !impl->track.number || !impl->track.codec_vp9)
        return fail(impl, IPTV_WEBM_UNSUPPORTED_CODEC);
    if (impl->track.content_encoding)
        return fail(impl, IPTV_WEBM_UNSUPPORTED_CONTENT_ENCODING);
    if (!impl->track.pixel_width_seen || !impl->track.pixel_height_seen ||
        !impl->track.pixel_width || !impl->track.pixel_height)
        return fail(impl, IPTV_WEBM_MALFORMED);

    const uint64_t display_width =
        impl->track.display_width_seen ? impl->track.display_width : impl->track.pixel_width;
    const uint64_t display_height =
        impl->track.display_height_seen ? impl->track.display_height : impl->track.pixel_height;
    if (!display_width || !display_height || impl->track.pixel_width > impl->limits.max_width ||
        impl->track.pixel_height > impl->limits.max_height ||
        display_width > impl->limits.max_width || display_height > impl->limits.max_height)
        return fail(impl, IPTV_WEBM_DIMENSION_LIMIT);
    if (impl->track.display_unit > 3u)
        return fail(impl, IPTV_WEBM_MALFORMED);

    uint32_t profile = 0u;
    uint32_t profile_present = 0u;
    const iptv_webm_result_t profile_result = parse_codec_private(impl, &profile, &profile_present);
    if (profile_result != IPTV_WEBM_OK)
        return profile_result;

    impl->owner->video.track_number = impl->track.number;
    impl->owner->video.timestamp_scale_ns = impl->owner->video.timestamp_scale_ns
                                                ? impl->owner->video.timestamp_scale_ns
                                                : kDefaultTimestampScaleNs;
    impl->owner->video.profile = profile;
    impl->owner->video.profile_present = profile_present;
    impl->owner->video.pixel_width = static_cast<uint32_t>(impl->track.pixel_width);
    impl->owner->video.pixel_height = static_cast<uint32_t>(impl->track.pixel_height);
    impl->owner->video.display_width = static_cast<uint32_t>(display_width);
    impl->owner->video.display_height = static_cast<uint32_t>(display_height);
    impl->owner->video.display_unit = static_cast<uint32_t>(impl->track.display_unit);
    impl->video_ready = true;
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t pop_frame(impl_t *impl)
{
    const uint32_t id = impl->stack[impl->depth - 1u].id;
    --impl->depth;
    if (id == kEbml && (!impl->doc_type_seen || !impl->doc_type_valid))
        return fail(impl, IPTV_WEBM_UNSUPPORTED_DOCUMENT);
    if (id == kTrackEntry)
        return finish_track(impl);
    if (id == kCluster)
        impl->cluster_timestamp_seen = false;
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t close_completed(impl_t *impl)
{
    while (impl->depth)
    {
        frame_t &top = impl->stack[impl->depth - 1u];
        if (!top.unknown)
        {
            if (impl->position > top.end)
                return fail(impl, IPTV_WEBM_MALFORMED);
            if (impl->position == top.end)
            {
                const iptv_webm_result_t result = pop_frame(impl);
                if (result != IPTV_WEBM_OK)
                    return result;
                continue;
            }
        }
        else
        {
            for (uint32_t index = impl->depth - 1u; index; --index)
            {
                const frame_t &ancestor = impl->stack[index - 1u];
                if (ancestor.unknown)
                    continue;
                if (impl->position > ancestor.end)
                    return fail(impl, IPTV_WEBM_MALFORMED);
                if (impl->position == ancestor.end)
                {
                    const iptv_webm_result_t result = pop_frame(impl);
                    if (result != IPTV_WEBM_OK)
                        return result;
                    goto next_frame;
                }
                break;
            }
        }
        break;
    next_frame:
        continue;
    }
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t unwind_unknown(impl_t *impl, int level)
{
    if (level < 0)
        return IPTV_WEBM_OK;
    while (impl->depth)
    {
        const frame_t &top = impl->stack[impl->depth - 1u];
        if (!top.unknown || level > top.level)
            break;
        const iptv_webm_result_t result = pop_frame(impl);
        if (result != IPTV_WEBM_OK)
            return result;
    }
    return IPTV_WEBM_OK;
}

static bool direct_parent(const impl_t *impl, uint32_t id)
{
    switch (id)
    {
    case kEbml:
    case kSegment:
        return impl->depth == 0u;
    case kInfo:
    case kTracks:
    case kCluster:
        return top_id(impl) == kSegment;
    case kTrackEntry:
        return top_id(impl) == kTracks;
    case kVideo:
    case kContentEncodings:
        return top_id(impl) == kTrackEntry;
    case kBlockGroup:
        return top_id(impl) == kCluster;
    case kContentEncoding:
        return top_id(impl) == kContentEncodings;
    case kContentCompression:
    case kContentEncryption:
        return top_id(impl) == kContentEncoding;
    default:
        return true;
    }
}

static iptv_webm_result_t begin_master(impl_t *impl, const element_t &element)
{
    if (!direct_parent(impl, element.id))
        return fail(impl, IPTV_WEBM_MALFORMED);
    if (element.unknown_size && element.id != kSegment && element.id != kCluster)
        return fail(impl, IPTV_WEBM_MALFORMED);
    if (impl->depth >= impl->limits.max_nesting || impl->depth >= IPTV_WEBM_HARD_MAX_NESTING)
        return fail(impl, IPTV_WEBM_NESTING_LIMIT);
    if (element.id == kEbml)
    {
        if (impl->ebml_seen || impl->segment_seen)
            return fail(impl, IPTV_WEBM_MALFORMED);
        impl->ebml_seen = true;
    }
    else if (element.id == kSegment)
    {
        if (!impl->ebml_seen || !impl->doc_type_valid || impl->segment_seen)
            return fail(impl, IPTV_WEBM_UNSUPPORTED_DOCUMENT);
        impl->segment_seen = true;
    }
    else if (element.id == kInfo)
    {
        if (impl->info_seen)
            return fail(impl, IPTV_WEBM_MALFORMED);
        impl->info_seen = true;
    }
    else if (element.id == kTracks)
    {
        if (impl->tracks_seen)
            return fail(impl, IPTV_WEBM_MALFORMED);
        impl->tracks_seen = true;
    }
    else if (element.id == kTrackEntry)
    {
        impl->track = {};
    }
    else if (element.id == kContentEncodings)
    {
        impl->track.content_encoding = true;
    }
    else if (element.id == kCluster)
    {
        impl->cluster_timestamp = 0;
        impl->cluster_timestamp_seen = false;
    }

    uint64_t end = 0;
    if (!element.unknown_size && !add_position(impl->position, element.size, &end))
        return fail(impl, IPTV_WEBM_MALFORMED);
    impl->stack[impl->depth++] = {element.id, end, element.level, element.unknown_size};
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t assign_once(impl_t *impl, bool *seen, uint64_t *target,
                                      const uint8_t *data, size_t bytes)
{
    uint64_t value = 0;
    if (*seen || !unsigned_value(data, bytes, &value))
        return fail(impl, IPTV_WEBM_MALFORMED);
    *seen = true;
    *target = value;
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t block_pts(impl_t *impl, int16_t relative, uint64_t *pts_us)
{
    uint64_t ticks = impl->cluster_timestamp;
    if (relative < 0)
    {
        const uint64_t amount = static_cast<uint64_t>(-static_cast<int32_t>(relative));
        if (ticks < amount)
            return fail(impl, IPTV_WEBM_MALFORMED);
        ticks -= amount;
    }
    else
    {
        if (!add_position(ticks, static_cast<uint16_t>(relative), &ticks))
            return fail(impl, IPTV_WEBM_TIMESTAMP_OVERFLOW);
    }
    const uint64_t scale = impl->owner->video.timestamp_scale_ns;
    if (!scale || ticks > UINT64_MAX / scale)
        return fail(impl, IPTV_WEBM_TIMESTAMP_OVERFLOW);
    *pts_us = ticks * scale / 1000u;
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t emit_block(impl_t *impl, const uint8_t *data, size_t bytes,
                                     iptv_webm_block_kind_t kind)
{
    vint_t track{};
    const int track_result = read_vint(data, bytes, false, &track);
    if (track_result != 1 || track.unknown || !track.value || bytes < track.bytes + 4u)
        return fail(impl, IPTV_WEBM_MALFORMED);
    if (!impl->video_ready)
        return fail(impl, IPTV_WEBM_MISSING_VIDEO_TRACK);
    if (track.value != impl->owner->video.track_number)
        return IPTV_WEBM_OK;

    const uint8_t *header = data + track.bytes;
    const int16_t relative = static_cast<int16_t>(static_cast<uint16_t>(header[0]) << 8u |
                                                  static_cast<uint16_t>(header[1]));
    const uint8_t flags = header[2];
    if (flags & 0x06u)
        return fail(impl, IPTV_WEBM_UNSUPPORTED_LACING);
    if (!impl->cluster_timestamp_seen)
        return fail(impl, IPTV_WEBM_MALFORMED);

    uint64_t pts_us = 0;
    const iptv_webm_result_t pts_result = block_pts(impl, relative, &pts_us);
    if (pts_result != IPTV_WEBM_OK)
        return pts_result;

    iptv_webm_block_t block{};
    block.data = header + 3u;
    block.bytes = bytes - track.bytes - 3u;
    block.pts_us = pts_us;
    block.track_number = track.value;
    block.kind = kind;
    block.keyframe = kind == IPTV_WEBM_SIMPLE_BLOCK && (flags & 0x80u) ? 1u : 0u;
    block.invisible = flags & 0x08u ? 1u : 0u;
    block.discardable = kind == IPTV_WEBM_SIMPLE_BLOCK && (flags & 0x01u) ? 1u : 0u;
    if (impl->callback(impl->callback_context, &impl->owner->video, &block) != 0)
        return fail(impl, IPTV_WEBM_CALLBACK_ERROR);
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t process_leaf(impl_t *impl, uint32_t id, const uint8_t *data, size_t bytes)
{
    if (id == kDocType && top_id(impl) == kEbml)
    {
        if (impl->doc_type_seen)
            return fail(impl, IPTV_WEBM_MALFORMED);
        impl->doc_type_seen = true;
        impl->doc_type_valid = (bytes == 4u && std::memcmp(data, "webm", 4u) == 0) ||
                               (bytes == 8u && std::memcmp(data, "matroska", 8u) == 0);
        return impl->doc_type_valid ? IPTV_WEBM_OK : fail(impl, IPTV_WEBM_UNSUPPORTED_DOCUMENT);
    }
    if (id == kTimestampScale && top_id(impl) == kInfo)
    {
        uint64_t value = 0;
        if (impl->timestamp_scale_seen || !unsigned_value(data, bytes, &value) || !value ||
            value > kMaxTimestampScaleNs)
            return fail(impl, IPTV_WEBM_MALFORMED);
        impl->timestamp_scale_seen = true;
        impl->owner->video.timestamp_scale_ns = value;
        return IPTV_WEBM_OK;
    }
    if (top_id(impl) == kTrackEntry)
    {
        if (id == kTrackNumber)
            return assign_once(impl, &impl->track.number_seen, &impl->track.number, data, bytes);
        if (id == kTrackType)
            return assign_once(impl, &impl->track.type_seen, &impl->track.type, data, bytes);
        if (id == kCodecId)
        {
            if (impl->track.codec_seen)
                return fail(impl, IPTV_WEBM_MALFORMED);
            impl->track.codec_seen = true;
            impl->track.codec_vp9 = bytes == 5u && std::memcmp(data, "V_VP9", 5u) == 0;
            return IPTV_WEBM_OK;
        }
        if (id == kCodecPrivate)
        {
            if (impl->track.private_seen)
                return fail(impl, IPTV_WEBM_MALFORMED);
            impl->track.private_seen = true;
            impl->track.private_too_large = bytes > kMaxCodecPrivateBytes;
            if (!impl->track.private_too_large)
            {
                std::memcpy(impl->track.private_data, data, bytes);
                impl->track.private_bytes = bytes;
            }
            return IPTV_WEBM_OK;
        }
    }
    if (top_id(impl) == kVideo)
    {
        if (id == kPixelWidth)
            return assign_once(impl, &impl->track.pixel_width_seen, &impl->track.pixel_width, data,
                               bytes);
        if (id == kPixelHeight)
            return assign_once(impl, &impl->track.pixel_height_seen, &impl->track.pixel_height,
                               data, bytes);
        if (id == kDisplayWidth)
            return assign_once(impl, &impl->track.display_width_seen, &impl->track.display_width,
                               data, bytes);
        if (id == kDisplayHeight)
            return assign_once(impl, &impl->track.display_height_seen, &impl->track.display_height,
                               data, bytes);
        if (id == kDisplayUnit)
            return assign_once(impl, &impl->track.display_unit_seen, &impl->track.display_unit,
                               data, bytes);
    }
    if (id == kClusterTimestamp && top_id(impl) == kCluster)
    {
        if (impl->cluster_timestamp_seen || !unsigned_value(data, bytes, &impl->cluster_timestamp))
            return fail(impl, IPTV_WEBM_MALFORMED);
        impl->cluster_timestamp_seen = true;
        return IPTV_WEBM_OK;
    }
    if (id == kSimpleBlock && top_id(impl) == kCluster)
        return emit_block(impl, data, bytes, IPTV_WEBM_SIMPLE_BLOCK);
    if (id == kBlock && top_id(impl) == kBlockGroup)
        return emit_block(impl, data, bytes, IPTV_WEBM_BLOCK);
    if ((id == kEncryptedBlock && inside(impl, kCluster)) ||
        (id == kCodecState && inside(impl, kBlockGroup)))
        return fail(impl, IPTV_WEBM_UNSUPPORTED_CONTENT_ENCODING);
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t check_parent_end(impl_t *impl, uint64_t end)
{
    for (uint32_t index = impl->depth; index; --index)
    {
        const frame_t &parent = impl->stack[index - 1u];
        if (!parent.unknown)
            return end <= parent.end ? IPTV_WEBM_OK : fail(impl, IPTV_WEBM_MALFORMED);
    }
    return IPTV_WEBM_OK;
}

static iptv_webm_result_t parse_available(impl_t *impl)
{
    while (true)
    {
        iptv_webm_result_t result = close_completed(impl);
        if (result != IPTV_WEBM_OK || !impl->buffer_bytes)
            return result;

        element_t element{};
        const uint8_t *data = impl->buffer + impl->buffer_start;
        const int header_result = read_element(data, impl->buffer_bytes, &element);
        if (header_result == 0)
            return IPTV_WEBM_OK;
        if (header_result < 0)
            return fail(impl, IPTV_WEBM_MALFORMED);

        result = unwind_unknown(impl, element.level);
        if (result != IPTV_WEBM_OK)
            return result;
        result = close_completed(impl);
        if (result != IPTV_WEBM_OK)
            return result;

        uint64_t payload_start = 0;
        if (!add_position(impl->position, element.header_bytes, &payload_start))
            return fail(impl, IPTV_WEBM_MALFORMED);
        uint64_t element_end = 0;
        if (!element.unknown_size && !add_position(payload_start, element.size, &element_end))
            return fail(impl, IPTV_WEBM_MALFORMED);
        if (!element.unknown_size)
        {
            result = check_parent_end(impl, element_end);
            if (result != IPTV_WEBM_OK)
                return result;
        }

        if (element.master)
        {
            if (!element.unknown_size && element.id != kSegment && element.id != kCluster &&
                element.size > impl->limits.max_element_bytes)
                return fail(impl, IPTV_WEBM_ELEMENT_LIMIT);
            consume(impl, element.header_bytes);
            result = begin_master(impl, element);
            if (result != IPTV_WEBM_OK)
                return result;
            continue;
        }
        if (element.unknown_size)
            return fail(impl, IPTV_WEBM_MALFORMED);
        if (element.size > impl->limits.max_element_bytes)
            return fail(impl, IPTV_WEBM_ELEMENT_LIMIT);
        if (element.size > SIZE_MAX - element.header_bytes)
            return fail(impl, IPTV_WEBM_ELEMENT_LIMIT);
        const size_t total = element.header_bytes + static_cast<size_t>(element.size);
        if (impl->buffer_bytes < total)
            return IPTV_WEBM_OK;
        result = process_leaf(impl, element.id, data + element.header_bytes,
                              static_cast<size_t>(element.size));
        if (result != IPTV_WEBM_OK)
            return result;
        consume(impl, total);
    }
}

static void compact(impl_t *impl)
{
    if (!impl->buffer_start)
        return;
    if (impl->buffer_bytes)
        std::memmove(impl->buffer, impl->buffer + impl->buffer_start, impl->buffer_bytes);
    impl->buffer_start = 0;
}

static iptv_webm_limits_t effective_limits(const iptv_webm_limits_t *requested)
{
    iptv_webm_limits_t limits{};
    iptv_webm_default_limits(&limits);
    if (!requested)
        return limits;
    if (requested->max_retained_bytes)
        limits.max_retained_bytes = requested->max_retained_bytes;
    if (requested->max_element_bytes)
        limits.max_element_bytes = requested->max_element_bytes;
    if (requested->max_nesting)
        limits.max_nesting = requested->max_nesting;
    if (requested->max_width)
        limits.max_width = requested->max_width;
    if (requested->max_height)
        limits.max_height = requested->max_height;
    return limits;
}

} // namespace

void iptv_webm_default_limits(iptv_webm_limits_t *limits)
{
    if (!limits)
        return;
    limits->max_retained_bytes = IPTV_WEBM_DEFAULT_MAX_RETAINED_BYTES;
    limits->max_element_bytes = IPTV_WEBM_DEFAULT_MAX_ELEMENT_BYTES;
    limits->max_nesting = IPTV_WEBM_DEFAULT_MAX_NESTING;
    limits->max_width = IPTV_WEBM_DEFAULT_MAX_WIDTH;
    limits->max_height = IPTV_WEBM_DEFAULT_MAX_HEIGHT;
}

void iptv_webm_init(iptv_webm_parser_t *parser)
{
    if (!parser)
        return;
    std::memset(parser, 0, sizeof(*parser));
    parser->_magic = kMagic;
    parser->last_result = IPTV_WEBM_OK;
}

iptv_webm_result_t iptv_webm_open(iptv_webm_parser_t *parser, const iptv_webm_limits_t *requested,
                                  iptv_webm_video_callback_t callback, void *context)
{
    if (!parser || parser->_magic != kMagic || !callback)
        return IPTV_WEBM_INVALID_ARGUMENT;
    if (parser->_impl)
        return IPTV_WEBM_INVALID_STATE;
    const iptv_webm_limits_t limits = effective_limits(requested);
    if (limits.max_retained_bytes < 16u || !limits.max_element_bytes ||
        limits.max_element_bytes > limits.max_retained_bytes - 12u || !limits.max_nesting ||
        limits.max_nesting > IPTV_WEBM_HARD_MAX_NESTING || !limits.max_width || !limits.max_height)
    {
        parser->last_result = IPTV_WEBM_INVALID_ARGUMENT;
        return parser->last_result;
    }

    impl_t *impl = new (std::nothrow) impl_t{};
    if (!impl)
    {
        parser->last_result = IPTV_WEBM_OUT_OF_MEMORY;
        return parser->last_result;
    }
    impl->buffer = new (std::nothrow) uint8_t[limits.max_retained_bytes];
    if (!impl->buffer)
    {
        delete impl;
        parser->last_result = IPTV_WEBM_OUT_OF_MEMORY;
        return parser->last_result;
    }
    parser->video = {};
    parser->video.timestamp_scale_ns = kDefaultTimestampScaleNs;
    parser->last_result = IPTV_WEBM_OK;
    parser->_impl = impl;
    impl->owner = parser;
    impl->limits = limits;
    impl->callback = callback;
    impl->callback_context = context;
    return IPTV_WEBM_OK;
}

iptv_webm_result_t iptv_webm_push(iptv_webm_parser_t *parser, const void *data, size_t bytes)
{
    if (!parser || parser->_magic != kMagic || (!data && bytes))
        return IPTV_WEBM_INVALID_ARGUMENT;
    impl_t *impl = static_cast<impl_t *>(parser->_impl);
    if (!impl || impl->finished)
        return IPTV_WEBM_INVALID_STATE;
    if (impl->failed)
        return parser->last_result;

    const uint8_t *input = static_cast<const uint8_t *>(data);
    while (bytes)
    {
        iptv_webm_result_t result = parse_available(impl);
        if (result != IPTV_WEBM_OK)
            return result;
        if (impl->buffer_start)
            compact(impl);
        const size_t room = impl->limits.max_retained_bytes - impl->buffer_bytes;
        if (!room)
            return fail(impl, IPTV_WEBM_RETAINED_LIMIT);
        const size_t copied = bytes < room ? bytes : room;
        std::memcpy(impl->buffer + impl->buffer_bytes, input, copied);
        impl->buffer_bytes += copied;
        input += copied;
        bytes -= copied;
    }
    return parse_available(impl);
}

iptv_webm_result_t iptv_webm_finish(iptv_webm_parser_t *parser)
{
    if (!parser || parser->_magic != kMagic)
        return IPTV_WEBM_INVALID_ARGUMENT;
    impl_t *impl = static_cast<impl_t *>(parser->_impl);
    if (!impl || impl->finished)
        return IPTV_WEBM_INVALID_STATE;
    if (impl->failed)
        return parser->last_result;
    iptv_webm_result_t result = parse_available(impl);
    if (result != IPTV_WEBM_OK)
        return result;
    if (impl->buffer_bytes)
        return fail(impl, IPTV_WEBM_TRUNCATED);

    result = close_completed(impl);
    if (result != IPTV_WEBM_OK)
        return result;
    while (impl->depth && impl->stack[impl->depth - 1u].unknown)
    {
        result = pop_frame(impl);
        if (result != IPTV_WEBM_OK)
            return result;
    }
    if (impl->depth)
        return fail(impl, IPTV_WEBM_TRUNCATED);
    if (!impl->ebml_seen || !impl->segment_seen || !impl->doc_type_valid)
        return fail(impl, IPTV_WEBM_UNSUPPORTED_DOCUMENT);
    if (!impl->video_ready)
        return fail(impl, IPTV_WEBM_MISSING_VIDEO_TRACK);
    impl->finished = true;
    parser->last_result = IPTV_WEBM_OK;
    return IPTV_WEBM_OK;
}

iptv_webm_result_t iptv_webm_cleanup(iptv_webm_parser_t *parser)
{
    if (!parser || parser->_magic != kMagic)
        return IPTV_WEBM_INVALID_ARGUMENT;
    impl_t *impl = static_cast<impl_t *>(parser->_impl);
    if (impl)
    {
        delete[] impl->buffer;
        delete impl;
    }
    parser->_impl = nullptr;
    parser->video = {};
    parser->last_result = IPTV_WEBM_OK;
    return IPTV_WEBM_OK;
}

const iptv_webm_video_info_t *iptv_webm_video(const iptv_webm_parser_t *parser)
{
    return parser && parser->_magic == kMagic && parser->_impl && parser->video.track_number
               ? &parser->video
               : nullptr;
}

const char *iptv_webm_result_name(iptv_webm_result_t result)
{
    switch (result)
    {
    case IPTV_WEBM_OK:
        return "ok";
    case IPTV_WEBM_INVALID_ARGUMENT:
        return "invalid argument";
    case IPTV_WEBM_INVALID_STATE:
        return "invalid state";
    case IPTV_WEBM_OUT_OF_MEMORY:
        return "out of memory";
    case IPTV_WEBM_RETAINED_LIMIT:
        return "retained byte limit";
    case IPTV_WEBM_ELEMENT_LIMIT:
        return "element size limit";
    case IPTV_WEBM_NESTING_LIMIT:
        return "nesting limit";
    case IPTV_WEBM_DIMENSION_LIMIT:
        return "dimension limit";
    case IPTV_WEBM_MALFORMED:
        return "malformed WebM";
    case IPTV_WEBM_TRUNCATED:
        return "truncated WebM";
    case IPTV_WEBM_UNSUPPORTED_DOCUMENT:
        return "unsupported document";
    case IPTV_WEBM_UNSUPPORTED_CODEC:
        return "unsupported codec";
    case IPTV_WEBM_UNSUPPORTED_PROFILE:
        return "unsupported VP9 profile";
    case IPTV_WEBM_UNSUPPORTED_CONTENT_ENCODING:
        return "unsupported content encoding";
    case IPTV_WEBM_UNSUPPORTED_LACING:
        return "unsupported video lacing";
    case IPTV_WEBM_MISSING_VIDEO_TRACK:
        return "missing VP9 video track";
    case IPTV_WEBM_TIMESTAMP_OVERFLOW:
        return "timestamp overflow";
    case IPTV_WEBM_CALLBACK_ERROR:
        return "video callback failed";
    }
    return "unknown WebM result";
}
