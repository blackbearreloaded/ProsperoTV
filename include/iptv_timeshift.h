// ProsperoTV - Bounded live transport history, independent of the decoder.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iptv_stream.h"
#include "iptv_webm.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace iptv
{
class Timeshift
{
  public:
    static constexpr std::size_t max_bytes = 512u * 1024u * 1024u;
    static constexpr std::uint64_t max_duration_us = 300000000;
    struct Position
    {
        std::uint64_t offset = 0, pts_us = 0, generation = 0;
    };
    struct Range
    {
        std::uint64_t begin = 0, end = 0, first_pts_us = 0, last_pts_us = 0, generation = 0;
        bool timed = false;
        int replay_result = IPTV_STREAM_OK;
    };
    enum class ReadStatus
    {
        data,
        empty,
        expired
    };
    struct Read
    {
        ReadStatus status = ReadStatus::empty;
        std::size_t bytes = 0;
        std::uint64_t generation = 0; // Captured under the same lock as the byte copy.
    };
    explicit Timeshift(std::size_t bytes = max_bytes, std::uint64_t duration_us = max_duration_us);
    ~Timeshift();
    // Enable before the first append. Settings are parsed on download, with no
    // decoder callbacks, so unread headers survive byte-ring expiry.
    bool enable_replay();
    // Direct VP9 keeps complete coded packets and their setup in the same ring.
    // Select this instead of transport replay, before appending any input.
    bool enable_webm();
    bool append_webm(const iptv_webm_video_info_t &video, const iptv_webm_block_t &block);
    // offset must be a seek result or the end of the previous returned record.
    // block.data borrows output until the next read; bytes includes the record header.
    Read read_webm(std::uint64_t offset, iptv_webm_video_info_t &video,
                   iptv_webm_block_t &block, std::vector<std::uint8_t> &output) const;
    // Owner-thread only, after interrupting native submission. The history lock
    // protects validation and copying through the playback reset callback.
    int reposition(iptv_stream_session_t *playback, const Position &position);
    bool available() const
    {
        return storage_ != nullptr;
    }
    // One download producer, one decoder consumer; snapshots/seeks may also run
    // on the controls thread. Copies leave the lock before calling the decoder.
    bool append(const std::uint8_t *data, std::size_t bytes);
    Read read(std::uint64_t offset, std::uint8_t *output, std::size_t capacity) const;
    Range range() const;
    std::optional<Position> seek(std::uint64_t pts_us) const;
    // Resume a changed decoder setup without seeking back into the old format.
    // No result until that point has complete retained configuration metadata.
    std::optional<Position> seek_next(std::uint64_t pts_us) const;
    void set_video_pid(std::uint32_t pid);
    // A provider reconnect/discontinuity starts a new retained timeline.
    void discontinuity();
    // Keep transport and decoder-configuration retention boundaries together.
    void discard_before(std::uint64_t pts_us, std::uint64_t generation);

  private:
    struct Mark
    {
        std::uint64_t offset, pts_us;
        bool random_access;
    };
    void index();
    void trim();
    Range range_locked() const;
    void packet(const std::uint8_t *data, std::uint64_t offset);
    void copy(std::uint64_t offset, std::uint8_t *out, std::size_t bytes) const;
    void write(const std::uint8_t *data, std::size_t bytes);
    struct Unmap
    {
        std::size_t bytes;
        void operator()(std::uint8_t *data) const;
    };
    mutable std::mutex mutex_;
    std::unique_ptr<std::uint8_t[], Unmap> storage_;
    std::size_t capacity_;
    std::uint64_t duration_, begin_ = 0, end_ = 0, scan_ = 0, generation_ = 0;
    std::uint32_t video_pid_ = 0, indexed_pid_ = 0;
    std::int64_t ticks_ = 0;
    std::uint64_t raw_ticks_ = 0, latest_us_ = 0;
    bool synchronized_ = false, have_ticks_ = false;
    std::deque<Mark> marks_;
    iptv_stream_session_t metadata_{};
    bool replay_enabled_ = false;
    bool webm_enabled_ = false;
    iptv_webm_video_info_t webm_video_{};
    int replay_result_ = IPTV_STREAM_OK;
};

// Controls keep their requested position until a picture from the new decoder
// timeline arrives. The caller serializes requests and acknowledgements.
class TimeshiftSeek
{
  public:
    bool relative(std::uint64_t first, std::uint64_t last, std::uint64_t presented, int seconds);
    void live(std::uint64_t last);
    std::optional<std::uint64_t> take();
    // UINT64_MAX means there is no picture from the current decoder timeline yet.
    void acknowledge(std::uint64_t presented);

  private:
    std::optional<std::uint64_t> target_;
    bool pending_ = false;
};
} // namespace iptv
