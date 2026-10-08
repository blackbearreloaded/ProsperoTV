// ProsperoTV - A virtual list: only the visible movies or episodes are drawn.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/vod_screen.hpp"
#include "tv/category_path.hpp"
#include "tv/draw.hpp"
#include <algorithm>
#include <map>

namespace ptv
{
namespace
{
constexpr int kRows = 7;
}
void VodScreen::enter()
{
    const auto &view = shared_.model.view;
    if (view.vod_kind >= 0)
        shared_.model.vod().select(static_cast<VodKind>(view.vod_kind), view.vod_series);
    sync();
}
const VodScreen::Row *VodScreen::focused() const
{
    const int focus = shared_.model.view.vod_focus;
    return focus >= 0 && static_cast<std::size_t>(focus) < rows_.size() ? &rows_[focus] : nullptr;
}
void VodScreen::sync()
{
    auto &model = shared_.model;
    auto &view = model.view;
    const auto &vod = model.vod();
    rows_.clear();
    if (view.vod_kind < 0)
        rows_ = {{Kind::movies, "Movies", {}, 0}, {Kind::shows, "TV shows", {}, 0}};
    else
    {
        const auto &catalog = vod.catalog();
        std::map<std::string, std::string> categories;
        std::vector<Row> items;
        for (unsigned i = 0; i < catalog.size(); ++i)
        {
            const auto item = catalog[i];
            if (!view.vod_category.empty() &&
                !category_belongs(item.group_title, view.vod_category))
                continue;
            const bool searching = !view.vod_query.empty();
            if (!searching && !view.vod_all)
            {
                auto child = category_trim(item.group_title);
                while (!child.empty() && category_parent(child) != view.vod_category)
                    child = category_parent(child);
                if (!child.empty() && child != view.vod_category)
                {
                    categories.emplace(std::string(child), std::string(category_leaf(child)));
                    continue;
                }
            }
            if (!model.category_hidden(item.group_title) &&
                contains_nocase(item.name, view.vod_query))
                items.push_back({Kind::item, std::string(item.name), {}, i});
        }
        if (!view.vod_all && view.vod_query.empty() && !catalog.empty())
            rows_.push_back({Kind::all, "Browse all", {}, 0});
        std::vector<std::pair<std::string, std::string>> ordered(categories.begin(),
                                                                 categories.end());
        if (view.vod_kind == static_cast<int>(VodKind::episodes))
            std::sort(ordered.begin(), ordered.end(),
                      [](const auto &left, const auto &right)
                      {
                          return left.first.size() != right.first.size()
                                     ? left.first.size() < right.first.size()
                                     : left.first < right.first;
                      });
        for (const auto &[path, name] : ordered)
            rows_.push_back({Kind::category, name, path, 0});
        std::stable_sort(items.begin(), items.end(),
                         [&](const Row &left, const Row &right)
                         {
                             if (view.vod_kind == static_cast<int>(VodKind::episodes))
                                 return catalog[left.item].source_line <
                                        catalog[right.item].source_line;
                             return left.text < right.text;
                         });
        rows_.insert(rows_.end(), std::make_move_iterator(items.begin()),
                     std::make_move_iterator(items.end()));
    }
    view.vod_focus = std::clamp(view.vod_focus, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
    top_ = std::max(0, view.vod_focus - kRows + 1);
    revision_ = vod.revision();
    model_revision_ = model.revision();
}
void VodScreen::update()
{
    if (revision_ != shared_.model.vod().revision() || model_revision_ != shared_.model.revision())
        sync();
}
void VodScreen::refresh()
{
    const auto &view = shared_.model.view;
    if (view.vod_kind >= 0)
        shared_.model.vod().select(static_cast<VodKind>(view.vod_kind), view.vod_series, true);
}
bool VodScreen::back()
{
    auto &view = shared_.model.view;
    if (!view.vod_query.empty())
        view.vod_query.clear();
    else if (view.vod_all)
        view.vod_all = false;
    else if (!view.vod_category.empty())
        view.vod_category = category_parent(view.vod_category);
    else if (view.vod_kind == static_cast<int>(VodKind::episodes))
    {
        view.vod_kind = static_cast<int>(VodKind::series);
        view.vod_series.clear();
        view.vod_series_name.clear();
        view.vod_series_cover.clear();
        shared_.model.vod().select(VodKind::series);
    }
    else if (view.vod_kind >= 0)
    {
        view.vod_kind = -1;
        shared_.model.vod().stop();
    }
    else
        return false;
    view.vod_focus = 0;
    sync();
    return true;
}
void VodScreen::handle(const InputFrame &input, ui::Feedback &feedback)
{
    auto &model = shared_.model;
    auto &view = model.view;
    if (input.is_pressed(Action::west) && focused() && focused()->kind == Kind::category &&
        view.vod_kind != static_cast<int>(VodKind::episodes))
    {
        const auto category = focused()->category;
        if (!model.hide_category(category, !model.category_hidden(category)))
            feedback.play(audio::Cue::error);
        sync();
        return;
    }
    if (input.is_pressed(Action::north) && view.vod_kind >= 0)
    {
        model.ask_vod_query();
        return;
    }
    const auto old = view.vod_focus;
    if (input.nav == Direction::up)
        --view.vod_focus;
    if (input.nav == Direction::down)
        ++view.vod_focus;
    if (input.is_pressed(Action::jump_prev))
        view.vod_focus -= kRows;
    if (input.is_pressed(Action::jump_next))
        view.vod_focus += kRows;
    view.vod_focus = std::clamp(view.vod_focus, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
    if (old != view.vod_focus)
        feedback.play(audio::Cue::focus);
    if (view.vod_focus < top_)
        top_ = view.vod_focus;
    if (view.vod_focus >= top_ + kRows)
        top_ = view.vod_focus - kRows + 1;
    if (!input.is_pressed(Action::confirm) || !focused() || !model.vod().available())
        return;
    const auto row = *focused();
    if (row.kind == Kind::movies || row.kind == Kind::shows)
    {
        view.vod_kind =
            static_cast<int>(row.kind == Kind::movies ? VodKind::movies : VodKind::series);
        model.vod().select(static_cast<VodKind>(view.vod_kind));
    }
    else if (row.kind == Kind::category)
        view.vod_category = row.category;
    else if (row.kind == Kind::all)
        view.vod_all = true;
    else if (view.vod_kind == static_cast<int>(VodKind::series))
    {
        const auto show = model.vod().catalog()[row.item];
        view.vod_series = show.tvg_id;
        view.vod_series_name = show.name;
        view.vod_series_cover = show.tvg_logo;
        view.vod_kind = static_cast<int>(VodKind::episodes);
        view.vod_category.clear();
        view.vod_query.clear();
        view.vod_all = false;
        model.vod().select(VodKind::episodes, view.vod_series);
    }
    else
    {
        if (model.play_vod(row.item))
            feedback.play(audio::Cue::select);
        return;
    }
    feedback.play(audio::Cue::select);
    view.vod_focus = 0;
    sync();
}
std::vector<std::string> VodScreen::image_urls() const
{
    const auto *row = focused();
    if (row && row->kind == Kind::item)
    {
        const auto image = shared_.model.vod().catalog()[row->item].tvg_logo;
        return {image.empty() ? shared_.model.view.vod_series_cover : std::string(image)};
    }
    return {};
}
void VodScreen::draw(ui::Canvas &canvas) const
{
    const auto &model = shared_.model;
    const auto &view = model.view;
    const auto &theme = shared_.theme;
    const auto &fonts = canvas.fonts;
    auto &draw = canvas.list;
    const std::string heading = view.vod_kind < 0    ? "On demand"
                                : view.vod_kind == 0 ? "Movies"
                                : view.vod_kind == 1 ? "TV shows"
                                                     : view.vod_series_name;
    ui::text(draw, fonts.semibold, "YOUR PROVIDER'S LIBRARY", kMargin, 162, 18, tone::accent,
             gfx::Align::left, 4);
    const auto &face = title_face(fonts, heading);
    ui::text(draw, face, face.font->fit(heading, 54, kWidth - 2 * kMargin), kMargin, 224, 54,
             theme.text);
    const std::string location = view.vod_category.empty() ? "All categories" : view.vod_category;
    ui::text(
        draw, fonts.regular,
        fonts.regular.font->fit(
            location + (view.vod_query.empty() ? "" : " / Search: " + view.vod_query), 23, 1100),
        kMargin, 274, 23, theme.text_muted);
    for (int i = top_; i < static_cast<int>(rows_.size()) && i < top_ + kRows; ++i)
    {
        const auto &row = rows_[i];
        const float y = 305 + static_cast<float>(i - top_) * 85;
        const bool selected = i == view.vod_focus;
        draw.bordered_rect({kMargin, y, 1060, 76}, 14, kWhite.with_alpha(selected ? 0.16f : 0.04f),
                           selected ? 2 : 0, tone::cream);
        const auto label =
            row.text +
            (row.kind == Kind::category && model.category_hidden(row.category) ? " (Hidden)" : "");
        const auto &font = face_for(fonts, fonts.semibold, label);
        ui::text(draw, font, font.font->fit(label, 27, 960), kMargin + 24, y + 47, 27, theme.text);
        if (row.kind != Kind::item || view.vod_kind == 1)
            ui::text(draw, fonts.regular, ">", kMargin + 1020, y + 47, 27, tone::accent);
    }
    if (rows_.empty())
        ui::text(draw, fonts.regular,
                 model.vod().busy() || model.vod().requested() ? "Loading…" : "No matching titles",
                 kMargin, 370, 30, theme.text_muted);
    const auto *row = focused();
    if (row && row->kind == Kind::item)
    {
        const auto item = model.vod().catalog()[row->item];
        const Rect cover{1260, 310, 350, 480};
        draw.rounded_rect(cover, 20, kWhite.with_alpha(0.05f));
        const auto image = shared_.images.find(
            item.tvg_logo.empty() ? std::string_view(view.vod_series_cover) : item.tvg_logo);
        if (image.id)
        {
            const float scale = std::min(cover.w / image.width, cover.h / image.height);
            const float width = image.width * scale, height = image.height * scale;
            draw.image(image.id, {cover.cx() - width / 2, cover.cy() - height / 2, width, height},
                       {0, 0, 1, 1}, kWhite);
        }
        else
        {
            iptv::Channel named;
            named.name = view.vod_kind == static_cast<int>(VodKind::episodes)
                             ? view.vod_series_name
                             : std::string(item.name);
            ui::text(draw, fonts.display, monogram(named), cover.cx(), cover.cy() + 25, 76,
                     tone::accent, gfx::Align::center);
        }
        ui::paragraph(draw, fonts.regular, item.group_title, 1180, 840, 24, 600, 34,
                      theme.text_muted, 3);
    }
    else
        ui::paragraph(draw, fonts.regular,
                      "Browse movies or choose a show, then its season and episode. Circle returns "
                      "to the previous list.",
                      1200, 365, 28, 570, 42, theme.text_muted, 7);
    ui::text(draw, fonts.regular,
             fonts.regular.font->fit(model.vod().status(), 22, kWidth - 2 * kMargin), kMargin, 949,
             22, theme.text_muted);
}
int VodScreen::hints(ui::Hint *out, int capacity) const
{
    const ui::Hint hints[] = {{ui::Button::cross, "Open / Watch"},
                              {ui::Button::triangle, "Search"},
                              {ui::Button::options, "Update"},
                              {ui::Button::circle, "Back"}};
    int count = std::min(capacity, 4);
    std::copy_n(hints, count, out);
    if (count < capacity && focused() && focused()->kind == Kind::category &&
        shared_.model.view.vod_kind != static_cast<int>(VodKind::episodes))
        out[count++] = {ui::Button::square, "Show / Hide"};
    return count;
}
} // namespace ptv
