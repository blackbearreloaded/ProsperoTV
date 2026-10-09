// ProsperoTV - Persistent parental PIN and category policy.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ptv
{
bool adult_category(std::string_view category);
bool kids_category(std::string_view category);

// The session unlock is deliberately never written to disk.
class Parental
{
  public:
    bool load(std::string file);
    bool enabled() const
    {
        return enabled_;
    }
    bool valid() const
    {
        return valid_;
    }
    bool unlocked() const
    {
        return valid_ && (!enabled_ || unlocked_);
    }
    bool kids_only() const
    {
        return kids_;
    }
    bool set_pin(std::string_view pin);
    bool unlock(std::string_view pin, std::uint64_t now);
    void lock()
    {
        unlocked_ = false;
    }
    bool set_kids(bool enabled);
    bool remove();
    std::uint64_t wait_until() const
    {
        return until_;
    }
    static bool valid_pin(std::string_view pin);

  private:
    bool save();
    std::string file_;
    std::array<unsigned char, 16> salt_{};
    std::array<unsigned char, 32> hash_{};
    bool valid_ = true, enabled_ = false, unlocked_ = false, kids_ = false;
    unsigned failures_ = 0;
    std::uint64_t until_ = 0;
};
} // namespace ptv
