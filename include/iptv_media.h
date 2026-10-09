// ProsperoTV - MP4/Matroska into the existing native transport-stream player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iptv_subtitles.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace iptv
{
// All callbacks run on the caller's thread. Read returns bytes, 0 at EOF or
// -1 on failure. Seek is absolute; size returns -1 when unknown.
struct MediaInput
{
    void *context = nullptr;
    int (*read)(void *, std::uint8_t *, int) = nullptr;
    std::int64_t (*seek)(void *, std::int64_t) = nullptr;
    std::int64_t (*size)(void *) = nullptr;
    bool (*cancelled)(void *) = nullptr;
    // HLS only: the application opens HTTP(S) resources with its own credential
    // policy. Range end is exclusive; -1 means no end. A successfully returned
    // resource belongs to the reader until its close callback. FFmpeg never
    // opens sockets or local files directly. A returned resource may set url to
    // its final URL after redirects (valid until close), for relative segments.
    // Leave both fields empty for ordinary files.
    const char *url = nullptr;
    bool (*open_resource)(void *, const char *, std::int64_t begin, std::int64_t end,
                          MediaInput *) = nullptr;
    void (*close)(void *) = nullptr;
};
struct MediaAudioTrack
{
    std::uint32_t pid = 0;
    std::string language, title;
    std::uint8_t audio_type = 0; // ISO 639 accessibility type, as in a TS descriptor.
};
struct MediaOutput
{
    void *context = nullptr;
    bool (*write)(void *, const std::uint8_t *, std::size_t) = nullptr;
    void (*subtitle_tracks)(void *, const std::vector<SubtitleTrack> &) = nullptr;
    void (*subtitle_packet)(void *, std::uint32_t, const std::uint8_t *, std::size_t,
                            std::int64_t pts_us, std::int64_t duration_us) = nullptr;
    void (*audio_tracks)(void *, const std::vector<MediaAudioTrack> &) = nullptr;
};
// 0: complete, 1: cancelled, -1: error. Streaming I/O, no transcoding.
int ReadMedia(const MediaInput &input, const MediaOutput &output, std::string *error);
bool LooksLikeMedia(const std::uint8_t *bytes, std::size_t count);
// HLS WebVTT uses a local cue clock mapped to a wrapping 90 kHz transport clock.
// Normalize before FFmpeg reads the segment (its WebVTT demuxer ignores the map).
bool NormalizeHlsWebVtt(std::string_view segment, std::string &normalized);
} // namespace iptv
