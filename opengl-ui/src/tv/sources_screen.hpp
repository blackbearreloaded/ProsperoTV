// ProsperoTV - Sources: where the channel list comes from, and how it is doing.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "tv/shared.hpp"
#include "ui/components/list.hpp"
#include "ui/glyphs.hpp"

namespace ptv
{

class SourcesScreen
{
  public:
    explicit SourcesScreen(Shared &shared);

    // Replays the entrance, with the focus where it was.
    void enter();
    // Everything but Back.
    void handle(const InputFrame &input, ui::Feedback &feedback);
    void update(float dt);
    void draw(ui::Canvas &canvas) const;
    int hints(ui::Hint *out, int capacity) const;
    std::int64_t focused_id() const;

  private:
    void sync();

    Shared &shared_;
    ui::ListView list_;
    unsigned seen_revision_ = 0;
    std::vector<std::int64_t> ids_;
};

} // namespace ptv
