// ProsperoTV - MP4/Matroska into the existing native transport-stream player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace iptv
{
// All callbacks run on the caller's thread. Read returns bytes, 0 at EOF or
// -1 on failure. Seek is absolute; size returns -1 when unknown. No URLs,
// credentials or filesystem access are given to FFmpeg.
struct MediaInput
{
    void *context = nullptr;
    int (*read)(void *, std::uint8_t *, int) = nullptr;
    std::int64_t (*seek)(void *, std::int64_t) = nullptr;
    std::int64_t (*size)(void *) = nullptr;
    bool (*cancelled)(void *) = nullptr;
};
struct MediaOutput
{
    void *context = nullptr;
    bool (*write)(void *, const std::uint8_t *, std::size_t) = nullptr;
};
// 0: complete, 1: cancelled, -1: error. Streaming I/O, no transcoding.
int ReadMedia(const MediaInput &input, const MediaOutput &output, std::string *error);
bool LooksLikeMedia(const std::uint8_t *bytes, std::size_t count);
} // namespace iptv
