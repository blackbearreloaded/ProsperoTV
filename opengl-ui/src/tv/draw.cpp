// ProsperoTV - Drawing the screens share: channel artwork, tiles, chips, the mark.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/draw.hpp"
#include "tv/platform.hpp"

#include "ui/components/overlay.hpp"
#include "ui/components/progress.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace ptv
{

namespace
{

struct Ground
{
    std::uint32_t top;
    std::uint32_t bottom;
    std::uint32_t accent;
};

// Channel grounds: every one sits comfortably on the red page.
constexpr Ground kGrounds[] = {
    {0xc9483f, 0x4a1a2c, 0xff9a7a}, // ember
    {0x8a3f9a, 0x2b1746, 0xd9a3ff}, // orchid
    {0xc98d2f, 0x4a2c12, 0xffd98a}, // amber
    {0x2f8fa3, 0x12303f, 0x8fe3f0}, // lagoon
    {0x4fae6d, 0x163325, 0xa6f0bf}, // fern
    {0x5d6cd6, 0x1c2152, 0xb0b9ff}, // dusk blue
    {0xd4587a, 0x47162b, 0xffa8c2}, // rose
    {0x35b0a4, 0x10363a, 0x9df0e4}, // teal
};

// FNV-1a: stable across builds, which a std::hash is not promised to be.
std::uint32_t hash(std::string_view text)
{
    std::uint32_t value = 2166136261u;
    for (const char c : text)
        value = (value ^ static_cast<unsigned char>(c)) * 16777619u;
    // Ids that differ in one digit must not end up neighbours: stir the bits.
    value ^= value >> 16;
    value *= 0x85ebca6bu;
    value ^= value >> 13;
    value *= 0xc2b2ae35u;
    value ^= value >> 16;
    return value;
}

// Letters and digits of a text, whatever the script.
int visible_count(std::string_view text)
{
    int count = 0;
    for (std::size_t index = 0; index < text.size();)
        if (gfx::next_codepoint(text, &index) > 0x20)
            ++count;
    return count;
}

} // namespace

std::string readable(const ui::FontRef &font, std::string_view text)
{
    std::string kept;
    kept.reserve(text.size());
    bool space = false;
    for (std::size_t index = 0; index < text.size();)
    {
        const std::size_t start = index;
        const std::uint32_t codepoint = gfx::next_codepoint(text, &index);
        if (codepoint <= 0x20)
        {
            space = !kept.empty();
            continue;
        }
        if (!font.font->has_glyph(codepoint))
            continue;
        if (space)
            kept.push_back(' ');
        space = false;
        kept.append(text.substr(start, index - start));
    }
    // Punctuation that stood between two words now stands alone at an end.
    static constexpr std::string_view kLoose = " -|/\\:.,;";
    while (!kept.empty() && kLoose.find(kept.back()) != std::string_view::npos)
        kept.pop_back();
    std::size_t first = 0;
    while (first < kept.size() && kLoose.find(kept[first]) != std::string_view::npos)
        ++first;
    return kept.substr(first);
}

const ui::FontRef &face_for(const ui::Fonts &fonts, const ui::FontRef &usual, std::string_view text)
{
    const ui::FontRef *faces[] = {&usual, &fonts.hand, &fonts.pixel};
    const ui::FontRef *best = &usual;
    int most = -1;
    for (const ui::FontRef *face : faces)
    {
        if (face->font == nullptr)
            continue;
        int held = 0;
        int missing = 0;
        for (std::size_t index = 0; index < text.size();)
        {
            const std::uint32_t codepoint = gfx::next_codepoint(text, &index);
            if (codepoint <= 0x20)
                continue;
            if (face->font->has_glyph(codepoint))
                ++held;
            else
                ++missing;
        }
        if (missing == 0)
            return face == &usual || most < held ? *face : *best;
        if (held > most)
        {
            most = held;
            best = face;
        }
    }
    return *best;
}

std::string shown_name(const ui::Fonts &fonts, const iptv::ChannelView &channel,
                       std::vector<std::string> *notes)
{
    const std::string name = display_name(channel, notes);
    if (notes != nullptr)
    {
        for (std::string &note : *notes)
            note = readable(face_for(fonts, fonts.semibold, note), note);
        std::erase_if(*notes, [](const std::string &note) { return note.empty(); });
    }
    const std::string kept = readable(face_for(fonts, fonts.semibold, name), name);
    if (kept.size() == name.size())
        return name;
    // Most of the name survived: a letter or two the fonts lack is no reason
    // to give it up.
    const int left = visible_count(kept);
    if (left >= 3 && left * 2 >= visible_count(name))
        return kept;
    // The playlist's id for the channel is written in plain letters
    // ("Name.cc"): better than a few stray characters.
    const std::string id =
        readable(fonts.semibold, channel.tvg_id.substr(0, channel.tvg_id.find_first_of(".@")));
    if (!id.empty())
        return id;
    return left >= 2 ? kept : "Channel";
}

const ui::FontRef &title_face(const ui::Fonts &fonts, std::string_view text)
{
    for (std::size_t index = 0; index < text.size();)
    {
        const std::uint32_t codepoint = gfx::next_codepoint(text, &index);
        if (codepoint > 0x20 && !fonts.display.font->has_glyph(codepoint))
            return face_for(fonts, fonts.semibold, text);
    }
    return fonts.display;
}

ArtColors art_colors(std::string_view channel_id)
{
    const Ground &ground = kGrounds[hash(channel_id) % std::size(kGrounds)];
    return {Color::rgb(ground.top), Color::rgb(ground.bottom), Color::rgb(ground.accent)};
}

Rect tv_body(const Rect &r)
{
    const float antenna = r.h * 0.17f;
    return {r.x, r.y + antenna, r.w, r.h - antenna};
}

void draw_antenna(gfx::DrawList &list, float cx, float base_y, float height, Color color)
{
    const float thick = std::max(2.0f, height * 0.085f);
    const float tip = thick * 1.15f;
    // Two rods, the left one leaning further, as on a set that has been tuned by hand.
    const float left_x = cx - height * 0.95f;
    const float left_y = base_y - height * 0.86f;
    const float right_x = cx + height * 0.78f;
    const float right_y = base_y - height;
    list.line(cx - height * 0.1f, base_y, left_x, left_y, thick, color);
    list.line(cx + height * 0.1f, base_y, right_x, right_y, thick, color);
    list.circle(left_x, left_y, tip, color);
    list.circle(right_x, right_y, tip, color);
    // The mount they turn in.
    const float mount = height * 0.56f;
    list.rounded_rect({cx - mount * 0.5f, base_y - mount * 0.3f, mount, mount * 0.6f}, mount * 0.3f,
                      color);
}

void draw_channel_art(gfx::DrawList &list, const ui::Fonts &fonts, const Rect &r, float radius,
                      const iptv::ChannelView &channel, ImageTexture image)
{
    const Rect screen = draw_tv_shell(list, r, radius, art_colors(channel.id));
    draw_channel_screen(list, fonts, screen, screen.h * 0.13f, channel, image);
}

Rect draw_tv_shell(gfx::DrawList &list, const Rect &r, float radius, const ArtColors &colors)
{
    const Rect body = tv_body(r);
    const Color metal = gfx::mix(colors.accent, kWhite, 0.45f).with_alpha(0.9f);
    draw_antenna(list, body.cx(), body.y, r.h * 0.17f * 0.92f, metal);

    // The shell, in the channel's colour gone dark.
    const float corner = std::min(radius, body.h * 0.14f);
    list.gradient_rect(body, corner, gfx::mix(colors.top, tone::night, 0.42f),
                       gfx::mix(colors.bottom, tone::night, 0.70f));
    list.bordered_rect(body, corner, kClear, std::max(1.5f, body.w * 0.004f),
                       kWhite.with_alpha(0.18f));

    // The screen on the left, the controls in a strip on the right.
    const float bezel = body.h * 0.075f;
    const float strip = body.w * 0.135f;
    const Rect screen{body.x + bezel, body.y + bezel, body.w - 2.0f * bezel - strip,
                      body.h - 2.0f * bezel};
    list.rounded_rect(screen.inset(-bezel * 0.32f), screen.h * 0.13f + bezel * 0.32f,
                      tone::night.with_alpha(0.62f));

    const float kx = screen.x + screen.w + (strip + bezel) * 0.5f;
    const float knob = strip * 0.27f;
    for (int i = 0; i < 2; ++i)
    {
        const float ky = body.y + body.h * (0.25f + 0.24f * static_cast<float>(i));
        list.circle(kx, ky, knob, tone::night.with_alpha(0.55f));
        list.ring(kx, ky, knob, std::max(1.5f, knob * 0.16f), metal.with_alpha(0.75f));
        // Each knob's mark, turned to its own place.
        const float turn = i == 0 ? -0.7f : 0.5f;
        list.line(kx, ky, kx + std::sin(turn) * knob * 0.72f, ky - std::cos(turn) * knob * 0.72f,
                  std::max(1.5f, knob * 0.18f), metal);
    }
    // The speaker grille and the light that says the set is on.
    for (int i = 0; i < 4; ++i)
    {
        const float gy = body.y + body.h * (0.68f + 0.05f * static_cast<float>(i));
        list.line(kx - knob, gy, kx + knob, gy, std::max(1.5f, body.h * 0.008f),
                  tone::night.with_alpha(0.55f));
    }
    list.circle(kx, body.y + body.h * 0.60f, std::max(2.0f, knob * 0.2f), colors.accent);
    return screen;
}

void draw_channel_screen(gfx::DrawList &list, const ui::Fonts &fonts, const Rect &r, float radius,
                         const iptv::ChannelView &channel, ImageTexture image)
{
    const ArtColors colors = art_colors(channel.id);
    list.gradient_rect(r, radius, colors.top, colors.bottom);
    // A soft disc behind the letters gives the flat ground a centre.
    const float disc = std::min(r.w, r.h) * 0.42f;
    list.circle(r.cx(), r.cy(), disc, kWhite.with_alpha(0.07f));
    if (image.id != 0 && image.width > 0 && image.height > 0)
    {
        const float scale = std::min(r.w * 0.82f / image.width, r.h * 0.78f / image.height);
        const float width = image.width * scale, height = image.height * scale;
        list.image(image.id, {r.cx() - width / 2, r.cy() - height / 2, width, height}, {0, 0, 1, 1},
                   kWhite);
    }
    else
    {
        const std::string letters = monogram(channel);
        const float size = std::min(r.h * 0.44f, r.w * 0.3f);
        ui::text(list, face_for(fonts, fonts.display, letters), letters, r.cx(),
                 r.cy() + size * 0.36f, size, kWhite.with_alpha(0.94f), gfx::Align::center);
    }
    // The glass: light from above, fading before the middle.
    list.gradient_rect({r.x, r.y, r.w, r.h * 0.46f}, radius, kWhite.with_alpha(0.11f),
                       kWhite.with_alpha(0.0f));
    // A hairline of light is what makes a flat rectangle read as an object.
    list.bordered_rect(r, radius, kClear, std::max(1.5f, r.w * 0.004f), kWhite.with_alpha(0.16f));
}

void draw_mark(gfx::DrawList &list, float cx, float cy, float size)
{
    const float radius = size * 0.5f;
    list.glow({cx - radius, cy - radius, size, size}, radius, size * 0.5f,
              tone::accent.with_alpha(0.3f));
    list.gradient_rect({cx - radius, cy - radius, size, size}, radius, tone::accent, tone::ember);
    // The horizon and one ripple under it, cut into the disc.
    const float line = std::max(2.0f, size * 0.07f);
    list.line(cx - radius * 0.62f, cy + radius * 0.3f, cx + radius * 0.62f, cy + radius * 0.3f,
              line, tone::night.with_alpha(0.85f));
    list.line(cx - radius * 0.34f, cy + radius * 0.58f, cx + radius * 0.34f, cy + radius * 0.58f,
              line, tone::night.with_alpha(0.6f));
}

void draw_glass(ui::Canvas &canvas, const ui::Theme &theme, const Rect &r, float radius)
{
    if (canvas.glass != 0)
    {
        ui::draw_overlay_panel(canvas, theme, r, true, 0.55f, radius);
        return;
    }
    gfx::DrawList &list = canvas.list;
    const float corner = radius < 0.0f ? theme.radius_card : radius;
    list.shadow({r.x, r.y + theme.shadow_offset, r.w, r.h}, corner, theme.shadow_blur,
                theme.shadow.with_alpha(0.7f));
    list.rounded_rect(r, corner, tone::panel.with_alpha(0.66f));
    list.bordered_rect(r, corner, kClear, 1.5f, theme.light.with_alpha(0.7f));
}

float chip_width(const ui::Painter &paint, std::string_view label, float size)
{
    return paint.label_width(label, size) + 34.0f;
}

float draw_chip(ui::Painter &paint, float x, float cy, std::string_view label, float selected,
                float height)
{
    const float width = chip_width(paint, label);
    paint.chip({x, cy - height * 0.5f, width, height}, label, selected, {});
    return width;
}

float draw_status_chip(ui::Canvas &canvas, const ui::Theme &theme, float x, float cy,
                       std::string_view label, Color dot, float height)
{
    gfx::DrawList &list = canvas.list;
    const float width = canvas.fonts.semibold.measure(label, 19.0f) + 54.0f;
    const Rect chip{x, cy - height * 0.5f, width, height};
    list.rounded_rect(chip, height * 0.5f, kWhite.with_alpha(0.09f));
    list.bordered_rect(chip, height * 0.5f, kClear, 1.5f, kWhite.with_alpha(0.16f));
    list.glow({chip.x + 16.0f, cy - 5.0f, 10.0f, 10.0f}, 5.0f, 8.0f, dot.with_alpha(0.5f));
    list.circle(chip.x + 21.0f, cy, 5.0f, dot);
    ui::text(list, canvas.fonts.semibold, label, chip.x + 36.0f, baseline_for(cy, 19.0f), 19.0f,
             theme.text);
    return width;
}

void draw_channel_tile(ui::Canvas &canvas, const Shared &shared, const Rect &cell,
                       const iptv::ChannelView &channel, float focus)
{
    gfx::DrawList &list = canvas.list;
    const ui::Fonts &fonts = canvas.fonts;
    const ui::Theme &theme = shared.theme;
    constexpr float kRadius = 20.0f;
    constexpr float kInset = 10.0f;
    constexpr float kArtHeight = 104.0f;

    // At rest a tile is a veil over the page. In focus it turns solid: the
    // light the grid puts around it must not shine through and wash the text.
    // The card is a television set: rods above it, the
    // channel's picture as its screen. In focus the rods take the channel's light.
    const Color metal = gfx::mix(kWhite.with_alpha(0.46f),
                                 gfx::mix(art_colors(channel.id).accent, kWhite, 0.4f), focus);
    draw_antenna(list, cell.cx(), cell.y + 1.0f, 18.0f, metal);
    list.rounded_rect(cell, kRadius,
                      gfx::mix(kWhite.with_alpha(0.06f), tone::panel_lit.with_alpha(0.97f), focus));
    list.bordered_rect(cell, kRadius, kClear, 1.5f, kWhite.with_alpha(0.12f + 0.14f * focus));
    const Rect art{cell.x + kInset, cell.y + kInset, cell.w - 2.0f * kInset, kArtHeight};
    list.rounded_rect(art.inset(-2.0f), 18.0f, tone::night.with_alpha(0.5f));
    draw_channel_screen(list, fonts, art, 16.0f, channel, shared.images.find(channel.tvg_logo));

    const float x = cell.x + 20.0f;
    const float room = cell.w - 40.0f;
    const std::string name = shown_name(fonts, channel);
    const ui::FontRef &name_face = face_for(fonts, fonts.semibold, name);
    ui::text(list, name_face, name_face.font->fit(name, 23.0f, room), x, cell.y + 150.0f, 23.0f,
             theme.text);

    const std::string size = resolution_label(channel);
    const float size_width = size.empty() ? 0.0f : fonts.mono.measure(size, 17.0f) + 14.0f;
    float at = x;
    if (channel.playback_status == iptv::PlaybackStatus::failed)
    {
        // It would not open last time: say so before the player tries again.
        list.circle(at + 5.0f, cell.y + 173.0f, 5.0f, tone::bad);
        at += 18.0f;
    }
    const auto *programme =
        shared.model.guide().now(channel.id, static_cast<std::int64_t>(platform::unix_time()));
    const std::string category = programme ? programme->title : category_of(channel);
    const ui::FontRef &category_face = face_for(fonts, fonts.regular, category);
    ui::text(list, category_face,
             category_face.font->fit(readable(category_face, category), 19.0f,
                                     room - size_width - (at - x)),
             at, cell.y + 180.0f, 19.0f, theme.text_muted);
    if (!size.empty())
        ui::text(list, fonts.mono, size, cell.x + cell.w - 20.0f, cell.y + 180.0f, 17.0f,
                 theme.text_muted, gfx::Align::right);

    if (shared.model.is_favorite(channel))
    {
        const float sx = art.x + art.w - 24.0f;
        const float sy = art.y + 24.0f;
        list.circle(sx, sy, 16.0f, tone::night.with_alpha(0.55f));
        list.star(sx, sy - 1.0f, 10.0f, tone::accent);
    }
}

void draw_tile_placeholder(ui::Canvas &canvas, const Shared &shared, const Rect &cell)
{
    gfx::DrawList &list = canvas.list;
    constexpr float kRadius = 20.0f;
    const Color bone = kWhite.with_alpha(0.09f);
    list.rounded_rect(cell, kRadius, kWhite.with_alpha(0.04f));
    list.bordered_rect(cell, kRadius, kClear, 1.5f, kWhite.with_alpha(0.08f));
    list.rounded_rect({cell.x + 10.0f, cell.y + 10.0f, cell.w - 20.0f, 104.0f}, 13.0f, bone);
    list.rounded_rect({cell.x + 20.0f, cell.y + 132.0f, (cell.w - 40.0f) * 0.72f, 20.0f}, 8.0f,
                      bone);
    list.rounded_rect({cell.x + 20.0f, cell.y + 166.0f, (cell.w - 40.0f) * 0.42f, 14.0f}, 6.0f,
                      bone);
    if (!shared.settings.reduced_motion)
    {
        // One band of light crosses every waiting tile at the same pace.
        const float period = 1.7f;
        const float phase =
            (shared.clock - period * static_cast<float>(static_cast<int>(shared.clock / period))) /
            period;
        const float centre = cell.x - cell.w * 0.3f + phase * cell.w * 1.6f;
        ui::draw_sweep(list, centre, cell.w * 0.18f, cell.x + 10.0f, cell.x + cell.w - 10.0f,
                       cell.y + 10.0f, 104.0f, kWhite.with_alpha(0.07f));
    }
}

} // namespace ptv
