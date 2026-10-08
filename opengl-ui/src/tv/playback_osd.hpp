// ProsperoTV - Channel controls and text over the foreground video.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tv/model.hpp"
#include "ui/fonts.hpp"
#include "iptv_player.h"
#include <mutex>

namespace ptv
{
// A bounded, opaque luma panel in the interface's 1920 x 1080 coordinates.
// The presenter supplies a copy of the decoder surface; reference frames are
// never modified. Native Main10 surfaces hold their ten bits in the low bits.
struct VideoPanel
{
    unsigned x = 0, y = 0, width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
    void reset(unsigned left, unsigned top, unsigned w, unsigned h);
    void text(const hui::gfx::Font &font, std::string_view value, float left, float baseline,
              float size, float max_width, std::uint8_t ink = 235);
    bool composite(void *surface, std::size_t bytes, unsigned pitch, unsigned surface_height,
                   unsigned visible_width, unsigned visible_height, unsigned depth) const;
};

class PlaybackOsd
{
  public:
    PlaybackOsd(Model &model, const PlayRequest &request);
    bool load_fonts(const std::string &directory);
    // The control thread alone calls input and reads selected_channel. Drawing
    // can happen on the presentation worker, protected by the same mutex.
    int input(int action, std::uint64_t now);
    void set_audio_state(const iptv_player_audio_state_t &state);
    std::optional<std::uint32_t> take_audio_selection();
    std::optional<unsigned> selected_channel() const
    {
        return selected_;
    }
    bool draw(void *surface, std::size_t bytes, unsigned pitch, unsigned surface_height,
              unsigned visible_width, unsigned visible_height, unsigned depth, std::uint64_t now);

  private:
    void update_guide();
    void paint();
    int select(unsigned index);
    void line(std::string_view value, float x, float y, float size, float width);
    Model &model_;
    std::string current_id_, title_, now_, next_;
    bool live_ = false, list_ = false, dirty_ = true, started_ = false;
    bool audio_menu_ = false;
    unsigned audio_focus_ = 0;
    iptv_player_audio_state_t audio_{};
    std::optional<std::uint32_t> audio_selection_;
    std::uint64_t banner_until_ = 0, guide_minute_ = 0;
    std::vector<unsigned> channels_;
    unsigned focus_ = 0;
    int current_position_ = -1;
    std::optional<unsigned> selected_;
    std::mutex mutex_;
    hui::gfx::Font regular_, east_asian_, korean_;
    hui::ui::Fonts fonts_;
    VideoPanel panel_;
};
} // namespace ptv
