// ProsperoTV - Channel and time grid.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tv/shared.hpp"

namespace ptv
{
class GuideSheet
{
  public:
    explicit GuideSheet(Shared &shared) : shared_(shared)
    {
    }
    void open(std::string_view channel, ui::Feedback &feedback);
    bool is_open() const
    {
        return open_;
    }
    void dismiss()
    {
        open_ = false;
    }
    void handle(const InputFrame &input, ui::Feedback &feedback);
    void update();
    void draw(ui::Canvas &canvas) const;
    unsigned row() const
    {
        return row_;
    }
    std::int64_t time() const
    {
        return time_;
    }

  private:
    const Programme *selected() const;
    void keep_in_view();
    Shared &shared_;
    bool open_ = false;
    unsigned row_ = 0, top_ = 0, revision_ = 0;
    std::string channel_;
    std::int64_t time_ = 0, left_ = 0;
};
} // namespace ptv
