// ProsperoTV - Bounded live transport history, independent of the decoder.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

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
    };
    explicit Timeshift(std::size_t bytes = max_bytes, std::uint64_t duration_us = max_duration_us);
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
    void set_video_pid(std::uint32_t pid);
    // A provider reconnect/discontinuity starts a new retained timeline.
    void discontinuity();

  private:
    struct Mark
    {
        std::uint64_t offset, pts_us;
        bool random_access;
    };
    void index();
    void trim();
    void packet(const std::uint8_t *data, std::uint64_t offset);
    void copy(std::uint64_t offset, std::uint8_t *out, std::size_t bytes) const;
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
