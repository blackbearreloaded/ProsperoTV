// ProsperoTV - A session sleep deadline shared by the menu and player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace ptv
{
class SleepTimer
{
  public:
    static constexpr std::array<unsigned, 6> durations{0, 15, 30, 60, 90, 120};
    void arm(unsigned choice, std::uint64_t now)
    {
        choice_ = now && choice < durations.size() ? choice : 0;
        const auto interval = durations[choice_] * UINT64_C(60000000);
        if (interval > UINT64_MAX - now)
            choice_ = 0;
        deadline_ = choice_ ? now + interval : 0;
        sleeping_ = false;
    }
    bool expire(std::uint64_t now)
    {
        if (!deadline_ || now < deadline_)
            return false;
        deadline_ = 0;
        choice_ = 0;
        sleeping_ = true;
        return true;
    }
    void wake()
    {
        sleeping_ = false;
    }
    bool sleeping() const
    {
        return sleeping_;
    }
    unsigned choice() const
    {
        return choice_;
    }
    std::uint64_t deadline() const
    {
        return deadline_;
    }
    unsigned remaining_minutes(std::uint64_t now) const
    {
        return deadline_ > now ? static_cast<unsigned>((deadline_ - now + 59999999) / 60000000) : 0;
    }

  private:
    unsigned choice_ = 0;
    std::uint64_t deadline_ = 0;
    bool sleeping_ = false;
};
} // namespace ptv
