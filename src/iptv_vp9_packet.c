/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_vp9_packet.h"

#include <string.h>

typedef struct bit_reader
{
    const uint8_t *data;
    size_t bits;
    size_t position;
} bit_reader_t;

static int read_bit(bit_reader_t *reader, uint32_t *value)
{
    if (!reader || !value || reader->position >= reader->bits)
        return 0;
    *value = (reader->data[reader->position >> 3] >> (reader->position & 7u)) & 1u;
    ++reader->position;
    return 1;
}

static int read_bits(bit_reader_t *reader, uint32_t count, uint32_t *value)
{
    uint32_t result = 0;
    uint32_t index;

    if (!value || count > 32u)
        return 0;
    for (index = 0; index < count; ++index)
    {
        uint32_t bit;
        if (!read_bit(reader, &bit))
            return 0;
        result |= bit << index;
    }
    *value = result;
    return 1;
}

int iptv_vp9_split_packet(const void *data, size_t bytes, iptv_vp9_packet_t *packet)
{
    const uint8_t *source = data;
    uint8_t marker;
    uint32_t frame_count;
    uint32_t magnitude;
    size_t index_bytes;
    size_t payload_bytes;
    size_t index_offset;
    size_t payload_offset = 0;
    uint32_t frame_index;

    if (!source || bytes == 0 || !packet)
        return -1;
    memset(packet, 0, sizeof(*packet));
    marker = source[bytes - 1u];
    if ((marker & 0xe0u) != 0xc0u)
    {
        packet->frames[0].data = source;
        packet->frames[0].bytes = bytes;
        packet->count = 1;
        return 0;
    }

    frame_count = (marker & 7u) + 1u;
    magnitude = ((marker >> 3) & 3u) + 1u;
    index_bytes = 2u + (size_t)frame_count * magnitude;
    if (bytes < index_bytes)
        return -1;
    index_offset = bytes - index_bytes;
    if (source[index_offset] != marker)
        return -1;
    payload_bytes = index_offset;

    for (frame_index = 0; frame_index < frame_count; ++frame_index)
    {
        size_t frame_bytes = 0;
        uint32_t byte_index;
        for (byte_index = 0; byte_index < magnitude; ++byte_index)
        {
            frame_bytes |=
                (size_t)source[index_offset + 1u + (size_t)frame_index * magnitude + byte_index]
                << (8u * byte_index);
        }
        if (frame_bytes == 0 || frame_bytes > payload_bytes - payload_offset)
            return -1;
        packet->frames[frame_index].data = source + payload_offset;
        packet->frames[frame_index].bytes = frame_bytes;
        payload_offset += frame_bytes;
    }
    if (payload_offset != payload_bytes)
        return -1;
    packet->count = frame_count;
    return 0;
}

int iptv_vp9_read_frame_flags(const void *data, size_t bytes, uint32_t expected_profile,
                              iptv_vp9_frame_flags_t *flags)
{
    bit_reader_t reader;
    uint32_t frame_marker;
    uint32_t profile_low;
    uint32_t profile_high;
    uint32_t profile;
    uint32_t value;

    if (!data || bytes == 0 || !flags || expected_profile > 3u)
        return -1;
    memset(flags, 0, sizeof(*flags));
    reader.data = data;
    reader.bits = bytes * 8u;
    reader.position = 0;
    if (!read_bits(&reader, 2, &frame_marker) || frame_marker != 2u ||
        !read_bit(&reader, &profile_low) || !read_bit(&reader, &profile_high))
        return -1;
    profile = profile_low | (profile_high << 1);
    if (profile == 3u && (!read_bit(&reader, &value) || value != 0u))
        return -1;
    if (profile != expected_profile)
        return -1;
    flags->profile = (uint8_t)profile;

    if (!read_bit(&reader, &value))
        return -1;
    flags->show_existing_frame = (uint8_t)value;
    if (value != 0u)
    {
        if (!read_bits(&reader, 3, &value))
            return -1;
        flags->displayable = 1;
        return 0;
    }

    /* frame_type is not needed for queueing, but precedes show_frame. */
    if (!read_bit(&reader, &value) || !read_bit(&reader, &value))
        return -1;
    flags->show_frame = (uint8_t)value;
    flags->displayable = (uint8_t)value;
    return 0;
}
