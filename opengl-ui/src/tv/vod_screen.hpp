// ProsperoTV - Movies, nested provider categories, shows and seasons.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tv/shared.hpp"
#include "ui/glyphs.hpp"
namespace ptv
{
class VodScreen
{
  public:
    explicit VodScreen(Shared &shared) : shared_(shared)
    {
    }
    void enter();
    void update();
    void handle(const InputFrame &, ui::Feedback &);
    bool back();
    void refresh();
    void draw(ui::Canvas &) const;
    std::vector<std::string> image_urls() const;
    int hints(ui::Hint *, int) const;

  private:
    enum class Kind
    {
        movies,
        shows,
        all,
        category,
        item
    };
    struct Row
    {
        Kind kind;
        std::string text, category;
        unsigned item = 0;
    };
    void sync();
    const Row *focused() const;
    Shared &shared_;
    std::vector<Row> rows_;
    unsigned revision_ = ~0u, model_revision_ = ~0u;
    int top_ = 0;
};
} // namespace ptv
