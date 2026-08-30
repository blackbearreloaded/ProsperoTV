/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_hls.h"

#include <cstring>

namespace
{

struct slice_t
{
    const char *data;
    size_t size;
};

struct url_parts_t
{
    slice_t scheme;
    slice_t authority;
    slice_t path;
    slice_t query;
    bool has_query;
};

enum attr_result_t
{
    ATTR_NOT_FOUND = 0,
    ATTR_FOUND,
    ATTR_INVALID
};

static bool ascii_space(char value)
{
    return value == ' ' || value == '\t';
}

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

static slice_t trim(slice_t value)
{
    while (value.size && ascii_space(value.data[0]))
    {
        ++value.data;
        --value.size;
    }
    while (value.size && ascii_space(value.data[value.size - 1]))
        --value.size;
    return value;
}

static bool equal(slice_t value, const char *text)
{
    const size_t bytes = std::strlen(text);
    return value.size == bytes && std::memcmp(value.data, text, bytes) == 0;
}

static bool equal_ascii_case(slice_t value, const char *text)
{
    const size_t bytes = std::strlen(text);
    if (value.size != bytes)
        return false;
    for (size_t i = 0; i < bytes; ++i)
    {
        if (ascii_lower(value.data[i]) != ascii_lower(text[i]))
            return false;
    }
    return true;
}

static bool starts_with(slice_t value, const char *prefix)
{
    const size_t bytes = std::strlen(prefix);
    return value.size >= bytes && std::memcmp(value.data, prefix, bytes) == 0;
}

static bool starts_with_ascii_case(slice_t value, const char *prefix)
{
    const size_t bytes = std::strlen(prefix);
    if (value.size < bytes)
        return false;
    for (size_t i = 0; i < bytes; ++i)
    {
        if (ascii_lower(value.data[i]) != ascii_lower(prefix[i]))
            return false;
    }
    return true;
}

static bool tag_value(slice_t line, const char *tag, slice_t *value)
{
    if (!starts_with(line, tag))
        return false;
    value->data = line.data + std::strlen(tag);
    value->size = line.size - std::strlen(tag);
    *value = trim(*value);
    return true;
}

static bool parse_u64(slice_t value, uint64_t *result)
{
    value = trim(value);
    if (!value.size)
        return false;
    uint64_t number = 0;
    for (size_t i = 0; i < value.size; ++i)
    {
        const char digit = value.data[i];
        if (digit < '0' || digit > '9')
            return false;
        const uint64_t next = static_cast<uint64_t>(digit - '0');
        if (number > (UINT64_MAX - next) / 10u)
            return false;
        number = number * 10u + next;
    }
    *result = number;
    return true;
}

static bool parse_u32(slice_t value, uint32_t *result)
{
    uint64_t number = 0;
    if (!parse_u64(value, &number) || number > UINT32_MAX)
        return false;
    *result = static_cast<uint32_t>(number);
    return true;
}

static bool parse_duration_ms(slice_t value, uint32_t *result)
{
    size_t end = 0;
    while (end < value.size && value.data[end] != ',')
        ++end;
    value.size = end;
    value = trim(value);
    if (!value.size)
        return false;

    uint64_t whole = 0;
    uint32_t fraction = 0;
    uint32_t fraction_digits = 0;
    bool decimal = false;
    bool saw_digit = false;
    for (size_t i = 0; i < value.size; ++i)
    {
        const char item = value.data[i];
        if (item == '.' && !decimal)
        {
            decimal = true;
            continue;
        }
        if (item < '0' || item > '9')
            return false;
        saw_digit = true;
        if (!decimal)
        {
            const uint64_t digit = static_cast<uint64_t>(item - '0');
            if (whole > (UINT64_MAX - digit) / 10u)
                return false;
            whole = whole * 10u + digit;
        }
        else if (fraction_digits < 3u)
        {
            fraction = fraction * 10u + static_cast<uint32_t>(item - '0');
            ++fraction_digits;
        }
    }
    if (!saw_digit || (decimal && value.data[value.size - 1] == '.'))
        return false;
    while (fraction_digits < 3u)
    {
        fraction *= 10u;
        ++fraction_digits;
    }
    if (whole > (UINT32_MAX - fraction) / 1000u)
        return false;
    const uint64_t milliseconds = whole * 1000u + fraction;
    if (!milliseconds || milliseconds > UINT32_MAX)
        return false;
    *result = static_cast<uint32_t>(milliseconds);
    return true;
}

static attr_result_t find_attribute(slice_t list, const char *wanted, slice_t *found)
{
    bool matched = false;
    size_t position = 0;
    while (position < list.size)
    {
        while (position < list.size && ascii_space(list.data[position]))
            ++position;
        const size_t key_start = position;
        while (position < list.size && list.data[position] != '=' && list.data[position] != ',')
            ++position;
        if (position == key_start || position >= list.size || list.data[position] != '=')
            return ATTR_INVALID;
        slice_t key = trim({list.data + key_start, position - key_start});
        ++position;
        while (position < list.size && ascii_space(list.data[position]))
            ++position;

        slice_t value{};
        if (position < list.size && list.data[position] == '"')
        {
            ++position;
            const size_t value_start = position;
            while (position < list.size && list.data[position] != '"')
                ++position;
            if (position >= list.size)
                return ATTR_INVALID;
            value = {list.data + value_start, position - value_start};
            ++position;
            while (position < list.size && ascii_space(list.data[position]))
                ++position;
            if (position < list.size && list.data[position] != ',')
                return ATTR_INVALID;
        }
        else
        {
            const size_t value_start = position;
            while (position < list.size && list.data[position] != ',')
                ++position;
            value = trim({list.data + value_start, position - value_start});
            if (!value.size)
                return ATTR_INVALID;
        }

        if (equal(key, wanted))
        {
            if (matched)
                return ATTR_INVALID;
            *found = value;
            matched = true;
        }
        if (position < list.size)
        {
            ++position;
            if (position == list.size)
                return ATTR_INVALID;
        }
    }
    return matched ? ATTR_FOUND : ATTR_NOT_FOUND;
}

static bool valid_url_bytes(slice_t value)
{
    if (!value.size)
        return false;
    for (size_t i = 0; i < value.size; ++i)
    {
        const unsigned char byte = static_cast<unsigned char>(value.data[i]);
        if (byte <= 0x20u || byte == 0x7fu || value.data[i] == '#' || value.data[i] == '\\')
            return false;
    }
    return true;
}

static bool parse_absolute_url(slice_t value, url_parts_t *parts)
{
    size_t colon = 0;
    while (colon < value.size && value.data[colon] != ':')
        ++colon;
    if (colon == value.size || colon + 2u >= value.size || value.data[colon + 1u] != '/' ||
        value.data[colon + 2u] != '/')
        return false;
    const slice_t scheme{value.data, colon};
    if (!equal_ascii_case(scheme, "http") && !equal_ascii_case(scheme, "https"))
        return false;

    size_t authority_end = colon + 3u;
    while (authority_end < value.size && value.data[authority_end] != '/' &&
           value.data[authority_end] != '?')
        ++authority_end;
    if (authority_end == colon + 3u)
        return false;

    size_t query_at = authority_end;
    while (query_at < value.size && value.data[query_at] != '?')
        ++query_at;
    const size_t path_start = authority_end;
    const size_t path_end = query_at;
    parts->scheme = scheme;
    parts->authority = {value.data + colon + 3u, authority_end - (colon + 3u)};
    parts->path = {value.data + path_start, path_end - path_start};
    parts->has_query = query_at < value.size;
    parts->query = parts->has_query
                       ? slice_t{value.data + query_at + 1u, value.size - query_at - 1u}
                       : slice_t{nullptr, 0};
    return true;
}

static iptv_hls_result_t normalize_path(slice_t input, char *output, size_t output_bytes,
                                        size_t *written)
{
    slice_t segments[IPTV_HLS_URL_BYTES / 2u + 1u]{};
    size_t segment_count = 0;
    size_t position = 0;
    bool trailing_slash = input.size && input.data[input.size - 1u] == '/';

    while (position < input.size)
    {
        while (position < input.size && input.data[position] == '/')
            ++position;
        const size_t start = position;
        while (position < input.size && input.data[position] != '/')
            ++position;
        slice_t segment{input.data + start, position - start};
        if (!segment.size)
            continue;
        if (equal(segment, "."))
        {
            trailing_slash = position == input.size;
            continue;
        }
        if (equal(segment, ".."))
        {
            if (segment_count)
                --segment_count;
            trailing_slash = position == input.size;
            continue;
        }
        if (segment_count >= sizeof(segments) / sizeof(segments[0]))
            return IPTV_HLS_URL_LIMIT;
        segments[segment_count++] = segment;
    }

    size_t used = 0;
    if (used + 1u >= output_bytes)
        return IPTV_HLS_URL_LIMIT;
    output[used++] = '/';
    for (size_t i = 0; i < segment_count; ++i)
    {
        if (i)
        {
            if (used + 1u >= output_bytes)
                return IPTV_HLS_URL_LIMIT;
            output[used++] = '/';
        }
        if (used + segments[i].size >= output_bytes)
            return IPTV_HLS_URL_LIMIT;
        std::memcpy(output + used, segments[i].data, segments[i].size);
        used += segments[i].size;
    }
    if (trailing_slash && used > 1u)
    {
        if (used + 1u >= output_bytes)
            return IPTV_HLS_URL_LIMIT;
        output[used++] = '/';
    }
    output[used] = '\0';
    *written = used;
    return IPTV_HLS_OK;
}

static bool append_bytes(char *output, size_t output_bytes, size_t *used, const char *data,
                         size_t bytes)
{
    if (*used + bytes >= output_bytes)
        return false;
    std::memcpy(output + *used, data, bytes);
    *used += bytes;
    output[*used] = '\0';
    return true;
}

static iptv_hls_result_t build_absolute_url(const url_parts_t &parts, slice_t raw_path,
                                            slice_t query, bool has_query, char *output,
                                            size_t output_bytes)
{
    char normalized[IPTV_HLS_URL_BYTES]{};
    size_t normalized_bytes = 0;
    const iptv_hls_result_t normalized_result =
        normalize_path(raw_path, normalized, sizeof(normalized), &normalized_bytes);
    if (normalized_result != IPTV_HLS_OK)
        return normalized_result;

    size_t used = 0;
    if (!append_bytes(output, output_bytes, &used, parts.scheme.data, parts.scheme.size) ||
        !append_bytes(output, output_bytes, &used, "://", 3u) ||
        !append_bytes(output, output_bytes, &used, parts.authority.data, parts.authority.size) ||
        !append_bytes(output, output_bytes, &used, normalized, normalized_bytes))
        return IPTV_HLS_URL_LIMIT;
    if (has_query && (!append_bytes(output, output_bytes, &used, "?", 1u) ||
                      !append_bytes(output, output_bytes, &used, query.data, query.size)))
        return IPTV_HLS_URL_LIMIT;
    return IPTV_HLS_OK;
}

static bool known_scheme(slice_t reference)
{
    for (size_t i = 0; i < reference.size; ++i)
    {
        const char value = reference.data[i];
        if (value == ':')
            return true;
        if (value == '/' || value == '?')
            return false;
    }
    return false;
}

static bool has_fmp4_extension(const char *url)
{
    const size_t bytes = std::strlen(url);
    size_t end = 0;
    while (end < bytes && url[end] != '?')
        ++end;
    const char *extensions[] = {".m4s", ".mp4", ".cmfv", ".cmfa"};
    for (size_t item = 0; item < sizeof(extensions) / sizeof(extensions[0]); ++item)
    {
        const size_t extension_bytes = std::strlen(extensions[item]);
        if (end < extension_bytes)
            continue;
        slice_t tail{url + end - extension_bytes, extension_bytes};
        if (equal_ascii_case(tail, extensions[item]))
            return true;
    }
    return false;
}

static bool parse_resolution(slice_t value, uint32_t *width, uint32_t *height)
{
    size_t delimiter = 0;
    while (delimiter < value.size && value.data[delimiter] != 'x' && value.data[delimiter] != 'X')
        ++delimiter;
    if (!delimiter || delimiter == value.size - 1u)
        return false;
    const slice_t width_text{value.data, delimiter};
    const slice_t height_text{value.data + delimiter + 1u, value.size - delimiter - 1u};
    return parse_u32(width_text, width) && parse_u32(height_text, height) && *width && *height;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    value = ascii_lower(value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool parse_hex_byte(slice_t value, uint32_t *result)
{
    if (value.size != 2u)
        return false;
    const int high = hex_digit(value.data[0]);
    const int low = hex_digit(value.data[1]);
    if (high < 0 || low < 0)
        return false;
    *result = static_cast<uint32_t>((high << 4) | low);
    return true;
}

static bool codec_detail(slice_t codec, const char *prefix, slice_t *detail)
{
    const size_t prefix_bytes = std::strlen(prefix);
    if (!starts_with_ascii_case(codec, prefix))
        return false;
    if (codec.size == prefix_bytes)
    {
        *detail = {nullptr, 0};
        return true;
    }
    if (codec.size <= prefix_bytes + 1u || codec.data[prefix_bytes] != '.')
        return false;
    *detail = {codec.data + prefix_bytes + 1u, codec.size - prefix_bytes - 1u};
    return true;
}

static bool next_component(slice_t *remaining, slice_t *component)
{
    if (!remaining->size)
        return false;
    size_t bytes = 0;
    while (bytes < remaining->size && remaining->data[bytes] != '.')
        ++bytes;
    if (!bytes)
        return false;
    *component = {remaining->data, bytes};
    if (bytes == remaining->size)
    {
        remaining->data = nullptr;
        remaining->size = 0;
    }
    else
    {
        remaining->data += bytes + 1u;
        remaining->size -= bytes + 1u;
    }
    return true;
}

static bool all_hex(slice_t value)
{
    if (!value.size)
        return false;
    for (size_t i = 0; i < value.size; ++i)
    {
        if (hex_digit(value.data[i]) < 0)
            return false;
    }
    return true;
}

static bool parse_avc_detail(slice_t detail, iptv_hls_variant_t *variant)
{
    if (!detail.size)
        return true;
    if (detail.size != 6u || !all_hex(detail) ||
        !parse_hex_byte({detail.data, 2u}, &variant->profile) ||
        !parse_hex_byte({detail.data + 4u, 2u}, &variant->level))
        return false;
    variant->bit_depth = 8u;
    const uint32_t max_level = !variant->width || !variant->height                   ? 52u
                               : variant->width <= 1280u && variant->height <= 720u  ? 41u
                               : variant->width <= 2560u && variant->height <= 1440u ? 51u
                                                                                     : 52u;
    return (variant->profile == 66u || variant->profile == 77u || variant->profile == 100u) &&
           variant->level && variant->level <= max_level;
}

static bool parse_hevc_detail(slice_t detail, iptv_hls_variant_t *variant)
{
    if (!detail.size)
        return true;
    slice_t profile{}, compatibility{}, tier_level{};
    if (!next_component(&detail, &profile) || !next_component(&detail, &compatibility) ||
        !next_component(&detail, &tier_level) || compatibility.size > 8u || !all_hex(compatibility))
        return false;

    if (profile.data[0] == 'A' || profile.data[0] == 'B' || profile.data[0] == 'C' ||
        profile.data[0] == 'a' || profile.data[0] == 'b' || profile.data[0] == 'c')
    {
        ++profile.data;
        --profile.size;
    }
    if (!parse_u32(profile, &variant->profile) || tier_level.size < 2u ||
        (tier_level.data[0] != 'L' && tier_level.data[0] != 'l' && tier_level.data[0] != 'H' &&
         tier_level.data[0] != 'h') ||
        !parse_u32({tier_level.data + 1u, tier_level.size - 1u}, &variant->level))
        return false;
    variant->high_tier = ascii_lower(tier_level.data[0]) == 'h' ? 1u : 0u;
    variant->bit_depth = variant->profile == 1u ? 8u : variant->profile == 2u ? 10u : 0u;

    while (detail.size)
    {
        slice_t constraint{};
        if (!next_component(&detail, &constraint) || constraint.size > 2u || !all_hex(constraint))
            return false;
    }
    const uint32_t max_level = !variant->width || !variant->height                   ? 153u
                               : variant->width <= 1920u && variant->height <= 1080u ? 123u
                               : variant->width <= 2560u && variant->height <= 1440u ? 150u
                                                                                     : 153u;
    return variant->profile == 1u && variant->level && variant->level <= max_level &&
           !variant->high_tier;
}

static iptv_hls_codec_t parse_codecs(slice_t value, iptv_hls_variant_t *variant)
{
    bool avc = false;
    bool hevc = false;
    bool unsupported = false;
    size_t position = 0;
    while (position < value.size)
    {
        const size_t start = position;
        while (position < value.size && value.data[position] != ',')
            ++position;
        slice_t codec = trim({value.data + start, position - start});
        if (!codec.size)
            return IPTV_HLS_CODEC_UNSUPPORTED;
        slice_t detail{};
        if (codec_detail(codec, "avc1", &detail) || codec_detail(codec, "avc3", &detail))
        {
            avc = true;
            if (!parse_avc_detail(detail, variant))
                variant->compatible = 0u;
        }
        else if (codec_detail(codec, "hvc1", &detail) || codec_detail(codec, "hev1", &detail))
        {
            hevc = true;
            if (!parse_hevc_detail(detail, variant))
                variant->compatible = 0u;
        }
        else if (!codec_detail(codec, "mp4a", &detail))
        {
            unsupported = true;
        }
        if (position < value.size)
            ++position;
    }
    if (unsupported || avc == hevc)
        return IPTV_HLS_CODEC_UNSUPPORTED;
    return avc ? IPTV_HLS_CODEC_AVC : IPTV_HLS_CODEC_HEVC;
}

static iptv_hls_result_t parse_variant(slice_t attributes, const iptv_hls_limits_t &limits,
                                       iptv_hls_variant_t *variant)
{
    std::memset(variant, 0, sizeof(*variant));
    variant->codec = IPTV_HLS_CODEC_UNKNOWN;
    variant->compatible = 1u;

    slice_t value{};
    const attr_result_t bandwidth = find_attribute(attributes, "BANDWIDTH", &value);
    if (bandwidth != ATTR_FOUND || !parse_u64(value, &variant->bandwidth) || !variant->bandwidth)
        return IPTV_HLS_MALFORMED;

    const attr_result_t average = find_attribute(attributes, "AVERAGE-BANDWIDTH", &value);
    if (average == ATTR_INVALID ||
        (average == ATTR_FOUND &&
         (!parse_u64(value, &variant->average_bandwidth) || !variant->average_bandwidth)))
        return IPTV_HLS_MALFORMED;

    const attr_result_t resolution = find_attribute(attributes, "RESOLUTION", &value);
    if (resolution == ATTR_INVALID ||
        (resolution == ATTR_FOUND && !parse_resolution(value, &variant->width, &variant->height)))
        return IPTV_HLS_MALFORMED;

    const attr_result_t codecs = find_attribute(attributes, "CODECS", &value);
    if (codecs == ATTR_INVALID)
        return IPTV_HLS_MALFORMED;
    if (codecs == ATTR_FOUND)
    {
        variant->codec = parse_codecs(value, variant);
        if (variant->codec == IPTV_HLS_CODEC_UNSUPPORTED)
            variant->compatible = 0u;
    }

    const attr_result_t video_range = find_attribute(attributes, "VIDEO-RANGE", &value);
    if (video_range == ATTR_INVALID)
        return IPTV_HLS_MALFORMED;
    if (video_range == ATTR_FOUND)
    {
        if (equal_ascii_case(value, "SDR"))
            variant->video_range = IPTV_HLS_VIDEO_RANGE_SDR;
        else if (equal_ascii_case(value, "PQ"))
            variant->video_range = IPTV_HLS_VIDEO_RANGE_PQ;
        else if (equal_ascii_case(value, "HLG"))
            variant->video_range = IPTV_HLS_VIDEO_RANGE_HLG;
        else
            variant->video_range = IPTV_HLS_VIDEO_RANGE_UNSUPPORTED;
        if (variant->video_range != IPTV_HLS_VIDEO_RANGE_SDR)
            variant->compatible = 0u;
    }

    const uint64_t effective_bandwidth =
        variant->average_bandwidth ? variant->average_bandwidth : variant->bandwidth;
    const bool resolution_ok =
        (!limits.max_width || !variant->width || variant->width <= limits.max_width) &&
        (!limits.max_height || !variant->height || variant->height <= limits.max_height);
    const bool bandwidth_ok = !limits.max_bandwidth || effective_bandwidth <= limits.max_bandwidth;
    variant->within_limits = resolution_ok && bandwidth_ok ? 1u : 0u;
    return IPTV_HLS_OK;
}

static uint64_t variant_pixels(const iptv_hls_variant_t &variant)
{
    return static_cast<uint64_t>(variant.width) * variant.height;
}

static uint64_t variant_bandwidth(const iptv_hls_variant_t &variant)
{
    return variant.average_bandwidth ? variant.average_bandwidth : variant.bandwidth;
}

static bool better_variant(const iptv_hls_variant_t &candidate, const iptv_hls_variant_t &current,
                           iptv_hls_codec_t preferred)
{
    const uint64_t candidate_pixels = variant_pixels(candidate);
    const uint64_t current_pixels = variant_pixels(current);
    if (candidate_pixels != current_pixels)
        return candidate_pixels > current_pixels;
    const bool candidate_preferred = candidate.codec == preferred;
    const bool current_preferred = current.codec == preferred;
    if (candidate_preferred != current_preferred)
        return candidate_preferred;
    return variant_bandwidth(candidate) > variant_bandwidth(current);
}

static uint32_t find_best_variant(const iptv_hls_limits_t &limits,
                                  const iptv_hls_playlist_t &playlist, uint32_t excluded_variants)
{
    uint32_t known = IPTV_HLS_NO_VARIANT;
    uint32_t unknown = IPTV_HLS_NO_VARIANT;
    for (uint32_t i = 0; i < playlist.variant_count; ++i)
    {
        const iptv_hls_variant_t &variant = playlist.variants[i];
        if ((excluded_variants & (UINT32_C(1) << i)) || !variant.compatible)
            continue;
        if (!variant.within_limits)
            continue;
        uint32_t *best = variant.codec == IPTV_HLS_CODEC_UNKNOWN ? &unknown : &known;
        if (*best == IPTV_HLS_NO_VARIANT ||
            better_variant(variant, playlist.variants[*best], limits.preferred_codec))
            *best = i;
    }
    return known != IPTV_HLS_NO_VARIANT ? known : unknown;
}

static iptv_hls_result_t select_variant(const iptv_hls_limits_t &limits,
                                        iptv_hls_playlist_t *playlist)
{
    bool compatible = false;
    for (uint32_t i = 0; i < playlist->variant_count; ++i)
    {
        if (playlist->variants[i].compatible)
            compatible = true;
    }
    playlist->selected_variant = find_best_variant(limits, *playlist, 0u);
    if (playlist->selected_variant != IPTV_HLS_NO_VARIANT)
        return IPTV_HLS_OK;
    return compatible ? IPTV_HLS_NO_VARIANT_WITHIN_LIMITS : IPTV_HLS_UNSUPPORTED_CODEC;
}

static bool valid_limits(const iptv_hls_limits_t &limits)
{
    return limits.max_input_bytes && limits.max_input_bytes <= IPTV_HLS_DEFAULT_MAX_INPUT_BYTES &&
           limits.max_line_bytes && limits.max_line_bytes <= IPTV_HLS_DEFAULT_MAX_LINE_BYTES &&
           limits.max_lines && limits.max_lines <= IPTV_HLS_DEFAULT_MAX_LINES &&
           limits.max_variants && limits.max_variants <= IPTV_HLS_MAX_VARIANTS &&
           limits.max_segments && limits.max_segments <= IPTV_HLS_MAX_SEGMENTS &&
           (limits.preferred_codec == IPTV_HLS_CODEC_UNKNOWN ||
            limits.preferred_codec == IPTV_HLS_CODEC_AVC ||
            limits.preferred_codec == IPTV_HLS_CODEC_HEVC);
}

static iptv_hls_result_t fail(iptv_hls_playlist_t *playlist, iptv_hls_result_t result,
                              uint32_t line)
{
    playlist->error_line = line;
    return result;
}

} // namespace

extern "C" void iptv_hls_default_limits(iptv_hls_limits_t *limits)
{
    if (!limits)
        return;
    std::memset(limits, 0, sizeof(*limits));
    limits->max_input_bytes = IPTV_HLS_DEFAULT_MAX_INPUT_BYTES;
    limits->max_line_bytes = IPTV_HLS_DEFAULT_MAX_LINE_BYTES;
    limits->max_lines = IPTV_HLS_DEFAULT_MAX_LINES;
    limits->max_variants = IPTV_HLS_MAX_VARIANTS;
    limits->max_segments = IPTV_HLS_MAX_SEGMENTS;
    limits->max_width = 3840u;
    limits->max_height = 2160u;
    limits->max_bandwidth = UINT64_C(100000000);
    limits->preferred_codec = IPTV_HLS_CODEC_AVC;
}

extern "C" uint32_t iptv_hls_select_variant(const iptv_hls_playlist_t *playlist,
                                            const iptv_hls_limits_t *requested_limits,
                                            uint32_t excluded_variants)
{
    if (!playlist || playlist->kind != IPTV_HLS_KIND_MASTER ||
        playlist->variant_count > IPTV_HLS_MAX_VARIANTS)
        return IPTV_HLS_NO_VARIANT;
    iptv_hls_limits_t defaults{};
    iptv_hls_default_limits(&defaults);
    const iptv_hls_limits_t &limits = requested_limits ? *requested_limits : defaults;
    if (!valid_limits(limits))
        return IPTV_HLS_NO_VARIANT;
    return find_best_variant(limits, *playlist, excluded_variants);
}

extern "C" iptv_hls_result_t iptv_hls_resolve_url(const char *base_url, size_t base_url_bytes,
                                                  const char *reference, size_t reference_bytes,
                                                  char *output, size_t output_bytes)
{
    if (!base_url || !base_url_bytes || !reference || !reference_bytes || !output || !output_bytes)
        return IPTV_HLS_INVALID_ARGUMENT;
    if (base_url_bytes >= IPTV_HLS_URL_BYTES || reference_bytes >= IPTV_HLS_URL_BYTES ||
        output_bytes > IPTV_HLS_URL_BYTES)
        return IPTV_HLS_URL_LIMIT;

    const slice_t base{base_url, base_url_bytes};
    const slice_t ref{reference, reference_bytes};
    if (!valid_url_bytes(base) || !valid_url_bytes(ref))
        return IPTV_HLS_INVALID_URL;

    url_parts_t base_parts{};
    if (!parse_absolute_url(base, &base_parts))
        return IPTV_HLS_INVALID_URL;

    if (starts_with_ascii_case(ref, "http://") || starts_with_ascii_case(ref, "https://"))
    {
        url_parts_t absolute{};
        if (!parse_absolute_url(ref, &absolute))
            return IPTV_HLS_INVALID_URL;
        return build_absolute_url(absolute, absolute.path, absolute.query, absolute.has_query,
                                  output, output_bytes);
    }
    if (known_scheme(ref))
        return IPTV_HLS_INVALID_URL;

    char network_absolute[IPTV_HLS_URL_BYTES]{};
    slice_t working = ref;
    url_parts_t parts = base_parts;
    if (ref.size >= 2u && ref.data[0] == '/' && ref.data[1] == '/')
    {
        size_t used = 0;
        if (!append_bytes(network_absolute, sizeof(network_absolute), &used, base_parts.scheme.data,
                          base_parts.scheme.size) ||
            !append_bytes(network_absolute, sizeof(network_absolute), &used, ":", 1u) ||
            !append_bytes(network_absolute, sizeof(network_absolute), &used, ref.data, ref.size))
            return IPTV_HLS_URL_LIMIT;
        working = {network_absolute, used};
        if (!parse_absolute_url(working, &parts))
            return IPTV_HLS_INVALID_URL;
        return build_absolute_url(parts, parts.path, parts.query, parts.has_query, output,
                                  output_bytes);
    }

    size_t query_at = 0;
    while (query_at < ref.size && ref.data[query_at] != '?')
        ++query_at;
    slice_t ref_path{ref.data, query_at};
    const bool has_query = query_at < ref.size;
    const slice_t query = has_query ? slice_t{ref.data + query_at + 1u, ref.size - query_at - 1u}
                                    : slice_t{nullptr, 0};

    if (!ref_path.size)
    {
        return build_absolute_url(parts, base_parts.path, query, has_query, output, output_bytes);
    }

    char combined[IPTV_HLS_URL_BYTES]{};
    slice_t path = ref_path;
    if (ref_path.data[0] != '/')
    {
        size_t directory_bytes = base_parts.path.size;
        while (directory_bytes && base_parts.path.data[directory_bytes - 1u] != '/')
            --directory_bytes;
        if (!directory_bytes)
            directory_bytes = 1u;
        size_t used = 0;
        if (base_parts.path.size &&
            !append_bytes(combined, sizeof(combined), &used, base_parts.path.data, directory_bytes))
            return IPTV_HLS_URL_LIMIT;
        if (!base_parts.path.size && !append_bytes(combined, sizeof(combined), &used, "/", 1u))
            return IPTV_HLS_URL_LIMIT;
        if (!append_bytes(combined, sizeof(combined), &used, ref_path.data, ref_path.size))
            return IPTV_HLS_URL_LIMIT;
        path = {combined, used};
    }
    return build_absolute_url(parts, path, query, has_query, output, output_bytes);
}

extern "C" iptv_hls_result_t iptv_hls_parse(const char *data, size_t data_bytes,
                                            const char *playlist_url, size_t playlist_url_bytes,
                                            const iptv_hls_limits_t *requested_limits,
                                            iptv_hls_playlist_t *playlist)
{
    if (!data || !data_bytes || !playlist_url || !playlist_url_bytes || !playlist)
        return IPTV_HLS_INVALID_ARGUMENT;

    iptv_hls_limits_t defaults{};
    iptv_hls_default_limits(&defaults);
    const iptv_hls_limits_t &limits = requested_limits ? *requested_limits : defaults;
    std::memset(playlist, 0, sizeof(*playlist));
    playlist->selected_variant = IPTV_HLS_NO_VARIANT;
    playlist->is_live = 1u;
    if (!valid_limits(limits))
        return IPTV_HLS_INVALID_ARGUMENT;
    if (data_bytes > limits.max_input_bytes)
        return IPTV_HLS_INPUT_LIMIT;
    if (playlist_url_bytes >= IPTV_HLS_URL_BYTES)
        return IPTV_HLS_URL_LIMIT;

    char validated_base[IPTV_HLS_URL_BYTES]{};
    const char self[] = "./";
    const iptv_hls_result_t base_result =
        iptv_hls_resolve_url(playlist_url, playlist_url_bytes, self, sizeof(self) - 1u,
                             validated_base, sizeof(validated_base));
    if (base_result != IPTV_HLS_OK)
        return base_result;

    bool header_seen = false;
    bool master_seen = false;
    bool media_seen = false;
    bool pending_variant = false;
    bool pending_segment = false;
    bool pending_discontinuity = false;
    bool segments_seen = false;
    uint32_t pending_duration_ms = 0;
    iptv_hls_variant_t variant{};
    uint64_t current_discontinuity = 0;

    size_t position = 0;
    uint32_t line_number = 0;
    while (position < data_bytes)
    {
        const size_t line_start = position;
        while (position < data_bytes && data[position] != '\n' && data[position] != '\r')
            ++position;
        const size_t raw_line_bytes = position - line_start;
        if (raw_line_bytes > limits.max_line_bytes)
            return fail(playlist, IPTV_HLS_LINE_LIMIT, line_number + 1u);
        if (position < data_bytes && data[position] == '\r')
        {
            ++position;
            if (position < data_bytes && data[position] == '\n')
                ++position;
        }
        else if (position < data_bytes)
        {
            ++position;
        }
        if (++line_number > limits.max_lines)
            return fail(playlist, IPTV_HLS_INPUT_LIMIT, line_number);

        slice_t line{data + line_start, raw_line_bytes};
        for (size_t i = 0; i < line.size; ++i)
        {
            const unsigned char byte = static_cast<unsigned char>(line.data[i]);
            if (!byte || (byte < 0x20u && byte != '\t'))
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
        }
        if (line_number == 1u && line.size >= 3u &&
            static_cast<unsigned char>(line.data[0]) == 0xefu &&
            static_cast<unsigned char>(line.data[1]) == 0xbbu &&
            static_cast<unsigned char>(line.data[2]) == 0xbfu)
        {
            line.data += 3u;
            line.size -= 3u;
        }
        line = trim(line);

        if (!header_seen)
        {
            if (!equal(line, "#EXTM3U"))
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            header_seen = true;
            continue;
        }
        if (!line.size)
            continue;

        if (starts_with(line, "#EXT-X-BYTERANGE:"))
            return fail(playlist, IPTV_HLS_UNSUPPORTED_BYTE_RANGE, line_number);
        if (starts_with(line, "#EXT-X-MAP:"))
            return fail(playlist, IPTV_HLS_UNSUPPORTED_FMP4, line_number);
        if (starts_with(line, "#EXT-X-PART:") || starts_with(line, "#EXT-X-PRELOAD-HINT:") ||
            starts_with(line, "#EXT-X-SKIP:") || equal(line, "#EXT-X-I-FRAMES-ONLY"))
            return fail(playlist, IPTV_HLS_UNSUPPORTED_FEATURE, line_number);
        slice_t early_value{};
        if (tag_value(line, "#EXT-X-KEY:", &early_value) ||
            tag_value(line, "#EXT-X-SESSION-KEY:", &early_value))
        {
            slice_t method{};
            const attr_result_t method_result = find_attribute(early_value, "METHOD", &method);
            if (method_result != ATTR_FOUND)
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            if (!equal(method, "NONE"))
                return fail(playlist, IPTV_HLS_UNSUPPORTED_ENCRYPTION, line_number);
            continue;
        }

        if (pending_variant || pending_segment)
        {
            if (line.data[0] == '#')
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            if (!valid_url_bytes(line))
                return fail(playlist, IPTV_HLS_INVALID_URL, line_number);
            char *destination =
                pending_variant ? variant.url : playlist->segments[playlist->segment_count].url;
            const iptv_hls_result_t url_result =
                iptv_hls_resolve_url(playlist_url, playlist_url_bytes, line.data, line.size,
                                     destination, IPTV_HLS_URL_BYTES);
            if (url_result != IPTV_HLS_OK)
                return fail(playlist, url_result, line_number);

            if (pending_variant)
            {
                if (playlist->variant_count >= limits.max_variants)
                    return fail(playlist, IPTV_HLS_OUTPUT_LIMIT, line_number);
                playlist->variants[playlist->variant_count++] = variant;
                pending_variant = false;
            }
            else
            {
                if (has_fmp4_extension(destination))
                    return fail(playlist, IPTV_HLS_UNSUPPORTED_FMP4, line_number);
                if (playlist->media_sequence > UINT64_MAX - playlist->segment_count)
                    return fail(playlist, IPTV_HLS_MALFORMED, line_number);
                iptv_hls_segment_t &segment = playlist->segments[playlist->segment_count];
                segment.sequence = playlist->media_sequence + playlist->segment_count;
                segment.duration_ms = pending_duration_ms;
                segment.discontinuity = pending_discontinuity ? 1u : 0u;
                segment.discontinuity_sequence = current_discontinuity;
                ++playlist->segment_count;
                pending_segment = false;
                pending_discontinuity = false;
                segments_seen = true;
            }
            continue;
        }

        if (line.data[0] != '#')
            return fail(playlist, IPTV_HLS_MALFORMED, line_number);

        slice_t value{};
        if (tag_value(line, "#EXT-X-STREAM-INF:", &value))
        {
            if (media_seen)
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            master_seen = true;
            const iptv_hls_result_t variant_result = parse_variant(value, limits, &variant);
            if (variant_result != IPTV_HLS_OK)
                return fail(playlist, variant_result, line_number);
            pending_variant = true;
        }
        else if (tag_value(line, "#EXTINF:", &value))
        {
            if (master_seen || pending_segment)
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            media_seen = true;
            if (playlist->segment_count >= limits.max_segments)
                return fail(playlist, IPTV_HLS_OUTPUT_LIMIT, line_number);
            if (!parse_duration_ms(value, &pending_duration_ms))
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            pending_segment = true;
        }
        else if (tag_value(line, "#EXT-X-MEDIA-SEQUENCE:", &value))
        {
            if (master_seen || segments_seen || !parse_u64(value, &playlist->media_sequence))
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            media_seen = true;
        }
        else if (tag_value(line, "#EXT-X-TARGETDURATION:", &value))
        {
            uint64_t seconds = 0;
            if (master_seen || segments_seen || !parse_u64(value, &seconds) || !seconds ||
                seconds > UINT32_MAX / 1000u)
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            media_seen = true;
            playlist->target_duration_ms = static_cast<uint32_t>(seconds * 1000u);
        }
        else if (tag_value(line, "#EXT-X-DISCONTINUITY-SEQUENCE:", &value))
        {
            if (master_seen || segments_seen || pending_discontinuity ||
                !parse_u64(value, &current_discontinuity))
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            media_seen = true;
            playlist->discontinuity_sequence = current_discontinuity;
        }
        else if (equal(line, "#EXT-X-DISCONTINUITY"))
        {
            if (master_seen || current_discontinuity == UINT64_MAX)
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            media_seen = true;
            ++current_discontinuity;
            pending_discontinuity = true;
        }
        else if (equal(line, "#EXT-X-ENDLIST"))
        {
            if (master_seen)
                return fail(playlist, IPTV_HLS_MALFORMED, line_number);
            media_seen = true;
            playlist->is_live = 0u;
        }
    }

    if (!header_seen || pending_variant || pending_segment || pending_discontinuity)
        return fail(playlist, IPTV_HLS_MALFORMED, line_number);
    if (master_seen)
    {
        playlist->kind = IPTV_HLS_KIND_MASTER;
        if (!playlist->variant_count)
            return fail(playlist, IPTV_HLS_MALFORMED, line_number);
        return fail(playlist, select_variant(limits, playlist), 0u);
    }
    if (media_seen)
    {
        playlist->kind = IPTV_HLS_KIND_MEDIA;
        if (!playlist->target_duration_ms || !playlist->segment_count)
            return fail(playlist, IPTV_HLS_MALFORMED, line_number);
        return IPTV_HLS_OK;
    }
    return fail(playlist, IPTV_HLS_MALFORMED, line_number);
}

extern "C" const char *iptv_hls_result_name(iptv_hls_result_t result)
{
    switch (result)
    {
    case IPTV_HLS_OK:
        return "ok";
    case IPTV_HLS_INVALID_ARGUMENT:
        return "invalid argument";
    case IPTV_HLS_INPUT_LIMIT:
        return "input limit";
    case IPTV_HLS_LINE_LIMIT:
        return "line limit";
    case IPTV_HLS_OUTPUT_LIMIT:
        return "output limit";
    case IPTV_HLS_MALFORMED:
        return "malformed playlist";
    case IPTV_HLS_INVALID_URL:
        return "invalid URL";
    case IPTV_HLS_URL_LIMIT:
        return "URL limit";
    case IPTV_HLS_UNSUPPORTED_ENCRYPTION:
        return "unsupported encryption";
    case IPTV_HLS_UNSUPPORTED_BYTE_RANGE:
        return "unsupported byte range";
    case IPTV_HLS_UNSUPPORTED_FMP4:
        return "unsupported fMP4";
    case IPTV_HLS_UNSUPPORTED_CODEC:
        return "unsupported codec";
    case IPTV_HLS_NO_VARIANT_WITHIN_LIMITS:
        return "no variant within limits";
    case IPTV_HLS_UNSUPPORTED_FEATURE:
        return "unsupported HLS feature";
    default:
        return "unknown HLS result";
    }
}
