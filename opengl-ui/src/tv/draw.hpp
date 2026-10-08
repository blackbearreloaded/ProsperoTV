// ProsperoTV - Drawing the screens share: channel artwork, tiles, chips, the mark.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "tv/shared.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ptv
{

// Text as a face can write it: letters of a script the baked fonts do not
// hold (they cover Latin, Greek and Cyrillic) are left out instead of being
// drawn as question marks, and the spaces around them are closed up.
std::string readable(const ui::FontRef &font, std::string_view text);
// A channel's name for the screen. display_name() when the fonts can write
// it; otherwise what is left of it, then the playlist's own id for the
// channel, then the word "Channel". `notes` as display_name() fills them.
std::string shown_name(const ui::Fonts &fonts, const iptv::ChannelView &channel,
                       std::vector<std::string> *notes = nullptr);
// The face a text is written with: `usual` when it holds every letter of it;
// otherwise the one of the usual face, the Chinese and Japanese face
// (fonts.hand) and the Korean face (fonts.pixel) that holds the most of it.
const ui::FontRef &face_for(const ui::Fonts &fonts, const ui::FontRef &usual,
                            std::string_view text);
// The display face when it holds every letter of the text, else face_for() the semibold one.
const ui::FontRef &title_face(const ui::Fonts &fonts, std::string_view text);

// A channel without a picture gets a ground of its own and its initials.
struct ArtColors
{
    Color top;
    Color bottom;
    Color accent; // the light it gives off when focused
};
ArtColors art_colors(std::string_view channel_id);
// The channel's picture as a television set standing in r: antenna on top,
// the shell with its screen and knobs. tv_body() is the shell's
// place in r, for whoever puts a shadow or a light behind it.
Rect tv_body(const Rect &r);
void draw_channel_art(gfx::DrawList &list, const ui::Fonts &fonts, const Rect &r, float radius,
                      const iptv::ChannelView &channel, ImageTexture image = {});
// The set without a picture: rods, shell, knobs. Returns where its screen is
// (the corner radius of a screen is 0.13 of its height).
Rect draw_tv_shell(gfx::DrawList &list, const Rect &r, float radius, const ArtColors &colors);
// Only the screen: the channel's ground and initials behind curved glass.
void draw_channel_screen(gfx::DrawList &list, const ui::Fonts &fonts, const Rect &r, float radius,
                         const iptv::ChannelView &channel, ImageTexture image = {});
// The pair of rods of a set whose top edge is at base_y.
void draw_antenna(gfx::DrawList &list, float cx, float base_y, float height, Color color);

// The app's mark: a sun setting behind a horizon.
void draw_mark(gfx::DrawList &list, float cx, float cy, float size);

// A glass panel in the theme. Over the screens (canvas.glass set) it frosts
// what is behind it; inside a screen it is a tint with a hairline of light,
// which over the soft sky reads the same.
void draw_glass(ui::Canvas &canvas, const ui::Theme &theme, const Rect &r, float radius = -1.0f);

// A chip as wide as its label, vertically centred on cy. Returns its width.
float chip_width(const ui::Painter &paint, std::string_view label, float size = 19.0f);
float draw_chip(ui::Painter &paint, float x, float cy, std::string_view label,
                float selected = 0.0f, float height = 40.0f);
// A chip with a status dot before its label.
float draw_status_chip(ui::Canvas &canvas, const ui::Theme &theme, float x, float cy,
                       std::string_view label, Color dot, float height = 40.0f);

// One channel of a grid. focus is 0..1.
void draw_channel_tile(ui::Canvas &canvas, const Shared &shared, const Rect &cell,
                       const iptv::ChannelView &channel, float focus);
// The shape of a tile that has nothing to show yet.
void draw_tile_placeholder(ui::Canvas &canvas, const Shared &shared, const Rect &cell);

} // namespace ptv
