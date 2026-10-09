// ProsperoTV - A five-minute transport ring; no disk writes or provider secrets.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_timeshift.h"
#include <algorithm>
#include <cstring>
#include <sys/mman.h>

namespace iptv
{
namespace
{
constexpr std::size_t packet_bytes = 188;
}
bool TimeshiftSeek::relative(std::uint64_t first, std::uint64_t last, std::uint64_t presented,
                             int seconds)
{
    if (first > last)
        return false;
    const auto position = std::clamp(target_.value_or(presented), first, last);
    const auto delta = static_cast<std::int64_t>(seconds) * 1000000;
    target_ = delta < 0 ? position - std::min(position - first, static_cast<std::uint64_t>(-delta))
                        : position + std::min(last - position, static_cast<std::uint64_t>(delta));
    pending_ = true;
    return true;
}
void TimeshiftSeek::live(std::uint64_t last)
{
    target_ = last;
    pending_ = true;
}
std::optional<std::uint64_t> TimeshiftSeek::take()
{
    if (!pending_)
        return {};
    pending_ = false;
    return target_;
}
void TimeshiftSeek::acknowledge(std::uint64_t presented)
{
    if (!pending_ && presented != UINT64_MAX)
        target_.reset();
}
Timeshift::Timeshift(std::size_t bytes, std::uint64_t duration_us)
    : storage_(nullptr, Unmap{bytes}), capacity_(bytes), duration_(duration_us)
{
    if (bytes >= 16 * packet_bytes && bytes <= max_bytes && duration_us &&
        duration_us <= max_duration_us)
    {
        // The history has its own bounded mapping. A full ring must not consume
        // the application heap needed by catalogues and container indexes.
        void *data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (data != MAP_FAILED)
            storage_.reset(static_cast<std::uint8_t *>(data));
    }
}
void Timeshift::Unmap::operator()(std::uint8_t *data) const
{
    (void)munmap(data, bytes);
}
void Timeshift::copy(std::uint64_t offset, std::uint8_t *out, std::size_t bytes) const
{
    const auto at = static_cast<std::size_t>(offset % capacity_);
    const auto first = std::min(bytes, capacity_ - at);
    std::memcpy(out, storage_.get() + at, first);
    if (first < bytes)
        std::memcpy(out + first, storage_.get(), bytes - first);
}
bool Timeshift::append(const std::uint8_t *data, std::size_t bytes)
{
    if (!available() || (!data && bytes))
        return false;
    std::lock_guard lock(mutex_);
    if (bytes > UINT64_MAX - end_)
        return false;
    if (!bytes)
        return true;
    // A giant caller chunk keeps its newest bytes, just as ordinary overwrite.
    if (bytes > capacity_)
    {
        end_ += bytes - capacity_;
        data += bytes - capacity_;
        bytes = capacity_;
    }
    const auto at = static_cast<std::size_t>(end_ % capacity_);
    const auto first = std::min(bytes, capacity_ - at);
    std::memcpy(storage_.get() + at, data, first);
    if (first < bytes)
        std::memcpy(storage_.get(), data + first, bytes - first);
    end_ += bytes;
    if (end_ - begin_ > capacity_)
        begin_ = end_ - capacity_;
    if (scan_ < begin_)
    {
        scan_ = begin_;
        synchronized_ = false;
    }
    index();
    trim();
    return true;
}
Timeshift::Read Timeshift::read(std::uint64_t offset, std::uint8_t *output,
                                std::size_t capacity) const
{
    std::lock_guard lock(mutex_);
    if (!available() || offset < begin_ || offset > end_)
        return {ReadStatus::expired, 0};
    if (!output || !capacity || offset == end_)
        return {};
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, end_ - offset));
    copy(offset, output, count);
    return {ReadStatus::data, count};
}
Timeshift::Range Timeshift::range() const
{
    std::lock_guard lock(mutex_);
    Range result{begin_, end_, 0, latest_us_, generation_, !marks_.empty()};
    if (!marks_.empty())
        result.first_pts_us = marks_.front().pts_us;
    return result;
}
std::optional<Timeshift::Position> Timeshift::seek(std::uint64_t target) const
{
    std::lock_guard lock(mutex_);
    if (marks_.empty())
        return {};
    target = std::clamp(target, marks_.front().pts_us, latest_us_);
    const Mark *random = nullptr, *fallback = &marks_.front();
    const auto lead = target > 5000000 ? target - 5000000 : 0;
    for (const auto &mark : marks_)
    {
        if (mark.pts_us <= lead && mark.pts_us >= fallback->pts_us)
            fallback = &mark;
        if (mark.random_access && mark.pts_us <= target &&
            (!random || mark.pts_us >= random->pts_us))
            random = &mark;
    }
    // Some broadcasters omit the TS random-access indicator. Starting a few
    // seconds earlier lets the existing Annex-B parser find an IDR/CRA itself.
    const auto *chosen = random && target - random->pts_us <= 10000000 ? random : fallback;
    return Position{chosen->offset, chosen->pts_us, generation_};
}
void Timeshift::set_video_pid(std::uint32_t pid)
{
    if (!pid || pid >= 0x1fff)
        return;
    std::lock_guard lock(mutex_);
    if (pid == video_pid_)
        return;
    video_pid_ = pid;
    if (pid == indexed_pid_)
        return; // Preserve the wrapping clock when the demuxer confirms this PID.
    if (indexed_pid_)
        ++generation_;
    indexed_pid_ = pid;
    marks_.clear();
    scan_ = begin_;
    synchronized_ = have_ticks_ = false;
    latest_us_ = 0;
    index();
    trim();
}
void Timeshift::discontinuity()
{
    std::lock_guard lock(mutex_);
    begin_ = scan_ = end_;
    marks_.clear();
    synchronized_ = have_ticks_ = false;
    latest_us_ = 0;
    ++generation_;
}
void Timeshift::trim()
{
    const auto first_time = latest_us_ > duration_ ? latest_us_ - duration_ : 0;
    bool evicted = false;
    while (!marks_.empty() && (marks_.front().offset < begin_ ||
                               marks_.front().pts_us < first_time || marks_.size() > 32768))
    {
        marks_.pop_front();
        evicted = true;
    }
    if (evicted && !marks_.empty())
        begin_ = std::max(begin_, marks_.front().offset);
}
void Timeshift::discard_before(std::uint64_t pts_us, std::uint64_t generation)
{
    std::lock_guard lock(mutex_);
    if (generation != generation_)
        return; // A producer reset must not apply an old clock's retention floor.
    bool discarded = false;
    while (!marks_.empty() && marks_.front().pts_us < pts_us)
    {
        marks_.pop_front();
        discarded = true;
    }
    if (discarded)
        begin_ = std::max(begin_, marks_.empty() ? end_ : marks_.front().offset);
}
void Timeshift::index()
{
    std::uint8_t bytes[packet_bytes];
    while (end_ - scan_ >= packet_bytes)
    {
        copy(scan_, bytes, packet_bytes);
        if (bytes[0] != 0x47)
        {
            synchronized_ = false;
            ++scan_;
            continue;
        }
        if (!synchronized_)
        {
            if (end_ - scan_ < 2 * packet_bytes)
                break;
            std::uint8_t next = 0;
            copy(scan_ + packet_bytes, &next, 1);
            if (next != 0x47)
            {
                ++scan_;
                continue;
            }
            synchronized_ = true;
        }
        packet(bytes, scan_);
        scan_ += packet_bytes;
    }
}
void Timeshift::packet(const std::uint8_t *p, std::uint64_t offset)
{
    if ((p[1] & 0xc0) != 0x40 || (p[3] & 0xc0) || !(p[3] & 0x10))
        return; // Timed PES starts only; reject damaged/scrambled packets.
    const auto pid = static_cast<std::uint32_t>(((p[1] & 0x1f) << 8) | p[2]);
    if (!pid || pid == 0x1fff || (indexed_pid_ && pid != indexed_pid_))
        return;
    std::size_t at = 4;
    bool random = false;
    if (p[3] & 0x20)
    {
        if (p[4] > 182)
            return;
        random = p[4] && (p[5] & 0x40);
        at += 1 + p[4];
    }
    if (at + 14 > packet_bytes || p[at] || p[at + 1] || p[at + 2] != 1 || p[at + 3] < 0xe0 ||
        p[at + 3] > 0xef || (p[at + 6] & 0xc0) != 0x80 || !(p[at + 7] & 0x80) || p[at + 8] < 5)
        return;
    const auto *pts = p + at + 9;
    if (((pts[0] >> 4) != 2 && (pts[0] >> 4) != 3) || !(pts[0] & 1) || !(pts[2] & 1) ||
        !(pts[4] & 1))
        return;
    // Before the PMT selects a PID, follow one video clock, never interleave
    // clocks from different programmes in a multiplex.
    indexed_pid_ = pid;
    const auto raw = (static_cast<std::uint64_t>((pts[0] >> 1) & 7) << 30) |
                     (static_cast<std::uint64_t>(pts[1]) << 22) |
                     (static_cast<std::uint64_t>(pts[2] >> 1) << 15) |
                     (static_cast<std::uint64_t>(pts[3]) << 7) | (pts[4] >> 1);
    constexpr std::uint64_t wrap = UINT64_C(1) << 33;
    auto delta = static_cast<std::int64_t>((raw - raw_ticks_) & (wrap - 1));
    if (delta >= static_cast<std::int64_t>(wrap / 2))
        delta -= wrap;
    if (have_ticks_ && (delta < -900000 || delta > 2700000))
    {
        // A broadcaster reset its clock. Old bytes belong to a different
        // timeline; consumers see the generation change even if already ahead.
        marks_.clear();
        begin_ = offset;
        latest_us_ = 0;
        have_ticks_ = false;
        ++generation_;
    }
    ticks_ = have_ticks_ ? ticks_ + delta : static_cast<std::int64_t>(raw);
    raw_ticks_ = raw;
    have_ticks_ = true;
    if (ticks_ < 0 || ticks_ > INT64_MAX / 1000000)
        return;
    const auto us = static_cast<std::uint64_t>(ticks_) * 1000000 / 90000;
    latest_us_ = std::max(latest_us_, us);
    marks_.push_back({offset, us, random});
    trim(); // Bound index memory even when append receives a whole large segment.
}
} // namespace iptv
