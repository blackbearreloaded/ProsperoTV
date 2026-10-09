// ProsperoTV - Saved playlists and accounts, with independent refresh schedules.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/i18n.hpp"
#include "tv/sources_screen.hpp"
#include "tv/draw.hpp"
#include "tv/stream_sniff.hpp"

#include <algorithm>

namespace ptv
{
namespace
{
constexpr Rect kList{kMargin, 250, 1010, 670};
constexpr Rect kPanel{1144, 250, kWidth - kMargin - 1144, 640};

const char *source_title(const SavedSource &source)
{
    static constexpr const char *defaults[] = {"iptv-org public catalog", "Custom playlist",
                                               "Xtream Codes account", "MAC-code portal"};
    if (source.id >= 1 && source.id <= 4 && source.kind == source.id - 1 &&
        source.name == defaults[source.id - 1])
        return tr(defaults[source.id - 1]);
    return source.name.c_str();
}
} // namespace

SourcesScreen::SourcesScreen(Shared &shared) : shared_(shared)
{
    list_.style.theme = shared.theme;
    list_.style.row_height = 110;
    list_.style.gap = 12;
    list_.style.cards = true;
    list_.style.title_size = 28;
    list_.style.entrance_step = 0;
    list_.set_bounds(kList);
    sync();
    list_.set_focus(
        std::clamp(shared.model.view.focused_source, 0, static_cast<int>(ids_.size()) - 1), true);
}

void SourcesScreen::sync()
{
    const int focus = list_.focus();
    const auto &model = shared_.model;
    std::vector<ui::ListItem> rows;
    ids_.clear();
    for (const auto &source : model.saved_sources())
    {
        if (source.kind > 5)
            continue;
        ui::ListItem row;
        row.title = source_title(source);
        // The built-in list has no address of its own and nothing to set up.
        row.subtitle = source.url.empty() && source.kind != 0 ? tr("Press Cross to set up")
                                                              : tr(schedule_name(source.schedule));
        row.badge = source.id == model.selected_source_id() ? tr("In use") : "";
        rows.push_back(std::move(row));
        ids_.push_back(source.id);
    }
    ui::ListItem playlist;
    playlist.title = tr("Add another playlist");
    playlist.subtitle = tr("M3U or M3U8 address");
    rows.push_back(std::move(playlist));
    ids_.push_back(-1);
    ui::ListItem account;
    account.title = tr("Add another Xtream account");
    account.subtitle = tr("Server, user name and password");
    rows.push_back(std::move(account));
    ids_.push_back(-2);
    ui::ListItem portal;
    portal.title = tr("Add another MAC-code portal");
    portal.subtitle = tr("Stalker / Ministra address and MAC code");
    rows.push_back(std::move(portal));
    ids_.push_back(-3);
    ui::ListItem tuner;
    tuner.title = tr("Add an HDHomeRun tuner");
    tuner.subtitle = tr("Tuner address on your home network");
    rows.push_back(std::move(tuner));
    ids_.push_back(-4);
    ui::ListItem server;
    server.title = tr("Add a Tvheadend server");
    server.subtitle = tr("HTTP address and optional account");
    rows.push_back(std::move(server));
    ids_.push_back(-5);
    list_.set_items(std::move(rows));
    list_.set_focus(std::clamp(focus, 0, static_cast<int>(ids_.size()) - 1), true);
    seen_revision_ = model.revision();
}

std::int64_t SourcesScreen::focused_id() const
{
    const int focus = list_.focus();
    return focus >= 0 && static_cast<std::size_t>(focus) < ids_.size() ? ids_[focus] : 1;
}

void SourcesScreen::enter()
{
    sync();
    list_.enter();
}

void SourcesScreen::handle(const InputFrame &input, ui::Feedback &feedback)
{
    auto &model = shared_.model;
    const auto id = focused_id();
    if (input.is_pressed(Action::north) && id > 1)
    {
        model.edit_saved_source(id);
        return;
    }
    if (input.is_pressed(Action::west) && id > 0)
    {
        if (const auto *source = model.saved_source(id))
        {
            const auto next =
                static_cast<RefreshSchedule>((static_cast<int>(source->schedule) + 1) % 3);
            if (!model.set_schedule(id, next))
                model.announce(Level::error, tr("The schedule could not be saved"), "", 3);
            sync();
        }
        return;
    }
    if (list_.handle(input, feedback) == ui::Event::activated)
    {
        if (id < 0)
            model.add_source(static_cast<iptv::SourceKind>(-id));
        else
            model.use_saved_source(id);
    }
}

void SourcesScreen::update(float dt)
{
    if (seen_revision_ != shared_.model.revision())
        sync();
    shared_.model.view.focused_source = list_.focus();
    list_.style.reduced_motion = shared_.settings.reduced_motion;
    list_.update(dt);
}

void SourcesScreen::draw(ui::Canvas &canvas) const
{
    const auto &model = shared_.model;
    const auto &theme = shared_.theme;
    auto &draw = canvas.list;
    const auto &fonts = canvas.fonts;
    ui::text(draw, fonts.semibold, tr("WHERE THE CHANNELS COME FROM"), kMargin, 162, 18,
             tone::accent, gfx::Align::left, 4);
    ui::text(draw, fonts.display, tr("Sources"), kMargin - 3, 224, 60, theme.text);
    list_.draw(canvas);
    draw_glass(canvas, theme, kPanel, 26);
    const float left = kPanel.x + 36, width = kPanel.w - 72;
    const auto id = focused_id();
    const auto *source = model.saved_source(id);
    const std::string title = source != nullptr ? source_title(*source) : tr("Add a source");
    ui::text(draw, fonts.display, fonts.display.font->fit(title, 36, width), left, 315, 36,
             theme.text);
    if (source == nullptr)
    {
        ui::paragraph(
            draw, fonts.regular,
            tr("Keep several playlists and accounts. Each keeps its own saved channels and "
               "refresh schedule. Cross opens the form."),
            left, 375, 26, width, 38, theme.text_muted, 6);
        return;
    }
    const std::string address = source->kind == 0     ? tr("Built in")
                                : source->url.empty() ? tr("Not set up")
                                                      : redact_address(source->url);
    ui::paragraph(draw, fonts.regular, address, left, 375, 24, width, 34, theme.text_muted, 3);
    ui::text(draw, fonts.semibold,
             tr("Refresh: ") + std::string(tr(schedule_name(source->schedule))), left, 510, 26,
             theme.text);
    ui::paragraph(draw, fonts.regular,
                  tr("Square changes the schedule. A saved list opens immediately; a download runs "
                     "only when due or when requested."),
                  left, 563, 24, width, 34, theme.text_muted, 4);
    if (id == model.selected_source_id())
    {
        ui::text(draw, fonts.semibold, group_digits(model.channel_count()) + tr(" channels"), left,
                 735, 28, theme.text);
        ui::paragraph(draw, fonts.regular, model.source_detail(), left, 785, 22, width, 31,
                      theme.text_muted, 3);
    }
    else
        ui::text(draw, fonts.semibold, tr("Cross: Use this source"), left, 765, 26, theme.text);
}

int SourcesScreen::hints(ui::Hint *out, int capacity) const
{
    int count = 0;
    const auto add = [&](ui::Hint hint)
    {
        if (count < capacity)
            out[count++] = hint;
    };
    add({ui::Button::cross, focused_id() < 0 ? tr("Add") : tr("Use source")});
    if (focused_id() > 1)
        add({ui::Button::triangle, tr("Edit")});
    if (focused_id() > 0)
        add({ui::Button::square, tr("Refresh schedule")});
    return count;
}
} // namespace ptv
