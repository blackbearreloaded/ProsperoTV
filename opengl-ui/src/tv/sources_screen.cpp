// ProsperoTV - Saved playlists and accounts, with independent refresh schedules.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/i18n.hpp"
#include "tv/sources_screen.hpp"
#include "tv/usb_playlist.hpp"
#include "tv/draw.hpp"
#include "tv/stream_sniff.hpp"
#include "tv/platform.hpp"

#include <algorithm>
#include <ctime>

namespace ptv
{
namespace
{
constexpr Rect kList{kMargin, 250, 1010, 670};
constexpr Rect kPanel{1144, 250, kWidth - kMargin - 1144, 640};
// Compatibility strings, not a claim that this app runs either client.
constexpr const char *kUserAgents[] = {"", "", "IPTVSmartersPro", "VLC/3.0.21 LibVLC/3.0.21"};

std::string account_date(std::int64_t stamp)
{
    if (stamp < 0)
        return tr("Not supplied");
    if (stamp == 0)
        return tr("No expiry reported");
    char text[32]{};
    std::tm utc{};
    if (platform::calendar_time(stamp, false, utc) &&
        std::strftime(text, sizeof(text), "%Y-%m-%d UTC", &utc))
        return text;
    return tr("Not supplied");
}

// A source with nothing to open yet; the built-in list has no address and needs none.
bool needs_setup(const SavedSource *source)
{
    return source != nullptr && source->url.empty() && source->kind != 0;
}

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
    user_agents_.style = list_.style;
    user_agents_.set_bounds(kList);
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
        row.subtitle = needs_setup(&source) ? tr("Not set up") : tr(schedule_name(source.schedule));
        row.badge = source.id == model.selected_source_id() ? tr("In use") : "";
        if (source.kind == 2)
        {
            const auto days = account_expiry_notice(source.account, platform::unix_time());
            if (days)
                row.badge = days < 0 ? tr("Expired") : tr("Expires soon");
        }
        rows.push_back(std::move(row));
        ids_.push_back(source.id);
    }
    if (model.has_catalog())
    {
        ui::ListItem check;
        check.title = tr(model.checking_source() ? "Cancel source check" : "Check active source");
        check.subtitle = model.source_check_status();
        rows.push_back(std::move(check));
        ids_.push_back(-7);
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
    ui::ListItem usb;
    usb.title = tr("Find playlists on USB");
    usb.subtitle = tr("Top-level .m3u and .m3u8 files on connected drives");
    rows.push_back(std::move(usb));
    ids_.push_back(-6);
    if (!model.checking_source())
        usb_files_ = usb_playlists(model.usb_root());
    for (std::size_t i = 0; i < usb_files_.size(); ++i)
    {
        ui::ListItem row;
        row.title = tr("Import ") + usb_files_[i].substr(10);
        row.subtitle = usb_files_[i];
        rows.push_back(std::move(row));
        ids_.push_back(-100 - static_cast<std::int64_t>(i));
    }
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
    http_source_ = 0;
    sync();
    list_.enter();
}

void SourcesScreen::handle(const InputFrame &input, ui::Feedback &feedback)
{
    auto &model = shared_.model;
    if (http_source_)
    {
        if (input.is_pressed(Action::back))
        {
            http_source_ = 0;
            feedback.play(audio::Cue::back);
        }
        else if (user_agents_.handle(input, feedback) == ui::Event::activated)
        {
            const int choice = user_agents_.focus();
            if (choice == 0)
            {
                model.edit_source_http(http_source_);
                http_source_ = 0;
            }
            else if (choice > 0 && choice < 4 &&
                     model.set_source_user_agent(http_source_, kUserAgents[choice]))
                http_source_ = 0;
        }
        return;
    }
    const auto id = focused_id();
    if (input.is_pressed(Action::jump_next) && id > 1)
    {
        if (!model.require_parent() || model.refreshing())
            return;
        const auto *source = model.saved_source(id);
        if (!source || source->kind == 0)
            return;
        http_source_ = id;
        std::vector<ui::ListItem> choices;
        const char *titles[] = {tr("Custom user agent"), tr("Default user agent"),
                                "IPTVSmartersPro", "VLC"};
        for (unsigned i = 0; i < 4; ++i)
        {
            ui::ListItem row;
            row.title = titles[i];
            row.subtitle = i > 1 ? kUserAgents[i] : i == 1 ? tr("Leave empty for the default") : "";
            row.disabled = i == 0 && !model.keyboard_ready();
            if (i && source->user_agent == kUserAgents[i])
                row.badge = tr("In use");
            choices.push_back(std::move(row));
        }
        user_agents_.set_items(std::move(choices));
        user_agents_.set_focus(model.keyboard_ready() ? 0 : 1, true);
        user_agents_.enter();
        return;
    }
    if (input.is_pressed(Action::jump_prev) && id > 0)
    {
        model.edit_guide_url(id);
        return;
    }
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
        if (id == -7)
        {
            shared_.preview.clear();
            model.toggle_source_check();
        }
        else if (id == -6)
        {
            usb_files_ = usb_playlists(model.usb_root());
            sync();
            model.announce(Level::ready,
                           usb_files_.empty() ? tr("No USB playlists found")
                                              : tr("Choose a playlist below"),
                           tr("Use the root of a USB drive, with .m3u or .m3u8 files."), 4);
        }
        else if (id <= -100)
        {
            const auto index = static_cast<std::size_t>(-100 - id);
            if (index < usb_files_.size())
                model.import_usb_playlist(usb_files_[index]);
        }
        else if (id < 0)
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
    user_agents_.style.reduced_motion = shared_.settings.reduced_motion;
    user_agents_.update(dt);
}

void SourcesScreen::draw(ui::Canvas &canvas) const
{
    const auto &model = shared_.model;
    const auto &theme = shared_.theme;
    auto &draw = canvas.list;
    const auto &fonts = canvas.fonts;
    if (http_source_)
    {
        ui::text(draw, fonts.display, tr("HTTP settings"), kMargin, 224, 60, theme.text);
        user_agents_.draw(canvas);
        const auto focus = user_agents_.row_rect(user_agents_.focus());
        draw.push_clip(kList);
        draw.rounded_rect({focus.x + 7, focus.y + 14, 5, focus.h - 28}, 2, tone::accent);
        draw.pop_clip();
        draw_glass(canvas, theme, kPanel, 26);
        ui::paragraph(
            draw, fonts.regular,
            tr("Presets change only the user agent. Custom entry also edits other HTTP settings."),
            kPanel.x + 36, 330, 26, kPanel.w - 72, 38, theme.text_muted, 8);
        return;
    }
    ui::text(draw, fonts.semibold, tr("WHERE THE CHANNELS COME FROM"), kMargin, 162, 18,
             tone::accent, gfx::Align::left, 4);
    ui::text(draw, fonts.display, tr("Sources"), kMargin - 3, 224, 60, theme.text);
    list_.draw(canvas);
    const auto focused = list_.row_rect(list_.focus());
    draw.push_clip(kList);
    draw.rounded_rect({focused.x + 7, focused.y + 14, 5, focused.h - 28}, 2, tone::accent);
    draw.pop_clip();
    draw_glass(canvas, theme, kPanel, 26);
    const float left = kPanel.x + 36, width = kPanel.w - 72;
    const auto id = focused_id();
    const auto *source = model.saved_source(id);
    const std::string title = id == -7            ? tr("Check active source")
                              : source != nullptr ? source_title(*source)
                                                  : tr("Add a source");
    ui::text(draw, fonts.display, fonts.display.font->fit(title, 36, width), left, 315, 36,
             theme.text);
    if (id == -7)
    {
        ui::paragraph(
            draw, fonts.regular,
            tr("Checks one visible live channel at a time. A decoded picture passes; only missing "
               "channel addresses fail. Unsupported formats and timeouts are skipped. Playback, "
               "previews, recording or source changes stop the check."),
            left, 375, 26, width, 38, theme.text_muted, 12);
        ui::paragraph(draw, fonts.regular, model.source_check_status(), left, 700, 24, width, 34,
                      theme.text, 5);
        return;
    }
    if (source == nullptr && (id == -6 || id <= -100))
    {
        ui::paragraph(draw, fonts.regular,
                      tr("Import a playlist from the root of /mnt/usb0 through /mnt/usb7. "
                         "Channels are saved in the app. Updates reread the same USB path; "
                         "keep that drive connected when refreshing. Missing drives leave the "
                         "last saved channels available. New USB sources use manual refresh."),
                      left, 375, 26, width, 38, theme.text_muted, 12);
        return;
    }
    if (source == nullptr)
    {
        ui::paragraph(
            draw, fonts.regular,
            tr("Keep several playlists and accounts. Each keeps its own saved channels and "
               "refresh schedule."),
            left, 375, 26, width, 38, theme.text_muted, 6);
        return;
    }
    const std::string address = source->kind == 0     ? tr("Built in")
                                : source->url.empty() ? tr("Not set up")
                                                      : redact_address(source->url);
    ui::paragraph(draw, fonts.regular, address, left, 375, 24, width, 34, theme.text_muted, 3);
    const auto guide_url = model.guide_url_override(id);
    if (!guide_url.empty())
        ui::text(draw, fonts.regular,
                 fonts.regular.font->fit(tr("XMLTV: ") + redact_address(guide_url), 20, width),
                 left, 488, 20, theme.text_muted);
    ui::text(draw, fonts.semibold,
             tr("Refresh: ") + std::string(tr(schedule_name(source->schedule))), left, 510, 26,
             theme.text);
    if (source->kind == 2)
    {
        const auto &info = source->account;
        const auto line = [&](const std::string &text, float y)
        {
            ui::text(draw, fonts.regular, fonts.regular.font->fit(text, 22, width), left, y, 22,
                     theme.text_muted);
        };
        line(tr("Expires: ") + account_date(info.expires_unix), 563);
        line(tr("Connection limit: ") + (info.max_connections < 0 ? std::string(tr("Not supplied"))
                                         : info.max_connections == 0
                                             ? std::string(tr("No limit reported"))
                                             : std::to_string(info.max_connections)),
             607);
        const int days = account_expiry_notice(info, platform::unix_time());
        if (days)
            line(days < 0 ? tr("Account expiry date has passed")
                          : tr("Account expires within a week"),
                 651);
        else if (!info.checked_unix)
            line(tr("Details are checked on the next refresh."), 651);
        if (info.checked_unix)
            line(tr("Last checked: ") + account_date(info.checked_unix), 695);
    }
    else
    {
        ui::paragraph(draw, fonts.regular,
                      tr("A saved list opens immediately; a download runs only when due or when "
                         "requested."),
                      left, 563, 24, width, 34, theme.text_muted, 4);
    }
    if (id == model.selected_source_id())
    {
        const auto &counts = model.counts();
        float y = 724;
        const auto count = [&](const std::string &text)
        {
            ui::text(draw, fonts.semibold, text, left, y, 26, theme.text);
            y += 32;
        };
        count(group_digits(counts.channels) + tr(" channels"));
        if (counts.movies)
            count(group_digits(*counts.movies) + tr(" movies"));
        if (counts.shows)
            count(group_digits(*counts.shows) + tr(" TV shows"));
        ui::paragraph(draw, fonts.regular, model.source_detail(), left, y + 10, 22, width, 31,
                      theme.text_muted, 2);
    }
    else
    {
        const ui::Hint hints[] = {
            {ui::Button::cross, needs_setup(source) ? tr("Set up") : tr("Use source")}};
        ui::HintLayout layout;
        layout.size = 36.0f;
        layout.text_size = 26.0f;
        layout.font = &fonts.semibold;
        layout.cy = 756.0f;
        draw_hint_row(draw, fonts, hints, 1, left, width, layout);
    }
}

int SourcesScreen::hints(ui::Hint *out, int capacity) const
{
    int count = 0;
    const auto add = [&](ui::Hint hint)
    {
        if (count < capacity)
            out[count++] = hint;
    };
    if (http_source_)
    {
        add({ui::Button::cross, tr("Edit")});
        add({ui::Button::circle, tr("Back")});
        return count;
    }
    add({ui::Button::cross,
         focused_id() == -7
             ? tr(shared_.model.checking_source() ? "Cancel source check" : "Check active source")
         : focused_id() < 0                                      ? tr("Add")
         : needs_setup(shared_.model.saved_source(focused_id())) ? tr("Set up")
                                                                 : tr("Use source")});
    if (focused_id() > 1)
        add({ui::Button::triangle, tr("Edit")});
    if (focused_id() > 0)
    {
        add({ui::Button::square, tr("Refresh schedule")});
        add({ui::Button::l2, tr("XMLTV guide")});
    }
    if (focused_id() > 1)
        add({ui::Button::r2, tr("HTTP settings")});
    return count;
}
} // namespace ptv
