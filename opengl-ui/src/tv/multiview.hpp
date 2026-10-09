// ProsperoTV - Two or four live channels with one audible tile.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tv/shared.hpp"
#include <array>

namespace ptv
{
class Multiview
{
  public:
    explicit Multiview(Shared &shared) : shared_(shared)
    {
    }
    ~Multiview()
    {
        stop();
    }
    void configure(LivePreview::Upload upload, ImageCache::Release release);
    void open(std::string channel);
    void stop();
    void handle(const InputFrame &input, ui::Feedback &feedback);
    bool choose(std::string channel);
    void cancel_choice()
    {
        choosing_ = false;
    }
    void update(float dt);
    void draw(ui::Canvas &canvas) const;
    bool active() const
    {
        return active_;
    }
    bool is_open() const
    {
        return active_ && !choosing_;
    }
    bool choosing() const
    {
        return active_ && choosing_;
    }
    unsigned count() const
    {
        return count_;
    }
    unsigned selected() const
    {
        return selected_;
    }
    unsigned audible_count() const;
    std::string_view channel(unsigned slot) const
    {
        return slot < channels_.size() ? std::string_view(channels_[slot]) : std::string_view();
    }

  private:
    Shared &shared_;
    std::array<LivePreview, 4> previews_;
    std::array<std::string, 4> channels_, names_;
    unsigned count_ = 2, selected_ = 0;
    bool active_ = false, choosing_ = false, muted_ = false;
};
} // namespace ptv
