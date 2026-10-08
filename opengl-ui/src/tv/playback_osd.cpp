// ProsperoTV - Channel controls and text over the foreground video.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/playback_osd.hpp"
#include "tv/draw.hpp"
#include "tv/platform.hpp"
#include "core/save_file.hpp"
#include "iptv_input.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ptv
{
void VideoPanel::reset(unsigned left, unsigned top, unsigned w, unsigned h)
{
    x = left;
    y = top;
    width = w;
    height = h;
    if (x > 1920 || y > 1080 || w > 1920 - x || h > 1080 - y)
        width = height = 0;
    pixels.assign(static_cast<std::size_t>(width) * height, 28);
}

void VideoPanel::text(const hui::gfx::Font &font, std::string_view value, float left,
                      float baseline, float size, float max_width, std::uint8_t ink)
{
    if (font.atlas().empty() || size <= 0 || max_width <= 0)
        return;
    std::vector<hui::gfx::GlyphQuad> quads;
    font.layout(font.fit(value, size, max_width), left, baseline, size, hui::gfx::Align::left,
                quads);
    const int aw = font.atlas_width(), ah = font.atlas_height();
    const auto sample = [&](int sx, int sy)
    {
        return static_cast<float>(
                   font.atlas()[std::clamp(sy, 0, ah - 1) * aw + std::clamp(sx, 0, aw - 1)]) /
               255.0f;
    };
    for (const auto &q : quads)
    {
        if (q.x1 <= q.x0 || q.y1 <= q.y0)
            continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(q.x0)));
        const int y0 = std::max(0, static_cast<int>(std::floor(q.y0)));
        const int x1 = std::min(static_cast<int>(width), static_cast<int>(std::ceil(q.x1)));
        const int y1 = std::min(static_cast<int>(height), static_cast<int>(std::ceil(q.y1)));
        for (int py = y0; py < y1; ++py)
            for (int px = x0; px < x1; ++px)
            {
                const float u =
                    (q.u0 + (q.u1 - q.u0) * (px + 0.5f - q.x0) / (q.x1 - q.x0)) * aw - 0.5f;
                const float v =
                    (q.v0 + (q.v1 - q.v0) * (py + 0.5f - q.y0) / (q.y1 - q.y0)) * ah - 0.5f;
                const int sx = static_cast<int>(std::floor(u)),
                          sy = static_cast<int>(std::floor(v));
                const float fx = u - sx, fy = v - sy;
                const float sdf =
                    std::lerp(std::lerp(sample(sx, sy), sample(sx + 1, sy), fx),
                              std::lerp(sample(sx, sy + 1), sample(sx + 1, sy + 1), fx), fy);
                const float alpha =
                    std::clamp((sdf - 0.5f) * 2.0f * font.sdf_range(size) + 0.5f, 0.0f, 1.0f);
                auto &pixel = pixels[static_cast<std::size_t>(py) * width + px];
                pixel = static_cast<std::uint8_t>(
                    std::lerp(static_cast<float>(pixel), static_cast<float>(ink), alpha));
            }
    }
}

bool VideoPanel::composite(void *surface, std::size_t bytes, unsigned pitch, unsigned sh,
                           unsigned vw, unsigned vh, unsigned depth) const
{
    if (!surface || (depth != 8 && depth != 10) || !vw || !vh || vw > pitch || vh > sh ||
        pitch > 8192 || sh > 8192 || (pitch & 1u) || !width || !height || x > 1920 || y > 1080 ||
        width > 1920 - x || height > 1080 - y ||
        pixels.size() != static_cast<std::size_t>(width) * height)
        return false;
    const unsigned component = depth == 10 ? 2 : 1;
    const std::size_t y_size = static_cast<std::size_t>(pitch) * sh;
    if ((y_size + static_cast<std::size_t>(pitch) * ((sh + 1) / 2)) * component > bytes)
        return false;
    const unsigned left = x * vw / 1920, top = y * vh / 1080;
    const unsigned right = (x + width) * vw / 1920, bottom = (y + height) * vh / 1080;
    if (left == right || top == bottom)
        return false;
    auto *out = static_cast<std::uint8_t *>(surface);
    const auto put = [&](std::size_t at, std::uint8_t value)
    {
        if (component == 1)
            out[at] = value;
        else
        {
            const std::uint16_t word = static_cast<std::uint16_t>(value) << 2;
            std::memcpy(out + at * 2, &word, sizeof(word));
        }
    };
    for (unsigned py = top; py < bottom; ++py)
    {
        const auto row = static_cast<std::size_t>((py - top) * height / (bottom - top)) * width;
        for (unsigned px = left; px < right; ++px)
            put(static_cast<std::size_t>(py) * pitch + px,
                pixels[row + (px - left) * width / (right - left)]);
    }
    // Neutral chroma for the panel, preserving all other video and row padding.
    for (unsigned py = top / 2; py < (bottom + 1) / 2; ++py)
        for (unsigned px = left & ~1u; px < ((right + 1) & ~1u); ++px)
            put(y_size + static_cast<std::size_t>(py) * pitch + px, 128);
    return true;
}

PlaybackOsd::PlaybackOsd(Model &model, const PlayRequest &request)
    : model_(model), current_id_(request.channel_id), title_(request.channel_name),
      live_(request.record_channel_result)
{
    if (live_)
    {
        for (unsigned i = 0; i < model.visible_count(); ++i)
        {
            const auto index = model.visible(i);
            if (model.channel(index).id == current_id_)
            {
                focus_ = i;
                current_position_ = static_cast<int>(i);
            }
            channels_.push_back(index);
        }
    }
    update_guide();
}

bool PlaybackOsd::load_fonts(const std::string &directory)
{
    const auto load = [&](const char *name, hui::gfx::Font &font, hui::ui::FontRef &ref)
    {
        std::string data;
        if (!hui::save::read_file(directory + "/" + name, &data, 64u << 20) || !font.load(data))
            return false;
        ref.font = &font;
        return true;
    };
    if (!load("inter-regular.huifont", regular_, fonts_.regular))
        return false;
    fonts_.semibold = fonts_.regular;
    bool east_asian = model_.uses_east_asian(), korean = model_.uses_korean();
    note_scripts(title_ + now_ + next_, &east_asian, &korean);
    if (east_asian)
        (void)load("noto-sans-east-asian.huifont", east_asian_, fonts_.hand);
    if (korean)
        (void)load("noto-sans-korean.huifont", korean_, fonts_.pixel);
    return true;
}

void PlaybackOsd::update_guide()
{
    const auto time = static_cast<std::int64_t>(platform::unix_time());
    guide_minute_ = time / 60;
    now_ = live_ ? "Programme guide unavailable" : "";
    next_.clear();
    if (live_)
        if (const auto channel = model_.find(current_id_))
        {
            title_ = display_name(*channel);
            if (const auto programme = model_.guide().now(channel->id, time))
                now_ = "Now  " + programme_time(programme->start) + "  " + programme->title;
            if (const auto programme = model_.guide().next(channel->id, time))
                next_ = "Next  " + programme_time(programme->start) + "  " + programme->title;
        }
    dirty_ = true;
}

int PlaybackOsd::select(unsigned index)
{
    list_ = false;
    dirty_ = true;
    if (model_.channel(index).id == current_id_)
        return 1;
    selected_ = index;
    return 2;
}

void PlaybackOsd::set_audio_state(const iptv_player_audio_state_t &state)
{
    std::lock_guard lock(mutex_);
    bool changed = audio_.count != state.count || audio_.selected_pid != state.selected_pid ||
                   audio_.disabled != state.disabled || audio_.pending != state.pending ||
                   audio_.result != state.result;
    const auto count = std::min(state.count, IPTV_STREAM_MAX_AUDIO_TRACKS);
    for (unsigned i = 0; i < count && !changed; ++i)
        changed = audio_.tracks[i].pid != state.tracks[i].pid ||
                  audio_.tracks[i].stream_type != state.tracks[i].stream_type ||
                  audio_.tracks[i].audio_type != state.tracks[i].audio_type ||
                  std::strncmp(audio_.tracks[i].language, state.tracks[i].language, 3) != 0;
    if (changed)
    {
        audio_ = state;
        audio_.count = count;
        audio_focus_ = std::min(audio_focus_, count);
        dirty_ = true;
    }
}

std::optional<std::uint32_t> PlaybackOsd::take_audio_selection()
{
    std::lock_guard lock(mutex_);
    const auto result = audio_selection_;
    audio_selection_.reset();
    return result;
}

int PlaybackOsd::input(int action, std::uint64_t now)
{
    std::lock_guard lock(mutex_);
    if (action == -1)
    {
        if (platform::unix_time() / 60 != guide_minute_)
            update_guide();
        return 0;
    }
    if (action == IPTV_INPUT_OPTIONS)
    {
        audio_menu_ = !audio_menu_;
        list_ = false;
        audio_focus_ = 0;
        for (unsigned i = 0; i < audio_.count; ++i)
            if (audio_.tracks[i].pid == audio_.selected_pid)
                audio_focus_ = i + 1;
        dirty_ = true;
        return 1;
    }
    if (audio_menu_)
    {
        if (action == IPTV_INPUT_CIRCLE)
            audio_menu_ = false;
        else if (action == IPTV_INPUT_CROSS && audio_.count && !audio_.pending)
            audio_selection_ = audio_focus_ ? audio_.tracks[audio_focus_ - 1].pid : 0;
        else if (action == IPTV_INPUT_UP || action == IPTV_INPUT_DOWN ||
                 action == IPTV_INPUT_LEFT || action == IPTV_INPUT_RIGHT)
        {
            const int delta = action == IPTV_INPUT_UP     ? -1
                              : action == IPTV_INPUT_DOWN ? 1
                              : action == IPTV_INPUT_LEFT ? -9
                                                          : 9;
            audio_focus_ = static_cast<unsigned>(std::clamp(static_cast<int>(audio_focus_) + delta,
                                                            0, static_cast<int>(audio_.count)));
        }
        dirty_ = true;
        return 1;
    }
    if (action == IPTV_INPUT_TRIANGLE || action == IPTV_INPUT_TOUCHPAD)
    {
        banner_until_ = now + 5000000;
        return 1;
    }
    if (action == IPTV_INPUT_CIRCLE && list_)
    {
        list_ = false;
        dirty_ = true;
        return 1;
    }
    if (!live_)
        return 0;
    if (action == IPTV_INPUT_SQUARE)
    {
        if (const auto previous = model_.previous_channel(current_id_))
            return select(*previous);
        banner_until_ = now + 5000000;
        return 1;
    }
    if (channels_.empty())
        return 0;
    if (action == IPTV_INPUT_L1 || action == IPTV_INPUT_R1 ||
        (!list_ && (action == IPTV_INPUT_UP || action == IPTV_INPUT_DOWN)))
    {
        const bool backwards = action == IPTV_INPUT_L1 || action == IPTV_INPUT_DOWN;
        const auto current = current_position_;
        const unsigned at = current < 0
                                ? (backwards ? 0u : static_cast<unsigned>(channels_.size() - 1))
                                : static_cast<unsigned>(current);
        return select(channels_[(at + channels_.size() + (backwards ? -1 : 1)) % channels_.size()]);
    }
    if (action == IPTV_INPUT_CROSS)
    {
        if (list_)
            return select(channels_[focus_]);
        list_ = true;
        dirty_ = true;
        return 1;
    }
    if (action == IPTV_INPUT_UP || action == IPTV_INPUT_DOWN ||
        (list_ && (action == IPTV_INPUT_LEFT || action == IPTV_INPUT_RIGHT)))
    {
        if (list_)
        {
            const int delta = action == IPTV_INPUT_UP     ? -1
                              : action == IPTV_INPUT_DOWN ? 1
                              : action == IPTV_INPUT_LEFT ? -9
                                                          : 9;
            focus_ = static_cast<unsigned>(std::clamp(static_cast<int>(focus_) + delta, 0,
                                                      static_cast<int>(channels_.size()) - 1));
        }
        list_ = true;
        dirty_ = true;
        return 1;
    }
    return 0;
}

void PlaybackOsd::line(std::string_view value, float x, float y, float size, float width)
{
    const auto &face = face_for(fonts_, fonts_.regular, value);
    if (face.font)
        panel_.text(*face.font, readable(face, value), x, y, size, width);
}

void PlaybackOsd::paint()
{
    if (audio_menu_)
    {
        panel_.reset(80, 100, 820, 860);
        line("Audio", 28, 54, 32, 760);
        line(!audio_.count                            ? "No supported audio tracks advertised"
             : audio_.pending                         ? "Changing audio..."
             : audio_.result < 0                      ? "Could not switch. Choose another track."
             : audio_.selected_pid && audio_.disabled ? "Audio unavailable. Choose another track."
                                                      : "Choose a language or turn audio off",
             28, 88, 22, 760);
        const unsigned begin = (audio_focus_ / 9) * 9;
        for (unsigned row = 0; row < 9 && begin + row <= audio_.count; ++row)
        {
            const unsigned index = begin + row, top = 110 + row * 72;
            if (index == audio_focus_)
                for (unsigned y = top; y < top + 66; ++y)
                    std::fill_n(panel_.pixels.begin() + y * panel_.width + 16, 788, 66);
            std::string label = "Off";
            std::uint32_t pid = 0;
            if (index)
            {
                const auto &track = audio_.tracks[index - 1];
                pid = track.pid;
                label = "Track " + std::to_string(index);
                if (track.language[0])
                    label += " (" + std::string(track.language, 3) + ")";
                const char *codec = track.stream_type == 0x81   ? "AC-3"
                                    : track.stream_type == 0x87 ? "E-AC-3"
                                    : track.stream_type == 3 || track.stream_type == 4
                                        ? "MPEG audio"
                                        : "AAC";
                label += std::string("  ") + codec;
                if (track.audio_type == 2)
                    label += "  Hearing impaired";
                if (track.audio_type == 3)
                    label += "  Audio description";
            }
            line((pid == audio_.selected_pid ? "> " : "") + label, 28, top + 43, 28, 760);
        }
        line("Up/Down Browse · Left/Right Page", 28, 800, 22, 760);
        line("Cross Select · Circle Close", 28, 833, 22, 760);
    }
    else if (list_)
    {
        panel_.reset(80, 100, 760, 860);
        line("Channels", 28, 54, 32, 700);
        line(std::to_string(focus_ + 1) + " / " + std::to_string(channels_.size()), 28, 88, 22,
             700);
        const unsigned begin = (focus_ / 9) * 9;
        for (unsigned row = 0; row < 9 && begin + row < channels_.size(); ++row)
        {
            const auto index = channels_[begin + row];
            const auto channel = model_.channel(index);
            const unsigned top = 110 + row * 72;
            if (begin + row == focus_)
                for (unsigned y = top; y < top + 66; ++y)
                    std::fill_n(panel_.pixels.begin() + y * panel_.width + 16, 728, 66);
            line((channel.id == current_id_ ? "> " : "") + std::to_string(model_.number_of(index)) +
                     "  " + display_name(channel),
                 28, top + 43, 28, 700);
        }
        line("Up/Down Browse · Left/Right Page", 28, 800, 22, 700);
        line("Cross Watch · Circle Close", 28, 833, 22, 700);
    }
    else
    {
        panel_.reset(80, 770, 1760, 230);
        line(title_, 30, 52, 36, 1700);
        line(now_, 30, 101, 28, 1700);
        line(next_, 30, 143, 28, 1700);
        line(live_ ? "Up/Down Channel · Square Last · Cross List · Options Audio · Triangle Info · "
                     "Circle Back"
                   : "Options Audio · Triangle Info · Circle Back",
             30, 204, 24, 1700);
    }
    dirty_ = false;
}

bool PlaybackOsd::draw(void *surface, std::size_t bytes, unsigned pitch, unsigned sh, unsigned vw,
                       unsigned vh, unsigned depth, std::uint64_t now)
{
    std::lock_guard lock(mutex_);
    if (!fonts_.regular.font)
        return false;
    if (!started_)
    {
        started_ = true;
        banner_until_ = now + 5000000;
    }
    if (!list_ && !audio_menu_ && now >= banner_until_)
        return false;
    if (!surface)
        return true;
    if (dirty_)
        paint();
    return panel_.composite(surface, bytes, pitch, sh, vw, vh, depth);
}
} // namespace ptv
