// ProsperoTV - Reuse independent preview workers for a bounded live mosaic.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/multiview.hpp"
#include "tv/draw.hpp"
#include "tv/channel_text.hpp"
#include "ui/glyphs.hpp"
#include <algorithm>

namespace ptv
{
void Multiview::configure(LivePreview::Upload upload, ImageCache::Release release)
{
    stop();
    for (auto &preview : previews_)
        preview.configure(upload, release, true);
}
void Multiview::open(std::string channel)
{
    stop();
    active_ = true;
    count_ = 2;
    selected_ = 0;
    muted_ = false;
    if (!channel.empty())
        choose(std::move(channel));
}
void Multiview::stop()
{
    active_ = choosing_ = false;
    // Cancel every connection before joining any worker.
    for (auto &preview : previews_)
        preview.request_stop();
    for (auto &preview : previews_)
        preview.clear();
    channels_ = {};
    names_ = {};
}
bool Multiview::choose(std::string channel)
{
    const auto request = shared_.model.preview_request(channel);
    if (!active_ || !request)
        return false;
    if (channels_[selected_] == channel && previews_[selected_].finished())
        previews_[selected_].clear();
    channels_[selected_] = std::move(channel);
    names_[selected_] = request->channel_name;
    choosing_ = false;
    return true;
}
unsigned Multiview::audible_count() const
{
    unsigned result = 0;
    for (const auto &preview : previews_)
        result += preview.audio_active();
    return result;
}
void Multiview::handle(const InputFrame &input, ui::Feedback &feedback)
{
    if (!is_open())
        return;
    if (input.is_pressed(Action::back))
    {
        stop();
        feedback.play(audio::Cue::back);
        return;
    }
    if (input.is_pressed(Action::north))
    {
        count_ = count_ == 2 ? 4 : 2;
        selected_ = std::min(selected_, count_ - 1);
        for (unsigned i = count_; i < 4; ++i)
            previews_[i].request_stop();
        for (unsigned i = count_; i < 4; ++i)
        {
            previews_[i].clear();
            channels_[i].clear();
            names_[i].clear();
        }
    }
    if (input.nav == Direction::left && selected_ % 2)
        --selected_;
    else if (input.nav == Direction::right && selected_ % 2 == 0)
        ++selected_;
    else if (input.nav == Direction::up && selected_ >= 2)
        selected_ -= 2;
    else if (input.nav == Direction::down && selected_ + 2 < count_)
        selected_ += 2;
    if (input.is_pressed(Action::r3))
        muted_ = !muted_;
    if (input.is_pressed(Action::west))
        choosing_ = true;
    if (input.is_pressed(Action::confirm))
    {
        if (channels_[selected_].empty())
            choosing_ = true;
        else
            shared_.model.play_channel(channels_[selected_]);
    }
}
void Multiview::update(float dt)
{
    if (!active_)
        return;
    std::array<std::optional<PlayRequest>, 4> requests;
    for (unsigned i = 0; i < count_; ++i)
    {
        requests[i] = shared_.model.preview_request(channels_[i]);
        if (requests[i])
            names_[i] = requests[i]->channel_name;
    }
    bool previous_audio = false;
    for (unsigned i = 0; i < 4; ++i)
        if (i != selected_ || muted_ || !requests[i])
        {
            previews_[i].set_audio(false);
            previous_audio |= previews_[i].audio_active();
        }
    // Acknowledge the old audio port's close before opening the next one.
    previews_[selected_].set_audio(!muted_ && requests[selected_] && !previous_audio);
    for (unsigned i = 0; i < 4; ++i)
        previews_[i].update(std::move(requests[i]), dt);
}
void Multiview::draw(ui::Canvas &canvas) const
{
    auto &list = canvas.list;
    const auto &fonts = canvas.fonts;
    list.rounded_rect({0, 0, kWidth, kHeight}, 0, Color::rgb(0x101015));
    ui::text(list, fonts.semibold, "Multiview", 48, 64, 34, kWhite);
    ui::text(list, fonts.regular, muted_ ? "Muted" : "Sound follows the selected channel",
             kWidth - 48, 64, 22, shared_.theme.text_muted, gfx::Align::right);
    const float gap = 20, width = (kWidth - 96 - gap) / 2;
    const float height = count_ == 2 ? 590 : 408;
    const float top = count_ == 2 ? 210 : 104;
    for (unsigned i = 0; i < count_; ++i)
    {
        const Rect tile{48 + (width + gap) * (i % 2), top + (height + gap) * (i / 2), width,
                        height};
        list.bordered_rect(tile, 12, Color::rgb(0x050508), i == selected_ ? 4 : 1,
                           i == selected_ ? tone::accent : Color::rgb(0x343440));
        const Rect picture{tile.x + 8, tile.y + 8, tile.w - 16, tile.h - 65};
        const auto texture = previews_[i].find(channels_[i]);
        if (texture.id)
        {
            const float scale = std::min(picture.w / texture.width, picture.h / texture.height);
            const float w = texture.width * scale, h = texture.height * scale;
            list.image(texture.id, {picture.cx() - w / 2, picture.cy() - h / 2, w, h}, {0, 0, 1, 1},
                       kWhite);
        }
        else
        {
            const char *message = channels_[i].empty()      ? "Choose a channel"
                                  : previews_[i].finished() ? "Could not open this channel"
                                                            : "Opening channel...";
            ui::text(list, fonts.regular, message, picture.cx(), picture.cy(), 26,
                     shared_.theme.text_muted, gfx::Align::center);
        }
        const auto &face = face_for(fonts, fonts.semibold, names_[i]);
        const auto name = face.font->fit(readable(face, names_[i]), 23, tile.w - 160);
        ui::text(list, face, name, tile.x + 18, tile.y + tile.h - 22, 23, kWhite);
        const char *status = previews_[i].audio_failed()             ? "No audio"
                             : previews_[i].audio_active()           ? "Audio"
                             : previews_[i].finished() && texture.id ? "Ended"
                                                                     : "";
        ui::text(list, fonts.regular, status, tile.x + tile.w - 18, tile.y + tile.h - 22, 20,
                 tone::accent, gfx::Align::right);
    }
    const ui::Hint hints[] = {
        {ui::Button::cross, "Full screen"},
        {ui::Button::square, "Choose channel"},
        {ui::Button::triangle, count_ == 2 ? "Four channels" : "Two channels"},
        {ui::Button::right_stick, muted_ ? "Unmute" : "Mute"},
        {ui::Button::circle, "Close"}};
    ui::HintLayout layout;
    layout.cy = 1028;
    layout.text_size = 21;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, 5, kWidth - 48, true, layout);
}
} // namespace ptv
