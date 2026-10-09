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
    // Subtitle language may differ from every channel name in the catalogue.
    (void)load("noto-sans-east-asian.huifont", east_asian_, fonts_.hand);
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
                  std::strncmp(audio_.titles[i], state.titles[i], 128) != 0 ||
                  std::strncmp(audio_.languages[i], state.languages[i], 32) != 0 ||
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

void PlaybackOsd::set_subtitle_state(const iptv::SubtitleState &state)
{
    std::lock_guard lock(mutex_);
    if (subtitles_.tracks != state.tracks || subtitles_.selected != state.selected ||
        subtitles_.error != state.error)
    {
        subtitles_ = state;
        if (subtitles_.tracks.size() > iptv::Subtitles::max_tracks)
            subtitles_.tracks.resize(iptv::Subtitles::max_tracks);
        subtitle_focus_ =
            std::min(subtitle_focus_, static_cast<unsigned>(subtitles_.tracks.size()));
        dirty_ = true;
    }
}
std::optional<std::uint32_t> PlaybackOsd::take_subtitle_selection()
{
    std::lock_guard lock(mutex_);
    const auto result = subtitle_selection_;
    subtitle_selection_.reset();
    return result;
}

void PlaybackOsd::set_live_state(const iptv_player_live_state_t &state)
{
    std::lock_guard lock(mutex_);
    const auto behind = [](const iptv_player_live_state_t &value)
    { return value.last_us > value.position_us ? (value.last_us - value.position_us) / 1000000 : 0; };
    if (state.available != history_.available || state.paused != history_.paused ||
        state.expired != history_.expired || behind(state) != behind(history_))
        dirty_ = true;
    history_ = state;
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
        subtitle_tab_ = false;
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
        if (action == IPTV_INPUT_L1 || action == IPTV_INPUT_R1)
        {
            subtitle_tab_ = !subtitle_tab_;
            subtitle_focus_ = 0;
            for (unsigned i = 0; i < subtitles_.tracks.size(); ++i)
                if (subtitles_.tracks[i].id == subtitles_.selected)
                    subtitle_focus_ = i + 1;
            dirty_ = true;
            return 1;
        }
        auto &focus = subtitle_tab_ ? subtitle_focus_ : audio_focus_;
        const unsigned count =
            subtitle_tab_ ? static_cast<unsigned>(subtitles_.tracks.size()) : audio_.count;
        if (action == IPTV_INPUT_CIRCLE)
            audio_menu_ = false;
        else if (action == IPTV_INPUT_CROSS && count)
        {
            if (subtitle_tab_)
                subtitle_selection_ = focus ? subtitles_.tracks[focus - 1].id : 0;
            else if (!audio_.pending)
                audio_selection_ = focus ? audio_.tracks[focus - 1].pid : 0;
        }
        else if (action == IPTV_INPUT_UP || action == IPTV_INPUT_DOWN ||
                 action == IPTV_INPUT_LEFT || action == IPTV_INPUT_RIGHT)
        {
            const int delta = action == IPTV_INPUT_UP     ? -1
                              : action == IPTV_INPUT_DOWN ? 1
                              : action == IPTV_INPUT_LEFT ? -9
                                                          : 9;
            focus = static_cast<unsigned>(
                std::clamp(static_cast<int>(focus) + delta, 0, static_cast<int>(count)));
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
    if (!list_ && (action == IPTV_INPUT_PLAY_PAUSE || action == IPTV_INPUT_GO_LIVE ||
                   action == IPTV_INPUT_LEFT || action == IPTV_INPUT_RIGHT))
    {
        banner_until_ = now + 5000000;
        return 0; // The player owns the live-history request.
    }
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
    if (audio_menu_ && subtitle_tab_)
    {
        panel_.reset(80, 100, 820, 860);
        line("Subtitles", 28, 54, 32, 760);
        line(subtitles_.tracks.empty() ? "No supported subtitles advertised"
             : subtitles_.error != iptv::SubtitleError::none
                 ? "Subtitles unavailable. Choose another track."
                 : "Choose a language or turn subtitles off",
             28, 88, 22, 760);
        const unsigned begin = (subtitle_focus_ / 9) * 9;
        for (unsigned row = 0; row < 9 && begin + row <= subtitles_.tracks.size(); ++row)
        {
            const unsigned index = begin + row, top = 110 + row * 72;
            if (index == subtitle_focus_)
                for (unsigned y = top; y < top + 66; ++y)
                    std::fill_n(panel_.pixels.begin() + y * panel_.width + 16, 788, 66);
            std::string label = "Off";
            unsigned id = 0;
            if (index)
            {
                const auto &track = subtitles_.tracks[index - 1];
                id = track.id;
                label = "Track " + std::to_string(index);
                if (!track.language.empty())
                    label += " (" + track.language + ")";
                if (track.forced)
                    label += "  Forced";
                if (track.hearing_impaired)
                    label += "  Hearing impaired";
                if (!track.title.empty())
                    label += "  " + track.title;
            }
            line((id == subtitles_.selected ? "> " : "") + label, 28, top + 43, 28, 760);
        }
        line("L1/R1 Audio / Subtitles", 28, 764, 22, 760);
        line("Up/Down Browse · Left/Right Page", 28, 800, 22, 760);
        line("Cross Select · Circle Close", 28, 833, 22, 760);
    }
    else if (audio_menu_)
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
                const auto *title = audio_.titles[index - 1];
                const auto *language = audio_.languages[index - 1];
                label = title[0] ? std::string(title, strnlen(title, 128))
                                 : "Track " + std::to_string(index);
                if (language[0])
                    label += " (" + std::string(language, strnlen(language, 32)) + ")";
                else if (track.language[0])
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
        line("L1/R1 Audio / Subtitles", 28, 764, 22, 760);
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
        panel_.reset(80, history_.available ? 690 : 770, 1760, history_.available ? 310 : 230);
        line(title_, 30, 52, 36, 1700);
        line(now_, 30, 101, 28, 1700);
        line(next_, 30, 143, 28, 1700);
        if (history_.available)
        {
            const auto seconds = history_.last_us > history_.position_us
                                     ? (history_.last_us - history_.position_us) / 1000000
                                     : 0;
            std::string position = history_.paused ? "Paused" : seconds <= 3 ? "Live" : "Replay";
            if (seconds > 3)
                position += "  " + std::to_string(seconds / 60) + ":" +
                            (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60) +
                            " behind live";
            if (history_.expired)
                position += "  Earlier video is no longer retained";
            line(position, 30, 187, 24, 1700);
            line("L2 Pause / resume  |  Left / Right 30 seconds  |  R2 Back to live", 30, 231, 24,
                 1700);
        }
        line(live_
                 ? "Up/Down Channel · Square Last · Cross List · Options Tracks · Triangle Info · "
                   "Circle Back"
                 : "Options Tracks · Triangle Info · Circle Back",
             30, history_.available ? 281 : 204, 24, 1700);
    }
    dirty_ = false;
}

void PlaybackOsd::paint_subtitles()
{
    struct Line
    {
        const hui::gfx::Font *font;
        std::string text;
    };
    std::vector<Line> lines;
    float widest = 0;
    constexpr float size = 36, max_width = 1560;
    for (const auto &cue : subtitle_cues_)
    {
        if (!cue)
            continue;
        const auto &face = face_for(fonts_, fonts_.regular, cue->text);
        if (!face.font)
            continue;
        for (const auto &wrapped : face.font->wrap(cue->text, size, max_width))
        {
            std::string_view remaining = wrapped;
            while (!remaining.empty() && lines.size() < 8)
            {
                std::size_t end = 0, next = 0;
                while (next < remaining.size())
                {
                    hui::gfx::next_codepoint(remaining, &next);
                    if (end && face.font->measure(remaining.substr(0, next), size) > max_width)
                        break;
                    end = next;
                }
                const auto text = remaining.substr(0, end);
                widest = std::max(widest, face.font->measure(text, size));
                lines.push_back({face.font, std::string(text)});
                remaining.remove_prefix(end);
            }
            if (lines.size() == 8)
                break;
        }
        if (lines.size() == 8)
            break;
    }
    const unsigned width = lines.empty() ? 0 : static_cast<unsigned>(std::ceil(widest)) + 44;
    subtitle_panel_.reset((1920 - width) / 2, 0, width,
                          lines.size() * 46 + (lines.empty() ? 0 : 20));
    for (unsigned i = 0; i < lines.size(); ++i)
        subtitle_panel_.text(*lines[i].font, lines[i].text,
                             (width - lines[i].font->measure(lines[i].text, size)) / 2, 42 + i * 46,
                             size, max_width);
}

bool PlaybackOsd::draw(void *surface, std::size_t bytes, unsigned pitch, unsigned sh, unsigned vw,
                       unsigned vh, unsigned depth, std::uint64_t now,
                       const std::vector<std::shared_ptr<const iptv::SubtitleCue>> &subtitles)
{
    std::lock_guard lock(mutex_);
    if (!fonts_.regular.font)
        return false;
    if (!started_)
    {
        started_ = true;
        banner_until_ = now + 5000000;
    }
    const bool overlay = list_ || audio_menu_ || history_.paused || now < banner_until_;
    const bool captions = !list_ && !audio_menu_ && !subtitles.empty();
    if (!overlay && !captions)
    {
        subtitle_cues_.clear();
        return false;
    }
    if (!surface)
        return true;
    bool drawn = false;
    if (captions)
    {
        if (subtitle_cues_ != subtitles)
        {
            subtitle_cues_ = subtitles;
            paint_subtitles();
        }
        for (const auto &cue : subtitles)
            if (cue)
                drawn =
                    composite_subtitle_bitmaps(*cue, surface, bytes, pitch, sh, vw, vh, depth) ||
                    drawn;
        if (subtitle_panel_.height)
        {
            subtitle_panel_.y = (overlay ? (history_.available ? 660 : 740) : 1030) -
                                 subtitle_panel_.height;
            drawn = subtitle_panel_.composite(surface, bytes, pitch, sh, vw, vh, depth) || drawn;
        }
    }
    if (overlay && dirty_)
        paint();
    return (overlay && panel_.composite(surface, bytes, pitch, sh, vw, vh, depth)) || drawn;
}
} // namespace ptv
