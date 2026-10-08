// ProsperoTV - Channel and time grid.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/guide_sheet.hpp"
#include "tv/draw.hpp"
#include "tv/platform.hpp"
#include <algorithm>

namespace ptv
{
namespace
{
constexpr unsigned kRows = 7;
constexpr std::int64_t kWindow = 2 * 3600;
constexpr float kTimeX = 435, kTimeWidth = 1360, kRowY = 304, kRowHeight = 70;
} // namespace
void GuideSheet::open(std::string_view channel, ui::Feedback &feedback)
{
    open_ = true;
    channel_ = channel;
    row_ = static_cast<unsigned>(std::max(0, shared_.model.position_of(channel)));
    top_ = row_;
    time_ = static_cast<std::int64_t>(platform::unix_time());
    left_ = time_ / 1800 * 1800;
    revision_ = shared_.model.revision();
    keep_in_view();
    feedback.play(audio::Cue::open);
}
void GuideSheet::keep_in_view()
{
    const auto &model = shared_.model;
    row_ = std::min(row_, model.visible_count() ? model.visible_count() - 1 : 0);
    if (row_ < top_)
        top_ = row_;
    if (row_ >= top_ + kRows)
        top_ = row_ - kRows + 1;
    if (time_ < left_ || time_ >= left_ + kWindow)
        left_ = time_ / 1800 * 1800;
    if (model.visible_count())
        channel_ = model.channel(model.visible(row_)).id;
}
void GuideSheet::update()
{
    if (!open_)
        return;
    if (revision_ != shared_.model.revision())
    {
        row_ = static_cast<unsigned>(std::max(0, shared_.model.position_of(channel_)));
        revision_ = shared_.model.revision();
    }
    keep_in_view();
}
const Programme *GuideSheet::selected() const
{
    const auto &model = shared_.model;
    if (row_ >= model.visible_count())
        return nullptr;
    return model.guide().now(model.channel(model.visible(row_)).id, time_);
}
void GuideSheet::handle(const InputFrame &input, ui::Feedback &feedback)
{
    auto &model = shared_.model;
    if (input.is_pressed(Action::back) || input.is_pressed(Action::r3))
    {
        dismiss();
        feedback.play(audio::Cue::back);
        return;
    }
    if (input.is_pressed(Action::menu))
    {
        model.refresh_guide();
        return;
    }
    if (input.is_pressed(Action::north))
    {
        model.ask_query();
        return;
    }
    if (input.is_pressed(Action::west))
    {
        time_ = static_cast<std::int64_t>(platform::unix_time());
        keep_in_view();
        return;
    }
    if (input.is_pressed(Action::confirm) && row_ < model.visible_count())
    {
        const auto *p = selected();
        const bool playing =
            p ? model.play_programme(model.visible(row_), *p) : model.play(model.visible(row_));
        if (playing)
            dismiss();
        else
            feedback.play(audio::Cue::error);
        return;
    }
    const auto previous = time_;
    const auto row = row_;
    if (input.nav == Direction::up && row_)
        --row_;
    if (input.nav == Direction::down && row_ + 1 < model.visible_count())
        ++row_;
    if (input.is_pressed(Action::jump_prev))
        row_ = row_ > kRows ? row_ - kRows : 0;
    if (input.is_pressed(Action::jump_next))
        row_ += kRows;
    if (input.is_pressed(Action::page_prev))
        time_ -= 86400;
    if (input.is_pressed(Action::page_next))
        time_ += 86400;
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const auto *p = selected();
        time_ = p ? input.nav == Direction::left ? p->start - 1 : p->end
                  : time_ + (input.nav == Direction::left ? -1800 : 1800);
    }
    const auto now = static_cast<std::int64_t>(platform::unix_time());
    time_ = std::clamp(time_, now - 14 * 86400, now + 14 * 86400);
    keep_in_view();
    if (previous != time_ || row != row_)
        feedback.play(audio::Cue::focus);
}
void GuideSheet::draw(ui::Canvas &canvas) const
{
    if (!open_)
        return;
    const auto &model = shared_.model;
    auto &list = canvas.list;
    const auto &fonts = canvas.fonts;
    const auto &theme = shared_.theme;
    list.rounded_rect({0, 0, kWidth, kHeight}, 0, tone::night.with_alpha(0.98f));
    ui::text(list, fonts.display, "Programme guide", kMargin, 155, 48, theme.text);
    ui::text(list, fonts.semibold, programme_time(time_, true), kWidth - kMargin, 155, 27,
             tone::accent, gfx::Align::right);
    ui::text(list, fonts.regular, model.guide_status(), kMargin, 205, 22, theme.text_muted);
    if (!model.query().empty())
        ui::text(list, fonts.regular,
                 "Search: " + model.query() + " (channels or programmes on now)", kMargin, 245, 22,
                 tone::accent);
    for (int tick = 0; tick <= 4; ++tick)
    {
        const float x = kTimeX + kTimeWidth * static_cast<float>(tick) / 4;
        ui::text(list, fonts.mono, programme_time(left_ + tick * 1800), x, 283, 20,
                 theme.text_muted, tick == 4 ? gfx::Align::right : gfx::Align::left);
    }
    for (unsigned row = top_; row < model.visible_count() && row < top_ + kRows; ++row)
    {
        const auto channel = model.channel(model.visible(row));
        const float y = kRowY + static_cast<float>(row - top_) * kRowHeight;
        const bool focus = row == row_;
        list.rounded_rect({kMargin, y, kTimeX - kMargin - 10, kRowHeight - 5}, 9,
                          kWhite.with_alpha(focus ? 0.16f : 0.04f));
        const auto name = shown_name(fonts, channel);
        const auto &face = face_for(fonts, fonts.semibold, name);
        ui::text(list, face, face.font->fit(name, 22, kTimeX - kMargin - 40), kMargin + 15, y + 40,
                 22, theme.text);
        list.rounded_rect({kTimeX, y, kTimeWidth, kRowHeight - 5}, 8, kWhite.with_alpha(0.025f));
        const auto programmes = model.guide().programmes(channel.id);
        bool any = false;
        for (const auto &p : programmes)
        {
            const auto end = p.end > p.start ? p.end : p.start + 1800;
            if (end <= left_)
                continue;
            if (p.start >= left_ + kWindow)
                break;
            any = true;
            const float x = kTimeX + kTimeWidth *
                                         static_cast<float>(std::max(p.start, left_) - left_) /
                                         static_cast<float>(kWindow);
            const float right =
                kTimeX + kTimeWidth * static_cast<float>(std::min(end, left_ + kWindow) - left_) /
                             static_cast<float>(kWindow);
            const Rect cell{x + 1, y, std::max(1.0f, right - x - 3), kRowHeight - 5};
            const bool active = focus && p.start <= time_ && time_ < end;
            list.bordered_rect(cell, 8, kWhite.with_alpha(active ? 0.2f : 0.08f), active ? 2 : 0,
                               active ? tone::cream : kClear);
            if (cell.w > 32)
            {
                const auto &title = face_for(fonts, fonts.regular, p.title);
                ui::text(list, title, title.font->fit(p.title, 21, cell.w - 22), x + 12, y + 39, 21,
                         theme.text);
            }
        }
        if (!any)
            ui::text(list, fonts.regular, "No programme information", kTimeX + 14, y + 40, 21,
                     theme.text_muted);
    }
    const auto now = static_cast<std::int64_t>(platform::unix_time());
    if (now >= left_ && now < left_ + kWindow)
    {
        const float x =
            kTimeX + kTimeWidth * static_cast<float>(now - left_) / static_cast<float>(kWindow);
        list.rounded_rect({x, kRowY - 7, 2, kRows * kRowHeight}, 0, tone::accent);
    }
    if (const auto *p = selected())
    {
        const auto &face = face_for(fonts, fonts.semibold, p->title);
        ui::text(list, face, face.font->fit(p->title, 29, 1450), kMargin, 855, 29, theme.text);
        const auto channel = model.channel(model.visible(row_));
        const auto available = !catchup_url(channel, *p, now).empty();
        const std::string status = programme_time(p->start) + " - " + programme_time(p->end) +
                                   (available                         ? "   Catch-up available"
                                    : p->start <= now && now < p->end ? "   On now"
                                    : p->start > now                  ? "   Upcoming"
                                                                      : "   Archive unavailable");
        ui::text(list, fonts.regular, status, kMargin, 890, 22,
                 available ? tone::accent : theme.text_muted);
        const auto &body = face_for(fonts, fonts.regular, p->description);
        ui::text(list, body, body.font->fit(p->description, 21, kWidth - 2 * kMargin), kMargin, 926,
                 21, theme.text_muted);
    }
    else if (model.visible_count() == 0)
        ui::text(list, fonts.regular, "No channels match the current category, folder or search.",
                 kMargin, 400, 26, theme.text_muted);
    ui::text(list, fonts.regular,
             "Cross: Watch   D-pad: Channel / programme   L1/R1: Day   L2/R2: Page   Square: Now   "
             "Triangle: Search   Circle: Back",
             kMargin, 990, 21, theme.text_muted);
    ui::text(list, fonts.regular, "Options: Update guide", kMargin, 1030, 21, theme.text_muted);
}
} // namespace ptv
