// ProsperoTV - Live TV and Favorites: the channel in focus as a hero, the list as a grid.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/browse_screen.hpp"
#include "tv/platform.hpp"

#include "tv/draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

namespace ptv
{

namespace
{

constexpr int kColumns = 5;
constexpr int kRowsInView = 2;
constexpr float kTileHeight = 200.0f;
constexpr float kGapX = 22.0f;
constexpr float kGapY = 20.0f;
constexpr float kGridTop = 510.0f;
constexpr float kGridPad = 20.0f; // room for the focused tile to grow and wear its ring
constexpr float kListY = 480.0f;  // the centre line of the row above the grid
// The letters of the alphabet stand in a column right of the grid.
constexpr float kRailRoom = 58.0f; // what the grid gives up for them
constexpr float kRailX = kWidth - kMargin - 15.0f;
constexpr float kRailTop = 506.0f; // the centre of '#'
constexpr float kRailPitch = 18.5f;
constexpr float kRailText = 15.0f;
constexpr float kGridWidth = kWidth - 2.0f * kMargin - kRailRoom;
// A held L2 or R2: the wait before the second page, then the pace, then the
// pace after a long hold.
constexpr float kPageDelay = 0.40f;
constexpr float kPagePace = 0.12f;
constexpr float kPageFastAfter = 1.6f;
constexpr float kPageFastPace = 0.07f;
constexpr Rect kHeroArt{1304.0f, 126.0f, 520.0f, 300.0f};
constexpr float kHeroRadius = 28.0f;
constexpr float kHeroText = 1120.0f; // the width the hero's words may take

// The lists of Live TV, in the order of their chips. Favorites has a tab of
// its own.
constexpr Group kChips[] = {Group::all, Group::recent, Group::news, Group::sports, Group::kids};
constexpr int kChipCount = static_cast<int>(std::size(kChips));

int chip_of(Group group)
{
    for (int i = 0; i < kChipCount; ++i)
        if (kChips[i] == group)
            return i;
    return 0;
}

} // namespace

BrowseScreen::BrowseScreen(Shared &shared) : shared_(shared)
{
    const ui::Theme &theme = shared.theme;

    grid_.style.theme = theme;
    grid_.style.columns = kColumns;
    grid_.style.cell_height = kTileHeight;
    grid_.style.gap_x = kGapX;
    grid_.style.gap_y = kGapY;
    grid_.style.padding = kGridPad;
    grid_.style.card.radius = 20.0f;
    grid_.style.card.focus_scale = 1.04f;
    grid_.style.card.lift = 4.0f;
    grid_.style.card.glow = true;
    grid_.style.scroll_thumb = false; // the letters say where the list is
    grid_.style.exits.right = true;
    grid_.set_bounds({kMargin - kGridPad, kGridTop, kGridWidth + 2.0f * kGridPad,
                      kRowsInView * (kTileHeight + kGapY) + 2.0f * kGridPad + 12.0f});
    rail_y_.snap(kRailTop);
    grid_.content =
        [this](ui::Canvas &canvas, const Rect &cell, const ui::CardItem &, int index, float focus)
    {
        const Model &model = shared_.model;
        if (index < 0 || static_cast<unsigned>(index) >= model.visible_count())
        {
            draw_tile_placeholder(canvas, shared_, cell);
            return;
        }
        draw_channel_tile(canvas, shared_, cell,
                          model.channel(model.visible(static_cast<unsigned>(index))), focus);
    };
    grid_.accent = [this](int index)
    {
        const Model &model = shared_.model;
        if (index < 0 || static_cast<unsigned>(index) >= model.visible_count())
            return kClear;
        return art_colors(model.channel(model.visible(static_cast<unsigned>(index))).id).accent;
    };

    groups_.style.theme = theme;
    groups_.style.kind = ui::TabKind::pill;
    groups_.style.height = 48.0f;
    groups_.style.text_size = 22.0f;
    groups_.style.padding = 22.0f;
    groups_.style.gap = 10.0f;
    groups_.style.track = false;
    groups_.style.on_page = true;
    std::vector<ui::TabItem> chips;
    for (const Group group : kChips)
        chips.push_back({Model::group_name(group)});
    groups_.set_tabs(std::move(chips));
    groups_.set_bounds({kMargin, kListY - 24.0f, 1000.0f, 48.0f});
    groups_.set_focused(false);

    empty_.style.theme = theme;
    empty_.style.title_size = 44.0f;
    empty_.style.body_size = 26.0f;
    empty_.style.hint_size = 26.0f;
    empty_.style.max_text_width = 820.0f;
    empty_.set_bounds({420.0f, 560.0f, 1080.0f, 380.0f});
    empty_.icon = [](ui::Canvas &canvas, const Rect &area)
    { draw_mark(canvas.list, area.cx(), area.cy(), area.w * 0.8f); };
}

void BrowseScreen::show(bool favorites)
{
    Model &model = shared_.model;
    favorites_ = favorites;
    zone_ = Zone::grid;
    model.set_group(favorites ? Group::favorites : model.view.live_group);
    groups_.set_active(chip_of(model.view.live_group), true);
    grid_.style.exits.up = !favorites;

    const int position = model.position_of(model.view.focused_channel);
    grid_.set_count(static_cast<int>(model.visible_count()));
    grid_.set_focus(std::max(position, 0), true);
    seen_revision_ = model.revision();
    has_previous_ = false;
    swap_.running = false;
    rail_y_.snap(kRailTop + static_cast<float>(current_letter()) * kRailPitch);
    rail_focus_.snap(0.0f);
    page_dir_ = 0;
    enter();
}

void BrowseScreen::enter()
{
    age_ = 0.0f;
    grid_.enter();
    empty_.enter();
}

unsigned BrowseScreen::focused_index() const
{
    return shared_.model.visible(static_cast<unsigned>(grid_.focus()));
}

std::optional<iptv::ChannelView> BrowseScreen::focused() const
{
    const Model &model = shared_.model;
    if (!model.has_catalog() || grid_.count() == 0 ||
        static_cast<unsigned>(grid_.focus()) >= model.visible_count())
        return std::nullopt;
    return model.channel(focused_index());
}

int BrowseScreen::current_letter() const
{
    return grid_.count() > 0 ? shared_.model.letter_at(static_cast<unsigned>(grid_.focus())) : 0;
}

std::vector<std::string> BrowseScreen::image_urls() const
{
    std::vector<std::string> urls;
    if (const auto channel = focused())
        urls.emplace_back(channel->tvg_logo);
    // Two rows in view, plus the rows entering/leaving during a scroll.
    const int first = std::max(0, grid_.focus() / kColumns * kColumns - kColumns);
    const int end = std::min(grid_.count(), first + kColumns * 5);
    for (int i = first; i < end; ++i)
    {
        const auto rect = grid_.cell_rect(i);
        if (rect.y + rect.h < kGridTop - kTileHeight || rect.y > 1000)
            continue;
        const auto channel = shared_.model.channel(shared_.model.visible(static_cast<unsigned>(i)));
        if (!channel.tvg_logo.empty())
            urls.emplace_back(channel.tvg_logo);
    }
    return urls;
}

int BrowseScreen::page_turn(const InputFrame &input, bool *repeat)
{
    *repeat = false;
    handled_ = true;
    const int pressed = input.is_pressed(Action::jump_next)   ? 1
                        : input.is_pressed(Action::jump_prev) ? -1
                                                              : 0;
    if (pressed != 0)
    {
        page_dir_ = pressed;
        page_hold_ = 0.0f;
        page_due_ = kPageDelay;
        return pressed;
    }
    const int held = input.is_held(Action::jump_next)   ? 1
                     : input.is_held(Action::jump_prev) ? -1
                                                        : 0;
    if (held == 0 || held != page_dir_)
    {
        page_dir_ = 0;
        return 0;
    }
    if (page_hold_ < page_due_)
        return 0;
    page_due_ += page_hold_ >= kPageFastAfter ? kPageFastPace : kPagePace;
    *repeat = true;
    return held;
}

void BrowseScreen::show_focus(int before, bool animate)
{
    if (animate)
    {
        begin_swap(before);
        return;
    }
    has_previous_ = false;
    swap_.running = false;
}

float BrowseScreen::appear(int order) const
{
    return tween::stagger(age_, order, 0.06f, 0.5f);
}

void BrowseScreen::begin_swap(int from)
{
    const Model &model = shared_.model;
    has_previous_ = from >= 0 && static_cast<unsigned>(from) < model.visible_count();
    if (has_previous_)
    {
        previous_index_ = model.visible(static_cast<unsigned>(from));
        // Kept across frames, so with texts of its own: a download may
        // replace the catalog while the hero changes hands.
        previous_ = model.channel(previous_index_).Copy();
        previous_favorite_ = model.is_favorite(previous_);
    }
    travel_ = grid_.focus() >= from ? 1.0f : -1.0f;
    swap_.start(0.34f);
}

// The list changed under the grid: start it from its first channel.
void BrowseScreen::restart_list()
{
    const Model &model = shared_.model;
    grid_.set_count(static_cast<int>(model.visible_count()));
    grid_.set_focus(0, true);
    seen_revision_ = model.revision();
    has_previous_ = false;
    swap_.running = false;
    focused_id_.clear();
    age_ = 0.32f; // the hero stays; only the list arrives again
    grid_.enter();
    empty_.enter();
}

void BrowseScreen::apply_group(int chip)
{
    Model &model = shared_.model;
    const Group group = kChips[std::clamp(chip, 0, kChipCount - 1)];
    model.view.live_group = group;
    model.set_group(group);
    restart_list();
}

BrowseScreen::Result BrowseScreen::handle(const InputFrame &input, ui::Feedback &feedback)
{
    Model &model = shared_.model;
    bool repeat = false;
    const int turn = page_turn(input, &repeat);
    if (!model.has_catalog())
    {
        if (model.catalog_failed() && !model.refreshing() && input.is_pressed(Action::confirm))
        {
            feedback.play(audio::Cue::select);
            model.refresh();
        }
        return Result::none;
    }
    if (input.is_pressed(Action::north))
    {
        feedback.play(audio::Cue::open);
        return Result::search;
    }

    if (zone_ == Zone::groups)
    {
        if (input.nav == Direction::down || input.is_pressed(Action::confirm))
        {
            if (grid_.count() > 0)
            {
                zone_ = Zone::grid;
                feedback.play(audio::Cue::focus, 0.97f);
            }
            else
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            }
            return Result::none;
        }
        if (groups_.handle(input, feedback) == ui::Event::changed)
            apply_group(groups_.active());
        return Result::none;
    }

    if (grid_.count() == 0)
    {
        if (favorites_ && !model.filtering() && input.is_pressed(Action::confirm))
        {
            feedback.play(audio::Cue::select);
            return Result::go_live;
        }
        if (!favorites_ && input.nav == Direction::up)
        {
            zone_ = Zone::groups;
            feedback.play(audio::Cue::focus, 1.03f);
        }
        return Result::none;
    }

    if (zone_ == Zone::rail)
    {
        if (input.nav == Direction::left || input.is_pressed(Action::confirm))
        {
            zone_ = Zone::grid;
            feedback.play(audio::Cue::focus, 0.97f);
        }
        else if (input.nav == Direction::up || input.nav == Direction::down)
        {
            // The next letter that has channels under it.
            const int step = input.nav == Direction::down ? 1 : -1;
            int letter = current_letter() + step;
            while (letter >= 0 && letter < kLetterCount && model.letter_start(letter) < 0)
                letter += step;
            if (letter < 0 || letter >= kLetterCount)
            {
                if (!input.nav_repeat)
                    feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            }
            else
            {
                const int before = grid_.focus();
                const int target = model.letter_start(letter);
                // A long way is not travelled: the list is simply there. The
                // letter's first row goes to the top of the view, so that its
                // channels follow it.
                const bool far = std::abs(target - before) > 3 * kColumns;
                grid_.set_focus(std::min(target + kColumns * (kRowsInView - 1), grid_.count() - 1),
                                far);
                grid_.set_focus(target, far);
                feedback.play(audio::Cue::focus, 1.1f - 0.008f * static_cast<float>(letter));
                show_focus(before, !input.nav_repeat);
            }
        }
        return Result::none;
    }

    const int before = grid_.focus();
    const ui::Event event = grid_.handle(input, feedback);
    if (grid_.exit() == Direction::up)
    {
        zone_ = Zone::groups;
        feedback.play(audio::Cue::focus, 1.03f);
        return Result::none;
    }
    if (grid_.exit() == Direction::right)
    {
        // Only a press of its own leads onto the letters: a held Right stops
        // at the end of the row.
        if (!input.nav_repeat)
        {
            zone_ = Zone::rail;
            feedback.play(audio::Cue::focus, 1.03f);
        }
        return Result::none;
    }
    if (event == ui::Event::moved)
        begin_swap(before);
    if (event == ui::Event::activated && model.play(focused_index()))
        return Result::play;

    if (input.is_pressed(Action::west))
    {
        const iptv::ChannelView &channel = model.channel(focused_index());
        const std::string name = shown_name(shared_.fonts, channel);
        keep_place_ = favorites_;
        switch (model.toggle_favorite(focused_index()))
        {
        case Model::Starred::added:
            feedback.play(audio::Cue::favorite_on);
            shared_.toasts.push(ui::StatusKind::success, "Added to Favorites", name);
            break;
        case Model::Starred::removed:
            feedback.play(audio::Cue::favorite_off);
            shared_.toasts.push(ui::StatusKind::info, "Removed from Favorites", name);
            break;
        case Model::Starred::failed:
            keep_place_ = false;
            feedback.play(audio::Cue::error);
            shared_.toasts.push(ui::StatusKind::danger, "Favorites could not be saved",
                                "The console's storage refused the change.");
            break;
        }
        star_.trigger();
    }

    // L2 and R2 turn a whole screenful, and keep turning while they are held.
    if (turn != 0)
    {
        const int target = std::clamp(before + turn * kColumns * kRowsInView, 0, grid_.count() - 1);
        if (target == before)
        {
            page_dir_ = 0; // the end of the list ends the hold
            if (!repeat)
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
        else
        {
            // A press glides to the next page; a hold flips them.
            grid_.set_focus(target, repeat);
            feedback.play(audio::Cue::tab, turn > 0 ? 0.96f : 1.04f, 0.0f, repeat ? 0.5f : 1.0f);
            show_focus(before, !repeat);
        }
    }
    return Result::none;
}

bool BrowseScreen::back(ui::Feedback &feedback)
{
    if (!shared_.model.has_catalog())
        return false;
    if (zone_ == Zone::rail)
    {
        zone_ = Zone::grid;
        feedback.play(audio::Cue::back);
        return true;
    }
    if (zone_ == Zone::groups)
    {
        if (groups_.active() != 0)
        {
            groups_.set_active(0);
            apply_group(0);
            feedback.play(audio::Cue::back);
            return true;
        }
        if (grid_.count() == 0)
            return false;
        zone_ = Zone::grid;
        feedback.play(audio::Cue::back);
        return true;
    }
    if (grid_.count() > 0 && grid_.focus() > 0)
    {
        // Back from deep in a list is the way to its top.
        const int before = grid_.focus();
        grid_.set_focus(0, false);
        begin_swap(before);
        feedback.play(audio::Cue::back);
        return true;
    }
    if (!favorites_ && groups_.active() != 0)
    {
        zone_ = Zone::groups;
        feedback.play(audio::Cue::back);
        return true;
    }
    return false;
}

void BrowseScreen::update(float dt)
{
    Model &model = shared_.model;
    const bool reduced = shared_.settings.reduced_motion;
    age_ += dt;

    // ---- follow the list ----
    if (model.revision() != seen_revision_)
    {
        seen_revision_ = model.revision();
        const int count = static_cast<int>(model.visible_count());
        const int kept = model.position_of(focused_id_);
        const int old = grid_.focus();
        grid_.set_count(count);
        if (count > 0)
            grid_.set_focus(kept >= 0 ? kept : keep_place_ ? std::min(old, count - 1) : 0, true);
        has_previous_ = false;
        swap_.running = false;
        if (kept < 0)
        {
            grid_.enter();
            empty_.enter();
        }
    }
    else
    {
        grid_.set_count(static_cast<int>(model.visible_count()));
    }
    keep_place_ = false;
    if (const std::optional<iptv::ChannelView> channel = focused())
    {
        focused_id_ = channel->id;
        model.view.focused_channel = focused_id_;
    }
    else
    {
        focused_id_.clear();
    }
    if (zone_ == Zone::rail && grid_.count() == 0)
        zone_ = Zone::grid;
    if (zone_ == Zone::grid && grid_.count() == 0 && !favorites_ && model.has_catalog() &&
        !model.filtering() && groups_.active() != 0)
        zone_ = Zone::groups; // an empty list leaves only its chips to stand on

    // ---- the letters, and a held L2 or R2 ----
    rail_y_.target = kRailTop + static_cast<float>(current_letter()) * kRailPitch;
    rail_focus_.target = zone_ == Zone::rail ? 1.0f : 0.0f;
    if (reduced)
    {
        rail_y_.snap(rail_y_.target);
        rail_focus_.snap(rail_focus_.target);
    }
    rail_y_.update(dt, 24.0f);
    rail_focus_.update(dt, 18.0f);
    if (!handled_)
        page_dir_ = 0; // something else had the controller this frame
    handled_ = false;
    if (page_dir_ != 0)
        page_hold_ += dt;

    // ---- the chips say how many each list holds ----
    for (int i = 0; i < kChipCount; ++i)
        groups_.tab(i).label = std::string(Model::group_name(kChips[i])) + "  " +
                               group_digits(model.group_size(kChips[i]));

    // One focus on screen: the part that is not in use shows no ring at all.
    groups_.set_focused(zone_ == Zone::groups);
    grid_.set_active(zone_ == Zone::grid);
    // With the focus on the letters the tile they lead to keeps a faint ring.
    grid_.style.card.ring = zone_ != Zone::groups;
    grid_.style.card.glow = zone_ == Zone::grid;
    grid_.style.reduced_motion = reduced;
    groups_.style.reduced_motion = reduced;
    empty_.style.reduced_motion = reduced;

    grid_.update(dt);
    groups_.update(dt);
    empty_.update(dt);
    swap_.update(dt);
    star_.update(dt, 5.0f);

    // ---- what an empty screen says ----
    empty_.action_button = ui::Button::cross;
    if (!model.has_catalog())
    {
        empty_.title = "The channel list could not be loaded";
        empty_.body = model.catalog_error();
        empty_.action = model.refreshing() ? "" : "Try again";
        empty_.set_bounds({420.0f, 300.0f, 1080.0f, 480.0f});
    }
    else
    {
        empty_.set_bounds({420.0f, 560.0f, 1080.0f, 380.0f});
        if (model.filtering())
        {
            empty_.title = "No channels match";
            empty_.body = "Try fewer filters, or check the spelling.";
            empty_.action = "Clear the search";
            empty_.action_button = ui::Button::circle;
        }
        else if (favorites_)
        {
            empty_.title = "No favorites yet";
            empty_.body = "Press Square on any channel to keep it here.";
            empty_.action = "Browse Live TV";
        }
        else
        {
            empty_.title = "Nothing here yet";
            empty_.body = model.group() == Group::recent
                              ? "Channels you watch are listed here."
                              : "This source has no channels of this kind.";
            empty_.action.clear();
        }
    }
}

void BrowseScreen::draw_hero_text(ui::Canvas &canvas, const iptv::ChannelView &channel,
                                  unsigned index, bool favorite, float alpha, float dx) const
{
    if (alpha <= 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    const ui::Fonts &fonts = canvas.fonts;
    const ui::Theme &theme = shared_.theme;
    const bool reduced = shared_.settings.reduced_motion;
    ui::Painter paint(list, fonts, theme, canvas.glass);
    const float x = kMargin + dx;
    const auto rise = [&](int order) { return reduced ? 0.0f : 16.0f * (1.0f - appear(order)); };

    list.push_opacity(alpha);

    // ---- which channel, and what kind ----
    list.push_opacity(appear(0));
    const std::string kicker =
        (favorites_ ? std::string("FAVORITE")
                    : "CHANNEL " + group_digits(shared_.model.number_of(index))) +
        "  \xC2\xB7  " +
        ui::upper(
            readable(face_for(fonts, fonts.semibold, category_of(channel)), category_of(channel)));
    const ui::FontRef &kicker_face = face_for(fonts, fonts.semibold, kicker);
    ui::text(list, kicker_face, kicker_face.font->fit(kicker, 18.0f, kHeroText * 0.8f), x,
             156.0f + rise(0), 18.0f, tone::accent, gfx::Align::left, 4.0f);
    list.pop_opacity();

    // ---- the name: a long one first drops a size, then takes an ellipsis ----
    std::vector<std::string> notes;
    const std::string name = shown_name(fonts, channel, &notes);
    // Montserrat for the name, unless it lacks a letter of it.
    const ui::FontRef &face = title_face(fonts, name);
    list.push_opacity(appear(1));
    const float size = face.measure(name, 76.0f) <= kHeroText ? 76.0f : 58.0f;
    ui::text(list, face, face.font->fit(name, size, kHeroText), x - 3.0f, 234.0f + rise(1), size,
             theme.text);
    list.pop_opacity();

    list.push_opacity(appear(2));
    const auto time = static_cast<std::int64_t>(platform::unix_time());
    const auto *now = shared_.model.guide().now(channel.id, time);
    const auto *next = shared_.model.guide().next(channel.id, time);
    const std::string place = now    ? "Now: " + now->title
                              : next ? "No programme on now"
                                     : place_line(channel);
    const ui::FontRef &place_face = face_for(fonts, fonts.regular, place);
    ui::text(list, place_face, place_face.font->fit(readable(place_face, place), 24.0f, kHeroText),
             x, 280.0f + rise(2), 24.0f, theme.text_muted);
    if (now || next)
    {
        const std::string line = next ? "Next " + programme_time(next->start) + ": " + next->title
                                      : "Next: No programme information";
        const auto &face = face_for(fonts, fonts.regular, line);
        ui::text(list, face, face.font->fit(line, 21, kHeroText), x, 309 + rise(2), 21,
                 theme.text_muted);
    }
    list.pop_opacity();

    // ---- what the record says about the picture, and how the last try went ----
    list.push_opacity(appear(3));
    const float chips_y = 346.0f + rise(3);
    float at = x;
    const std::string picture = resolution_label(channel);
    at += draw_chip(paint, at, chips_y, picture.empty() ? "Auto quality" : picture) + 12.0f;
    const char *codec = codec_label(channel);
    if (codec[0] != '\0')
        at += draw_chip(paint, at, chips_y, codec) + 12.0f;
    for (const std::string &note : notes)
        at += draw_chip(paint, at, chips_y, fonts.semibold.font->fit(note, 19.0f, 220.0f)) + 12.0f;
    if (channel.playback_status == iptv::PlaybackStatus::playable)
        at += draw_status_chip(canvas, theme, at, chips_y, "Opened last time", tone::good) + 12.0f;
    else if (channel.playback_status == iptv::PlaybackStatus::failed)
        at += draw_status_chip(canvas, theme, at, chips_y, "Did not open last time", tone::bad) +
              12.0f;
    list.pop_opacity();

    // ---- what Cross and Square will do ----
    list.push_opacity(appear(4));
    const float cy = 402.0f + rise(4);
    const float width = 16.0f + 40.0f + 14.0f + fonts.semibold.measure("Watch", 27.0f) + 34.0f;
    const Rect pill{x, cy - 34.0f, width, 68.0f};
    list.shadow({pill.x, pill.y + 10.0f, pill.w, pill.h}, 34.0f, 24.0f, Color::rgb(0x000000, 0.4f));
    list.rounded_rect(pill, 34.0f, tone::cream);
    ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::cross, pill.x + 14.0f, cy,
                    40.0f);
    ui::text(list, fonts.semibold, "Watch", pill.x + 70.0f, baseline_for(cy, 27.0f), 27.0f,
             tone::ink);
    const float star_x = pill.x + pill.w + 18.0f + 34.0f;
    list.circle(star_x, cy, 34.0f, kWhite.with_alpha(0.1f));
    list.ring(star_x, cy, 34.0f, 1.5f, kWhite.with_alpha(0.22f));
    list.star(star_x, cy - 1.0f, 15.0f + 8.0f * star_.value, favorite ? tone::accent : tone::cream,
              favorite ? 0.0f : 2.5f);
    ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::right_stick, star_x + 75, cy,
                    34);
    ui::text(list, fonts.regular, "Guide", star_x + 120, baseline_for(cy, 23), 23,
             theme.text_muted);
    list.pop_opacity();

    list.pop_opacity();
}

void BrowseScreen::draw_hero_art(ui::Canvas &canvas, const iptv::ChannelView &channel,
                                 float alpha) const
{
    if (alpha <= 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    const Color accent = art_colors(channel.id).accent;
    list.push_opacity(alpha * appear(1));
    const Rect set = tv_body(kHeroArt);
    list.shadow({set.x, set.y + 26.0f, set.w, set.h}, kHeroRadius, 56.0f,
                Color::rgb(0x000000, 0.5f));
    list.glow(set.inset(-4.0f), kHeroRadius + 4.0f, 80.0f, accent.with_alpha(0.24f));
    const auto preview = shared_.preview.find(channel.id);
    if (preview.id)
    {
        const auto screen = draw_tv_shell(list, kHeroArt, kHeroRadius, art_colors(channel.id));
        list.rounded_rect(screen, screen.h * 0.13f, Color::rgb(0x080808));
        // Keep the complete picture inside the curved screen, without cropping.
        const float scale =
            std::min(screen.w * 0.91f / preview.width, screen.h * 0.91f / preview.height);
        const float w = preview.width * scale, h = preview.height * scale;
        list.image(preview.id, {screen.cx() - w / 2, screen.cy() - h / 2, w, h}, {0, 0, 1, 1},
                   Color::rgb(0xffffff));
    }
    else
        draw_channel_art(list, canvas.fonts, kHeroArt, kHeroRadius, channel,
                         shared_.images.find(channel.tvg_logo));
    list.pop_opacity();
}

void BrowseScreen::draw_list_header(ui::Canvas &canvas) const
{
    gfx::DrawList &list = canvas.list;
    const ui::Fonts &fonts = canvas.fonts;
    const ui::Theme &theme = shared_.theme;
    const Model &model = shared_.model;

    list.push_opacity(appear(5));
    if (favorites_)
    {
        ui::text(list, fonts.semibold, "Your favorites", kMargin, baseline_for(kListY, 27.0f),
                 27.0f, theme.text);
    }
    else
    {
        // Every list wears a hairline; the cream plate of the bar marks the one in use.
        for (int i = 0; i < kChipCount; ++i)
        {
            const Rect chip = groups_.tab_rect(fonts, i);
            list.bordered_rect(chip, chip.h * 0.5f, kWhite.with_alpha(0.05f), 1.5f,
                               kWhite.with_alpha(0.14f));
        }
        groups_.draw(canvas);
    }

    // ---- at the right: where the focus is, and what narrows the list ----
    float right = kWidth - kMargin;
    if (grid_.count() > 0)
    {
        const std::string position = group_digits(static_cast<unsigned>(grid_.focus()) + 1u) +
                                     " of " + group_digits(model.visible_count());
        right -= ui::text(list, fonts.mono, position, right, baseline_for(kListY, 20.0f), 20.0f,
                          theme.text_muted, gfx::Align::right) +
                 26.0f;
    }
    if (model.filtering())
    {
        std::string summary;
        const auto add = [&](const std::string &part)
        {
            if (part.empty())
                return;
            if (!summary.empty())
                summary += "  \xC2\xB7  ";
            summary += part;
        };
        if (!model.query().empty())
            add("\"" + model.query() + "\"");
        add(model.country());
        add(model.provider_category());
        add(model.category());
        add(model.language());
        if (model.quality() != kQualityAny)
            add(quality_filter_name(model.quality()));
        const float room = right - (favorites_ ? 420.0f : 980.0f);
        const std::string shown = fonts.semibold.font->fit(summary, 20.0f, room - 150.0f);
        const float width = fonts.semibold.measure(shown, 20.0f);
        ui::text(list, fonts.semibold, shown, right, baseline_for(kListY, 20.0f), 20.0f, theme.text,
                 gfx::Align::right);
        ui::text(list, fonts.semibold, "SEARCH", right - width - 16.0f, baseline_for(kListY, 16.0f),
                 16.0f, tone::accent, gfx::Align::right, 3.0f);
    }
    list.pop_opacity();
}

void BrowseScreen::draw_waiting(ui::Canvas &canvas) const
{
    // The shape of the screen while the saved catalog is read.
    gfx::DrawList &list = canvas.list;
    const Color bone = kWhite.with_alpha(0.09f);
    list.rounded_rect({kMargin, 140.0f, 300.0f, 18.0f}, 8.0f, bone);
    list.rounded_rect({kMargin, 180.0f, 820.0f, 64.0f}, 16.0f, bone);
    list.rounded_rect({kMargin, 262.0f, 480.0f, 26.0f}, 10.0f, bone);
    list.rounded_rect(kHeroArt, kHeroRadius, bone);
    // Say what the wait is for: the first launch has nothing saved to show.
    if (shared_.model.refreshing())
    {
        const ui::Theme &theme = shared_.theme;
        ui::text(list, canvas.fonts.semibold, "Downloading the channel list", kMargin, 352.0f,
                 28.0f, theme.text);
        const unsigned so_far = shared_.model.refresh_progress() / 1000u * 1000u;
        ui::text(list, canvas.fonts.regular,
                 so_far != 0
                     ? group_digits(so_far) + " channels so far. Later launches open from the " +
                           "copy saved on this console."
                     : std::string("This happens once. Later launches open from the copy saved "
                                   "on this console."),
                 kMargin, 392.0f, 23.0f, theme.text_muted);
    }
    const float w = (kGridWidth - (kColumns - 1) * kGapX) / kColumns;
    for (int i = 0; i < kColumns * kRowsInView; ++i)
        draw_tile_placeholder(
            canvas, shared_,
            {kMargin + static_cast<float>(i % kColumns) * (w + kGapX),
             kGridTop + kGridPad + static_cast<float>(i / kColumns) * (kTileHeight + kGapY), w,
             kTileHeight});
}

void BrowseScreen::draw(ui::Canvas &canvas) const
{
    const Model &model = shared_.model;
    if (!model.has_catalog())
    {
        if (model.catalog_failed())
            empty_.draw(canvas);
        else
            draw_waiting(canvas);
        return;
    }

    const std::optional<iptv::ChannelView> channel = focused();
    if (channel)
    {
        const unsigned index = focused_index();
        const bool favorite = model.is_favorite(*channel);
        if (swap_.running && has_previous_)
        {
            // The old lines leave quickly; the new ones land a beat later.
            const float t = swap_.progress();
            const float distance = shared_.settings.reduced_motion ? 0.0f : 1.0f;
            const float leave = tween::clamp01(t * 3.0f);
            const float arrive = tween::clamp01((t - 0.24f) / 0.76f);
            draw_hero_art(canvas, previous_, 1.0f - tween::smoothstep(leave));
            draw_hero_art(canvas, *channel, tween::smoothstep(arrive));
            draw_hero_text(canvas, previous_, previous_index_, previous_favorite_,
                           1.0f - tween::smoothstep(leave),
                           -travel_ * 36.0f * distance * tween::cubic_in(leave));
            draw_hero_text(canvas, *channel, index, favorite, tween::smoothstep(arrive),
                           travel_ * 48.0f * distance * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_hero_art(canvas, *channel, 1.0f);
            draw_hero_text(canvas, *channel, index, favorite, 1.0f, 0.0f);
        }
    }
    else
    {
        // No channel to speak of: the screen's own name holds the hero's place.
        ui::Painter paint(canvas.list, canvas.fonts, shared_.theme, canvas.glass);
        canvas.list.push_opacity(appear(0));
        ui::text(canvas.list, canvas.fonts.semibold, favorites_ ? "FAVORITES" : "LIVE TV", kMargin,
                 156.0f, 18.0f, tone::accent, gfx::Align::left, 4.0f);
        paint.heading(favorites_ ? "Your favorite channels" : "Live TV", kMargin - 3.0f, 234.0f,
                      76.0f);
        canvas.list.pop_opacity();
    }

    draw_list_header(canvas);
    if (grid_.count() == 0)
    {
        empty_.draw(canvas);
        return;
    }
    grid_.draw(canvas);
    draw_rail(canvas);
}

void BrowseScreen::draw_rail(ui::Canvas &canvas) const
{
    gfx::DrawList &list = canvas.list;
    const ui::Fonts &fonts = canvas.fonts;
    const Model &model = shared_.model;
    const float focus = tween::clamp01(rail_focus_.value);
    const float y = rail_y_.value;

    list.push_opacity(appear(5));
    // The plate of the letter in use: orange while the list has the focus;
    // cream, larger and lit once the letters have it.
    const float radius = 11.5f + 6.5f * focus;
    if (focus > 0.01f)
        list.glow({kRailX - radius, y - radius, 2.0f * radius, 2.0f * radius}, radius, 24.0f,
                  tone::accent.with_alpha(0.7f * focus));
    list.circle(kRailX, y, radius, tone::accent);
    if (focus > 0.01f)
        list.circle(kRailX, y, radius, tone::cream.with_alpha(focus));
    for (int letter = 0; letter < kLetterCount; ++letter)
    {
        const char text[2] = {letter_char(letter), '\0'};
        const float home = kRailTop + static_cast<float>(letter) * kRailPitch;
        // With the focus on them the letters make room around the one in use,
        // which is written larger.
        const float away = (home - y) / kRailPitch;
        const float near = std::abs(away);
        const float push =
            near < 1.0f ? away : std::copysign(std::exp(-(near - 1.0f) * 0.9f), away);
        const float cy = home + focus * 7.0f * push;
        const float size = kRailText + focus * 7.0f * std::max(0.0f, 1.0f - near);
        // Letters with nothing under them stay, dimmed: the column keeps its shape.
        const Color color = near < 0.5f                       ? tone::ink
                            : model.letter_start(letter) >= 0 ? shared_.theme.text.with_alpha(0.88f)
                                                              : kWhite.with_alpha(0.2f);
        ui::text(list, fonts.semibold, text, kRailX, baseline_for(cy, size), size, color,
                 gfx::Align::center);
    }
    list.pop_opacity();
}

int BrowseScreen::hints(ui::Hint *out, int capacity) const
{
    const Model &model = shared_.model;
    int count = 0;
    const auto add = [&](ui::Hint hint)
    {
        if (count < capacity)
            out[count++] = hint;
    };
    if (!model.has_catalog())
    {
        if (model.catalog_failed() && !model.refreshing())
            add({ui::Button::cross, "Try again"});
        return count;
    }
    if (zone_ == Zone::groups)
    {
        add({ui::Button::dpad, "Choose a list"});
    }
    else if (zone_ == Zone::rail)
    {
        add({ui::Button::dpad, "Jump to a letter"});
        add({ui::Button::cross, "Channels"});
    }
    else if (focused())
    {
        add({ui::Button::cross, "Watch"});
        add({ui::Button::square, model.is_favorite(*focused()) ? "Unfavorite" : "Favorite"});
        add({ui::Button::l2, "Page", ui::Button::r2});
    }
    else if (favorites_ && !model.filtering())
    {
        add({ui::Button::cross, "Browse Live TV"});
    }
    add({ui::Button::triangle, "Search"});
    if (model.filtering())
        add({ui::Button::circle, "Clear the search"});
    return count;
}

} // namespace ptv
