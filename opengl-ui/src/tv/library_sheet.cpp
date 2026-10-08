// ProsperoTV - Provider categories and favorite folders.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/library_sheet.hpp"
#include "tv/draw.hpp"
#include "tv/category_path.hpp"
#include "iptv_ime.h"

#include <algorithm>
#include <map>

namespace ptv
{
LibrarySheet::LibrarySheet(Shared &shared) : shared_(shared)
{
    list_.style.theme = shared.theme;
    list_.style.row_height = 72;
    list_.style.title_size = 24;
    list_.style.entrance_step = 0;
    list_.set_bounds({400, 245, 1120, 640});
}

void LibrarySheet::sync()
{
    auto &model = shared_.model;
    const int focus = list_.focus();
    values_.clear();
    branches_.clear();
    std::vector<ui::ListItem> rows;
    ui::ListItem all;
    all.title = folders_ ? "All favorites" : "All provider categories";
    if (!folders_ && !category_path_.empty())
    {
        all.title = "Browse all in " + category_path_;
        all.value = model.category_hidden(category_path_) ? "Hidden" : "";
    }
    rows.push_back(std::move(all));
    if (folders_)
    {
        values_ = model.folders();
        for (const auto &folder : values_)
        {
            ui::ListItem row;
            row.title = folder;
            row.value =
                !channel_.empty() && model.in_folder(folder, channel_) ? "Channel included" : "";
            row.badge = model.folder() == folder ? "In use" : "";
            rows.push_back(std::move(row));
        }
    }
    else
    {
        std::map<std::string, std::pair<unsigned, bool>> children;
        for (const auto &category : model.provider_categories())
        {
            std::string_view child = category.value;
            bool branch = false;
            while (category_parent(child) != category_path_ && !child.empty())
            {
                child = category_parent(child);
                branch = true;
            }
            if (child.empty())
                continue;
            auto &row = children[std::string(child)];
            row.first += category.count;
            row.second = row.second || branch;
        }
        for (const auto &[child, info] : children)
        {
            values_.push_back(child);
            branches_.push_back(info.second);
            ui::ListItem row;
            row.title = category_leaf(child);
            row.value = model.category_hidden(child) ? "Hidden"
                        : info.second                ? ""
                                                     : group_digits(info.first);
            row.badge = info.second                          ? "Subcategories"
                        : model.provider_category() == child ? "In use"
                                                             : "";
            rows.push_back(std::move(row));
        }
    }
    list_.set_items(std::move(rows));
    list_.set_focus(std::clamp(focus, 0, static_cast<int>(values_.size())), true);
    seen_revision_ = model.revision();
    dirty_ = false;
}

void LibrarySheet::open(bool folders, std::string channel, ui::Feedback &feedback)
{
    folders_ = folders;
    category_path_.clear();
    channel_ = std::move(channel);
    open_ = true;
    sync();
    list_.set_focus(0, true);
    list_.enter();
    feedback.play(audio::Cue::open);
}

void LibrarySheet::dismiss()
{
    if (open_)
        iptv_ime_cancel();
    open_ = false;
}

void LibrarySheet::named(const char *text, void *context)
{
    auto &self = *static_cast<LibrarySheet *>(context);
    if (text == nullptr || *text == '\0')
        return;
    if (!self.shared_.model.create_folder(text))
        self.shared_.model.announce(Level::error, "Folder could not be created",
                                    "Use a new name. Check that storage is available.", 4);
    self.dirty_ = true;
}

void LibrarySheet::handle(const InputFrame &input, ui::Feedback &feedback)
{
    if (input.is_pressed(Action::back) || input.is_pressed(Action::touch))
    {
        if (!folders_ && !category_path_.empty() && input.is_pressed(Action::back))
        {
            category_path_ = category_parent(category_path_);
            sync();
            list_.set_focus(0, true);
            feedback.play(audio::Cue::back);
            return;
        }
        dismiss();
        feedback.play(audio::Cue::back);
        return;
    }
    auto &model = shared_.model;
    if (dirty_ || seen_revision_ != model.revision())
        sync();
    if (folders_ && input.is_pressed(Action::north))
    {
        if (model.keyboard_ready())
            iptv_ime_request_prompt("", "New favorite folder", "Folder name", 32,
                                    &LibrarySheet::named, this);
        return;
    }
    const auto event = list_.handle(input, feedback);
    const int at = list_.focus();
    const std::string value = at > 0 && static_cast<std::size_t>(at) <= values_.size()
                                  ? values_[static_cast<std::size_t>(at - 1)]
                                  : (folders_ ? std::string() : category_path_);
    if (input.is_pressed(Action::west) && !value.empty())
    {
        bool saved = false;
        if (folders_)
        {
            for (unsigned i = 0; i < model.channel_count(); ++i)
                if (model.channel(i).id == channel_)
                {
                    saved = model.put_in_folder(value, i, !model.in_folder(value, channel_));
                    break;
                }
        }
        else
        {
            if (model.category_hidden(category_parent(value)))
            {
                model.announce(Level::warning, "Show the parent category first",
                               std::string(category_parent(value)), 3);
                return;
            }
            saved = model.hide_category(value, !model.category_hidden(value));
        }
        if (!saved)
            model.announce(Level::error, "The change could not be saved", "", 3);
        sync();
    }
    else if (event == ui::Event::activated)
    {
        if (!folders_ && at > 0 && branches_[static_cast<std::size_t>(at - 1)])
        {
            category_path_ = value;
            sync();
            list_.set_focus(0, true);
            return;
        }
        if (folders_)
            model.set_folder(value);
        else if (!model.category_hidden(value))
            model.set_provider_category(value);
        else
        {
            feedback.play(audio::Cue::error);
            return;
        }
        dismiss();
    }
}

void LibrarySheet::update(float dt)
{
    if (open_ && (dirty_ || seen_revision_ != shared_.model.revision()))
        sync();
    list_.style.reduced_motion = shared_.settings.reduced_motion;
    list_.update(dt);
}

void LibrarySheet::draw(ui::Canvas &canvas) const
{
    if (!open_)
        return;
    auto &draw = canvas.list;
    const auto &fonts = canvas.fonts;
    draw.rounded_rect({0, 0, kWidth, kHeight}, 0, tone::night.with_alpha(0.9f));
    draw_glass(canvas, shared_.theme, {360, 120, 1200, 830}, 28);
    ui::text(draw, fonts.display, folders_ ? "Favorite folders" : "Provider categories", 400, 190,
             42, shared_.theme.text);
    list_.draw(canvas);
    ui::text(draw, fonts.regular,
             folders_
                 ? "Cross: Open   Square: Add/remove channel   Triangle: New folder   Circle: Back"
                 : "Cross: Browse   Square: Show/hide category   Circle: Back",
             400, 915, 22, shared_.theme.text_muted);
}
} // namespace ptv
