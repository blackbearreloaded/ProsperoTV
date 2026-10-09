// ProsperoTV - Selected subtitle decoding and presentation-time cues.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iptv
{
enum class SubtitleCodec
{
    none,
    text,
    subrip,
    ass,
    mov_text,
    webvtt,
    dvb,
    dvd,
    pgs
};
enum class SubtitleError
{
    none,
    unavailable,
    malformed,
    limit
};
struct SubtitleTrackInfo
{
    std::uint32_t id = 0; // Zero means Off. IDs are stable within one source session.
    SubtitleCodec codec = SubtitleCodec::none;
    std::string language, title;
    bool forced = false, hearing_impaired = false;
    bool operator==(const SubtitleTrackInfo &) const = default;
};
struct SubtitleTrack
{
    SubtitleTrackInfo info;
    std::vector<std::uint8_t> extra;
    unsigned width = 0, height = 0;
    bool operator==(const SubtitleTrack &) const = default;
};
struct SubtitleBitmap
{
    int x = 0, y = 0;
    unsigned width = 0, height = 0;
    std::vector<std::uint32_t> argb; // Straight alpha, AARRGGBB values.
};
struct SubtitleCue
{
    std::int64_t start_us = 0, end_us = 0;
    unsigned canvas_width = 0, canvas_height = 0;
    std::string text;
    std::vector<SubtitleBitmap> bitmaps;
};
struct SubtitleState
{
    std::vector<SubtitleTrackInfo> tracks;
    std::uint32_t selected = 0;
    SubtitleError error = SubtitleError::none;
};
// Conversion at the container reader boundary; no FFmpeg headers leak into the UI.
SubtitleCodec subtitle_codec(int av_codec_id);

class Subtitles
{
  public:
    static constexpr std::size_t max_tracks = 32, max_packet = 256 * 1024;
    Subtitles();
    ~Subtitles();
    Subtitles(const Subtitles &) = delete;
    Subtitles &operator=(const Subtitles &) = delete;
    // Thread-safe. Provider updates preserve a selection only while its ID exists.
    void set_tracks(std::vector<SubtitleTrack> tracks);
    SubtitleState state() const;
    bool select(std::uint32_t id);
    // Packet timestamps and durations use the same microsecond timeline as video.
    // Bad subtitles are isolated: false never requires stopping video playback.
    bool push(std::uint32_t id, const std::uint8_t *data, std::size_t bytes, std::int64_t pts_us,
              std::int64_t duration_us);
    std::vector<std::shared_ptr<const SubtitleCue>> at(std::int64_t video_pts_us);
    // Rebuild selected decoder/cues from retained packets on a same-timeline seek.
    bool seek(std::int64_t video_pts_us);
    void reset_timeline();
    void clear();

  private:
    bool replay_locked();
    bool decode_locked(std::uint32_t id, const std::uint8_t *data, std::size_t bytes,
                       std::int64_t pts_us, std::int64_t duration_us);
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace iptv
