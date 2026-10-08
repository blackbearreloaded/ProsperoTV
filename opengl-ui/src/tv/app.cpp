// ProsperoTV - The interface: tabs, the screens, and everything that floats.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/app.hpp"

#include "tv/diag.hpp"
#include "tv/draw.hpp"
#include "iptv_ime.h"
#include "ui/components/overlay.hpp"
#include "qrcodegen.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ptv
{

namespace
{

constexpr Rect kSettingsPanel{kMargin, 250.0f, 1010.0f, 640.0f};
constexpr Rect kGlancePanel{1144.0f, 250.0f, kWidth - kMargin - 1144.0f, 640.0f};
// The opening: the set stands in the middle; its moments, in seconds.
constexpr Rect kIntroSet{(kWidth - 800.0f) * 0.5f, 250.0f, 800.0f, 520.0f};
constexpr float kIntroArrive = 0.55f; // the set comes out of the dark
constexpr float kIntroLine = 0.90f;   // a line of light across its screen
constexpr float kIntroOpen = 1.30f;   // the line opens into a picture
constexpr float kIntroHold = 2.25f;   // the app's mark on the screen
constexpr float kIntroEnd = 3.20f;    // through the glass, into the app
// The tuning screen: the channel's picture in the middle, its bar below.
constexpr Rect kTuningArt{(kWidth - 640.0f) * 0.5f, 214.0f, 640.0f, 360.0f};
constexpr Rect kTuningBar{(kWidth - 560.0f) * 0.5f, 858.0f, 560.0f, 8.0f};
constexpr float kTuningStart = 0.06f;
// About: the credits at the left, how channels get here at the right.
constexpr Rect kCreditsPanel{kMargin, 250.0f, 846.0f, 690.0f};
constexpr Rect kGuidePanel{kMargin + 870.0f, 250.0f, kWidth - 2.0f * kMargin - 870.0f, 690.0f};

enum FormRow : int
{
    kRowMotion = 1,
    kRowSounds,
    kRowResolution,
    kRowChannels,
    kRowUpdate,
    kRowVolume,
    kRowPair,
    kRowForgetPhones,
    kRowPhones,
    kRowDiagnostics,
};

constexpr const char *kTabNames[] = {"Live TV", "Favorites", "Sources", "Settings", "About"};
constexpr const char *kActionNames[] = {"Up",       "Down", "Left", "Right", "Cross", "Circle",
                                        "Triangle", "Square", "L1",  "R1",    "L2",    "R2",
                                        "Options",  "Touchpad", "L3", "R3"};
constexpr const char *kDirectionNames[] = {"none", "up", "down", "left", "right"};

ui::StatusKind toast_kind(Level level)
{
    switch (level)
    {
    case Level::warning:
        return ui::StatusKind::warning;
    case Level::error:
        return ui::StatusKind::danger;
    case Level::busy:
        return ui::StatusKind::info;
    case Level::ready:
        break;
    }
    return ui::StatusKind::success;
}

const char *source_label(iptv::SourceKind source)
{
    switch (source)
    {
    case iptv::SourceKind::Custom:
        return "Your playlist";
    case iptv::SourceKind::Xtream:
        return "Your account";
    case iptv::SourceKind::BuiltIn:
        break;
    }
    return "iptv-org public list";
}

} // namespace

App::App(Model &model, const ui::Fonts &fonts, std::uint32_t glass_texture,
         const Settings &settings, std::string version)
    : shared_(model, fonts, settings), browse_(shared_), sources_(shared_), search_(shared_),
      update_(shared_), glass_texture_(glass_texture), version_(std::move(version))
{
    const ui::Theme &theme = shared_.theme;

    shared_.toasts.style.theme = theme;
    shared_.toasts.style.frosted = true;
    // Bottom left, on the line of the hints (which end at the right): the one
    // place no screen keeps anything a notice would hide.
    shared_.toasts.style.anchor = ui::ToastAnchor::bottom_left;
    shared_.toasts.style.width = 470.0f;
    shared_.toasts.style.margin = 0.0f;
    shared_.toasts.style.max_visible = 1;
    shared_.toasts.style.duration = 3.0f;
    shared_.toasts.set_bounds({kMargin, 0.0f, kWidth - 2.0f * kMargin, 1052.0f});

    // Announcements float at the top right, over the status chip, and stay
    // long enough to be read from the sofa.
    announcements_.style.theme = theme;
    announcements_.style.frosted = true;
    announcements_.style.anchor = ui::ToastAnchor::top_right;
    announcements_.style.width = 520.0f;
    announcements_.style.margin = 0.0f;
    announcements_.style.max_visible = 1;
    announcements_.style.duration = 10.0f;
    announcements_.set_bounds({kMargin, 34.0f, kWidth - 2.0f * kMargin, 600.0f});

    tabs_.style.theme = theme;
    tabs_.style.kind = ui::TabKind::underline;
    tabs_.style.text_size = 26.0f;
    tabs_.style.padding = 14.0f;
    tabs_.style.gap = 22.0f;
    tabs_.style.track = false;
    tabs_.style.focus_ring = false;
    tabs_.style.on_page = true;
    tabs_.set_tabs({{"Live TV"}, {"Favorites"}, {"Sources"}, {"Settings"}, {"About"}});
    tabs_.set_bounds({486.0f, kHeaderY - 28.0f, 900.0f, 56.0f});
    tabs_.set_focused(false);
    tabs_.set_active(std::clamp(model.view.tab, 0, kTabCount - 1), true);

    form_.style.theme = theme;
    form_.style.on_page = false;
    form_.style.label_ratio = 0.56f;
    form_.style.control_width = 380.0f;
    form_.add_header("Interface");
    form_.add_toggle(kRowMotion, "Reduce motion", settings.reduced_motion).description =
        "Screens fade instead of sliding, and the sky stands still.";
    form_.add_toggle(kRowSounds, "Interface sounds", settings.sounds);
    form_.add_slider(kRowVolume, "Volume", settings.volume, 0, 100, 5).unit = "%";
    form_
        .add_choice(kRowResolution, "Menu sharpness", {"Best for this TV", "1080p"},
                    settings.resolution)
        .description = "Takes effect the next time the menu opens. Video keeps its own size.";
    form_.add_header("Phone remote");
    form_.add_action(kRowPair, "Pair a phone").chevron = true;
    form_.add_value(kRowPhones, "Remembered phones", "0");
    form_.add_action(kRowForgetPhones, "Forget paired phones");
    form_.add_header("Channel list");
    form_.add_value(kRowChannels, "Channels", "");
    form_.add_action(kRowUpdate, "Download it again now");
    form_.add_header("Troubleshooting");
    form_.add_toggle(kRowDiagnostics, "Diagnostic log", settings.diagnostics).description =
        "Records what the app does in logs/debug-trace.txt, to send with a report.";
    form_.set_bounds(kSettingsPanel.inset(22.0f));
    // The viewer's switch; a debug build or a scripted run keeps its own.
    diag::set_enabled(settings.diagnostics);
    form_.focus_row(kRowMotion);

    failure_.style.theme = theme;
    failure_.style.width = 860.0f;
    failure_.style.body_lines = 5;
    failure_.style.scrim_color = tone::night;

    lean_.snap(tone::ember);
    lean_dark_.snap(tone::wine);
    tab_changed();
}

bool App::take_settings_changed()
{
    return std::exchange(settings_changed_, false);
}

void App::set_volume(int volume)
{
    shared_.settings.volume = std::clamp(volume, 0, 100);
    form_.set_slider(kRowVolume, static_cast<float>(shared_.settings.volume));
}

void App::remote_notice(const char *message)
{
    diag::event("notice \"%s\"", message != nullptr ? message : "");
    shared_.toasts.push(ui::StatusKind::info, message);
}

void App::phone_connected()
{
    diag::event("phone connected (pairing screen %s)", pairing_open_ ? "open" : "closed");
    if (std::exchange(pairing_open_, false))
        remote_notice("Phone connected");
}

void App::set_pairing_info(std::string url, std::string code, unsigned seconds, unsigned phones)
{
    if (pair_url_ != url)
    {
        pair_url_ = std::move(url);
        pair_qr_.clear();
        pair_qr_size_ = 0;
        uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(5)];
        uint8_t temporary[sizeof(qr)];
        if (!pair_url_.empty() && qrcodegen_encodeText(pair_url_.c_str(), temporary, qr,
                qrcodegen_Ecc_MEDIUM, 1, 5, qrcodegen_Mask_AUTO, true))
        {
            pair_qr_size_ = qrcodegen_getSize(qr);
            for (int y = 0; y < pair_qr_size_; ++y)
                for (int x = 0; x < pair_qr_size_; ++x)
                    pair_qr_.push_back(qrcodegen_getModule(qr, x, y));
        }
    }
    pair_code_ = std::move(code);
    pair_seconds_ = seconds;
    form_.set_value_text(kRowPhones, std::to_string(phones));
    form_.set_disabled(kRowForgetPhones, phones == 0);
}

void App::draw_pairing(ui::Canvas &canvas) const
{
    auto &list = canvas.list;
    const auto &fonts = canvas.fonts;
    const auto &theme = shared_.theme;
    list.rounded_rect({0, 0, kWidth, kHeight}, 0, tone::night.with_alpha(0.88f));
    draw_glass(canvas, theme, {300, 170, 1320, 730}, 28);
    ui::text(list, fonts.display, "Pair a phone", 380, 255, 54, theme.text);
    ui::text(list, fonts.regular, "Use the same Wi-Fi as your PS5.", 380, 305, 26, theme.text_muted);
    if (pair_qr_size_ > 0)
    {
        const float cell = std::floor(380.0f / (pair_qr_size_ + 8));
        const float size = cell * (pair_qr_size_ + 8);
        const float left = 380.0f, top = 350.0f;
        list.rounded_rect({left, top, size, size}, 0, Color::rgb(0xffffff));
        for (int y = 0; y < pair_qr_size_; ++y)
            for (int x = 0; x < pair_qr_size_; ++x)
                if (pair_qr_[static_cast<size_t>(y * pair_qr_size_ + x)])
                    list.rounded_rect({left + (x + 4) * cell, top + (y + 4) * cell, cell, cell},
                                      0, Color::rgb(0x000000));
    }
    ui::text(list, fonts.semibold, "1. Scan to open the remote", 825, 390, 30, theme.text);
    ui::text(list, fonts.regular, pair_url_.empty() ? "Remote unavailable" : pair_url_,
             825, 442, 28, theme.text_muted);
    ui::text(list, fonts.semibold, "2. Enter this code on your phone", 825, 515, 30, theme.text);
    if (!pair_code_.empty())
    {
        ui::text(list, fonts.mono, pair_code_, 825, 620, 78, tone::accent);
        ui::text(list, fonts.regular, "Expires in " + std::to_string(pair_seconds_) + " seconds",
                 825, 680, 26, theme.text_muted);
    }
    else
    {
        ui::text(list, fonts.semibold, "Code expired or unavailable", 825, 602, 28, tone::accent);
        ui::text(list, fonts.regular, "Press X for a new code", 825, 652, 26, theme.text_muted);
    }
    ui::text(list, fonts.regular, "Your browser reconnects automatically on future visits.",
             380, 800, 26, theme.text_muted);
    ui::text(list, fonts.semibold, "Circle: Close", 1380, 850, 24, theme.text_muted);
}

void App::show_tab(int index, bool glide)
{
    if (index == tabs_.active())
        return;
    tabs_.set_active(index, !glide);
    tab_changed();
}

void App::tab_changed()
{
    shared_.model.view.tab = tabs_.active();
    diag::event("tab %s", kTabNames[std::clamp(tabs_.active(), 0, kTabCount - 1)]);
    page_age_ = 0.0f;
    switch (tabs_.active())
    {
    case kLive:
        browse_.show(false);
        break;
    case kFavorites:
        browse_.show(true);
        break;
    case kSources:
        sources_.enter();
        break;
    case kSettings:
        form_.enter();
        break;
    default:
        break;
    }
}

void App::refresh(ui::Feedback &feedback)
{
    Model &model = shared_.model;
    if (model.refreshing())
    {
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        shared_.toasts.push(ui::StatusKind::info, "The channel list is already being updated");
        return;
    }
    feedback.play(audio::Cue::select);
    model.refresh();
    if (model.refreshing())
        shared_.toasts.push(ui::StatusKind::info, "Updating the channel list",
                            "You can keep browsing while it downloads.");
}

void App::open_failure(ui::Feedback &feedback)
{
    const PlaybackFailure *failure = shared_.model.failure();
    if (failure == nullptr)
        return;
    iptv::Channel named;
    named.name = failure->channel_name;
    ui::DialogContent content;
    content.icon = ui::StatusKind::danger;
    content.title = "Couldn't open " + shown_name(shared_.fonts, named);
    content.body = failure->reason;
    diag::event("failure dialog: \"%s\" attempts=%u retry=%d reason=\"%s\"",
                failure->channel_name.c_str(), failure->attempts, failure->can_retry ? 1 : 0,
                failure->reason.c_str());
    if (failure->attempts > 1)
        content.body +=
            "\nAll " + group_digits(failure->attempts) + " of its addresses were tried.";
    if (failure->can_retry)
        content.buttons = {{"Try again", ui::ButtonKind::primary}, {"Back to the channels"}};
    else
        content.buttons = {{"Back to the channels", ui::ButtonKind::primary}};
    failure_.open(std::move(content), feedback);
}

void App::apply_settings()
{
    Settings next = shared_.settings;
    next.reduced_motion = form_.toggle_value(kRowMotion);
    next.sounds = form_.toggle_value(kRowSounds);
    next.volume = static_cast<int>(form_.slider_value(kRowVolume));
    next.resolution = form_.choice_index(kRowResolution) == Settings::kFullHd ? Settings::kFullHd
                                                                              : Settings::kBest;
    next.diagnostics = form_.toggle_value(kRowDiagnostics);
    if (next == shared_.settings)
        return;
    // Said before it goes quiet and after it starts, so both ends are in the log.
    if (shared_.settings.diagnostics && !next.diagnostics)
        diag::event("diagnostic log turned off in Settings");
    diag::set_enabled(next.diagnostics);
    if (!shared_.settings.diagnostics && next.diagnostics)
        diag::event("diagnostic log turned on in Settings");
    shared_.settings = next;
    settings_changed_ = true;
    diag::event("settings: reduce motion=%d sounds=%d volume=%d menu sharpness=%d diagnostics=%d",
                next.reduced_motion ? 1 : 0, next.sounds ? 1 : 0, next.volume, next.resolution,
                next.diagnostics ? 1 : 0);
}

void App::handle_screen(const InputFrame &input, ui::Feedback &feedback)
{
    Model &model = shared_.model;
    const int turn = input.is_pressed(Action::page_next)   ? 1
                     : input.is_pressed(Action::page_prev) ? -1
                                                           : 0;
    if (turn != 0)
    {
        if (tabs_.step(turn, input, feedback) == ui::Event::changed)
            tab_changed();
        return;
    }
    if (input.is_pressed(Action::menu))
    {
        refresh(feedback);
        return;
    }
    if (input.is_pressed(Action::back))
    {
        // Back undoes the nearest thing: a search, then a place in the
        // screen, then the tab.
        if (browsing() && model.filtering())
        {
            model.clear_filters();
            feedback.play(audio::Cue::back);
            shared_.toasts.push(ui::StatusKind::info, "Search cleared");
        }
        else if (browsing() && browse_.back(feedback))
        {
        }
        else if (tabs_.active() != kLive)
        {
            feedback.play(audio::Cue::back);
            show_tab(kLive, true);
        }
        return;
    }

    switch (tabs_.active())
    {
    case kLive:
    case kFavorites:
        switch (browse_.handle(input, feedback))
        {
        case BrowseScreen::Result::search:
            search_.open(feedback);
            break;
        case BrowseScreen::Result::go_live:
            show_tab(kLive, true);
            break;
        default:
            break;
        }
        break;
    case kSources:
        sources_.handle(input, feedback);
        break;
    case kSettings:
    {
        const ui::Event event = form_.handle(input, feedback);
        if (event == ui::Event::changed)
            apply_settings();
        else if (event == ui::Event::activated && form_.changed_id() == kRowUpdate)
            refresh(feedback);
        else if (event == ui::Event::activated && form_.changed_id() == kRowPair)
        {
            pairing_open_ = pair_requested_ = true;
            diag::event("pairing screen opened");
        }
        else if (event == ui::Event::activated && form_.changed_id() == kRowForgetPhones)
            forget_requested_ = true;
        break;
    }
    default:
        break; // About is read, not operated
    }
}

void App::follow_channel(float dt)
{
    const std::optional<iptv::ChannelView> channel =
        browsing() ? browse_.focused() : std::nullopt;
    if (channel)
    {
        const ArtColors colors = art_colors(channel->id);
        lean_.target(colors.accent);
        lean_dark_.target(colors.top);
    }
    lean_amount_.target = channel ? 0.15f : 0.0f;
    lean_.update(dt, 3.5f);
    lean_dark_.update(dt, 3.5f);
    lean_amount_.update(dt, 3.5f);
    if (!shared_.settings.reduced_motion)
        drift_ += dt;
}

void App::play_intro()
{
    intro_ = shared_.settings.reduced_motion ? -1.0f : 0.0f;
}

bool App::accepts_remote_search() const
{
    return browsing() && !update_.is_open() && !failure_.is_open();
}

bool App::remote_search(const char *query)
{
    if (!accepts_remote_search() || query == nullptr)
        return false;
    iptv_ime_cancel();
    shared_.model.set_query(query);
    search_.dismiss();
    intro_ = -1.0f;
    return true;
}

void App::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    if (intro_ < 0.0f)
    {
        step(input, dt, feedback);
        return;
    }
    // The opening plays over the app, which keeps loading under it and is
    // not handed the controller. A button goes straight to its last moment.
    intro_ += dt;
    if (input.pressed != 0 && intro_ < kIntroHold)
        intro_ = kIntroHold;
    if (intro_ >= kIntroEnd)
        intro_ = -1.0f;
    InputFrame none;
    step(none, dt, feedback);
}

void App::step(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    Model &model = shared_.model;
    if (diag::enabled() && (input.pressed != 0 || (input.nav != Direction::none && !input.nav_repeat)))
    {
        // What the viewer did, and where the interface was when they did it.
        std::string buttons;
        for (unsigned bit = 0; bit < std::size(kActionNames); ++bit)
            if ((input.pressed & (1u << bit)) != 0)
                buttons += std::string(buttons.empty() ? "" : "+") + kActionNames[bit];
        const unsigned direction = static_cast<unsigned>(input.nav);
        diag::event("input %s%s%s on %s%s%s%s%s", buttons.c_str(),
                    !buttons.empty() && input.nav != Direction::none ? " " : "",
                    input.nav != Direction::none && direction < std::size(kDirectionNames)
                        ? kDirectionNames[direction]
                        : "",
                    kTabNames[std::clamp(tabs_.active(), 0, kTabCount - 1)],
                    search_.is_open() ? " (search open)" : "", failure_.is_open() ? " (failure dialog)" : "",
                    update_.stage() != UpdateSheet::Stage::closed ? " (update dialog)" : "",
                    pairing_open_ ? " (pairing screen)" : "");
    }
    const bool reduced = shared_.settings.reduced_motion;
    shared_.clock += dt;
    page_age_ += dt;

    if (search_.is_open() &&
        (input.is_pressed(Action::back) || input.is_pressed(Action::north)))
        iptv_ime_cancel();
    model.poll();
    for (Notice &notice : model.take_notices())
    {
        diag::event("notice level=%d \"%s\" | %s", static_cast<int>(notice.level),
                    notice.title.c_str(), notice.body.c_str());
        // A notice with a time of its own is an announcement.
        ui::ToastStack &stack = notice.seconds > 0.0f ? announcements_ : shared_.toasts;
        stack.push(toast_kind(notice.level), std::move(notice.title), std::move(notice.body),
                   notice.seconds);
    }

    // A newer version: offered when the app can install it itself, otherwise
    // only said.
    platform::UpdateOffer offer;
    if (platform::update_take(&offer))
    {
        diag::event("update offered: %s (installed %s, available %s, %llu bytes, installable=%d, "
                    "notes %zu bytes)",
                    offer.version.c_str(), offer.installed.c_str(), offer.available.c_str(),
                    static_cast<unsigned long long>(offer.size), offer.installable ? 1 : 0,
                    offer.notes.size());
        if (offer.installable)
        {
            search_.dismiss();
            update_.open(std::move(offer), feedback);
        }
        else
        {
            announcements_.push(ui::StatusKind::info,
                                "ProsperoTV " + offer.version + " is available",
                                "Get it from homebrew.page.", 10.0f);
        }
    }

    // A channel that would not open is the first thing said when the menu
    // comes back.
    if (model.failure() != nullptr && !failure_seen_)
    {
        failure_seen_ = true;
        search_.dismiss();
        open_failure(feedback);
    }

    // ---- input goes to whatever is on top ----
    if (pairing_open_)
    {
        if (input.is_pressed(Action::back))
            pairing_open_ = false;
        else if (input.is_pressed(Action::confirm) && pair_seconds_ == 0)
        {
            pair_requested_ = true;
        }
    }
    else if (update_.is_open())
    {
        update_.handle(input, feedback);
    }
    else if (failure_.is_open())
    {
        const ui::Event event = failure_.handle(input, feedback);
        if (event == ui::Event::activated || event == ui::Event::cancelled)
        {
            const bool retry = event == ui::Event::activated && failure_.choice() == 0 &&
                               model.failure() != nullptr && model.failure()->can_retry;
            if (retry)
                model.retry_failure();
            else
                model.dismiss_failure();
            failure_seen_ = false;
        }
    }
    else if (search_.is_open())
    {
        search_.handle(input, feedback);
    }
    else
    {
        handle_screen(input, feedback);
    }

    // ---- everything moves every frame ----
    tabs_.style.reduced_motion = reduced;
    form_.style.reduced_motion = reduced;
    failure_.style.reduced_motion = reduced;
    shared_.toasts.style.reduced_motion = reduced;
    announcements_.style.reduced_motion = reduced;

    form_.set_value_text(kRowChannels,
                         model.has_catalog() ? group_digits(model.channel_count()) : "None yet");
    follow_channel(dt);

    tabs_.update(dt);
    browse_.update(dt);
    sources_.update(dt);
    search_.update(dt);
    form_.update(dt);
    failure_.update(dt);
    update_.update(dt, feedback);
    shared_.toasts.update(dt, feedback);
    announcements_.update(dt, feedback);

    if (!shared_.settings.sounds)
        feedback.cues.clear();
}

void App::draw_status(ui::Canvas &canvas) const
{
    gfx::DrawList &list = canvas.list;
    const ui::Fonts &fonts = canvas.fonts;
    const ui::Theme &theme = shared_.theme;
    const Model &model = shared_.model;

    std::string text;
    Color dot = tone::good;
    switch (model.level())
    {
    case Level::busy:
        text = model.has_catalog() ? "Updating" : "Downloading";
        // A large list takes a while: say how far it is, in thousands.
        if (const unsigned so_far = model.refresh_progress() / 1000u * 1000u; so_far != 0)
            text += "  " + group_digits(so_far);
        dot = tone::wait;
        break;
    case Level::warning:
        text = model.status();
        dot = tone::wait;
        break;
    case Level::error:
        text = model.status();
        dot = tone::bad;
        break;
    case Level::ready:
        text = group_digits(model.channel_count()) + " channels";
        break;
    }
    const bool busy = model.refreshing();
    const bool reduced = shared_.settings.reduced_motion;
    const float width = fonts.semibold.measure(text, 19.0f) + 54.0f;
    const Rect chip{kWidth - kMargin - width, kHeaderY - 20.0f, width, 40.0f};
    list.rounded_rect(chip, 20.0f, kWhite.with_alpha(0.09f));
    list.bordered_rect(chip, 20.0f, kClear, 1.5f, kWhite.with_alpha(0.16f));
    if (busy)
    {
        // An arc turns where the dot would be.
        list.ring(chip.x + 21.0f, kHeaderY, 9.0f, 3.0f, kWhite.with_alpha(0.14f));
        list.arc(chip.x + 21.0f, kHeaderY, 9.0f, 3.0f, reduced ? 0.0f : shared_.clock * 4.6f, 2.2f,
                 tone::wait);
    }
    else
    {
        list.glow({chip.x + 16.0f, kHeaderY - 5.0f, 10.0f, 10.0f}, 5.0f, 8.0f,
                  dot.with_alpha(0.5f));
        list.circle(chip.x + 21.0f, kHeaderY, 5.0f, dot);
    }
    ui::text(list, fonts.semibold, text, chip.x + 36.0f, baseline_for(kHeaderY, 19.0f), 19.0f,
             theme.text);
}

void App::draw_header(ui::Canvas &canvas) const
{
    gfx::DrawList &list = canvas.list;
    const ui::Fonts &fonts = canvas.fonts;
    draw_mark(list, kMargin + 22.0f, kHeaderY, 42.0f);
    ui::text(list, fonts.display, "ProsperoTV", kMargin + 58.0f, baseline_for(kHeaderY, 30.0f),
             30.0f, shared_.theme.text);

    tabs_.draw(canvas);
    const ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
    const Rect first = tabs_.tab_rect(fonts, 0);
    const Rect last = tabs_.tab_rect(fonts, kTabCount - 1);
    const float l1 = ui::button_width(ui::Button::l1, 34.0f);
    ui::draw_button(list, fonts, glyphs, ui::Button::l1, first.x - l1 - 20.0f, kHeaderY, 34.0f);
    ui::draw_button(list, fonts, glyphs, ui::Button::r1, last.x + last.w + 20.0f, kHeaderY, 34.0f);
    draw_status(canvas);
}

void App::draw_settings(ui::Canvas &canvas) const
{
    gfx::DrawList &list = canvas.list;
    const ui::Fonts &fonts = canvas.fonts;
    const ui::Theme &theme = shared_.theme;
    const bool reduced = shared_.settings.reduced_motion;
    ui::Painter paint(list, fonts, theme, canvas.glass);

    list.push_opacity(tween::stagger(page_age_, 0, 0.06f, 0.45f));
    ui::text(list, fonts.semibold, "HOW THE MENUS BEHAVE", kMargin, 162.0f, 18.0f, tone::accent,
             gfx::Align::left, 4.0f);
    paint.heading("Settings", kMargin - 3.0f, 224.0f, 60.0f);
    list.pop_opacity();

    list.push_opacity(tween::stagger(page_age_, 1, 0.06f, 0.45f));
    draw_glass(canvas, theme, kSettingsPanel, 26.0f);
    form_.draw(canvas);
    list.pop_opacity();

    // ---- what the app holds right now ----
    const Model &model = shared_.model;
    const float in = tween::stagger(page_age_, 2, 0.06f, 0.45f);
    list.push_opacity(in);
    list.push_transform(1.0f, 0.0f, 0.0f, reduced ? 0.0f : 24.0f * (1.0f - in), 0.0f);
    draw_glass(canvas, theme, kGlancePanel, 26.0f);
    const float x = kGlancePanel.x + 40.0f;
    const float right = kGlancePanel.x + kGlancePanel.w - 40.0f;
    float y = kGlancePanel.y + 56.0f;
    ui::text(list, fonts.semibold, "AT A GLANCE", x, y, 16.0f, tone::accent, gfx::Align::left,
             3.0f);
    y += 22.0f;
    const auto fact = [&](const char *label, const std::string &value)
    {
        y += 58.0f;
        ui::text(list, fonts.regular, label, x, y, 23.0f, theme.text_muted);
        ui::text(list, fonts.semibold, value, right, y, 23.0f, theme.text, gfx::Align::right);
        list.rounded_rect({x, y + 22.0f, right - x, 1.0f}, 0.0f, kWhite.with_alpha(0.1f));
    };
    fact("Source", source_label(model.active_source()));
    fact("Channels", model.has_catalog() ? group_digits(model.channel_count()) : "None yet");
    fact("Favorites", group_digits(model.group_size(Group::favorites)));
    fact("Recent channels", group_digits(model.group_size(Group::recent)));
    ui::paragraph(list, fonts.regular,
                  "Who made ProsperoTV, and how channels get here, is on the About tab.", x,
                  y + 76.0f, 21.0f, right - x, 30.0f, theme.text_muted, 3);
    list.pop_transform();
    list.pop_opacity();
}

void App::draw_about(ui::Canvas &canvas) const
{
    gfx::DrawList &list = canvas.list;
    const ui::Fonts &fonts = canvas.fonts;
    const ui::Theme &theme = shared_.theme;
    const bool reduced = shared_.settings.reduced_motion;
    ui::Painter paint(list, fonts, theme, canvas.glass);
    const Color rule_color = kWhite.with_alpha(0.12f);

    list.push_opacity(tween::stagger(page_age_, 0, 0.06f, 0.45f));
    ui::text(list, fonts.semibold, "CREDITS AND HOW IT WORKS", kMargin, 162.0f, 18.0f, tone::accent,
             gfx::Align::left, 4.0f);
    paint.heading("About", kMargin - 3.0f, 224.0f, 60.0f);
    list.pop_opacity();

    // A panel is written from the top down: each piece leaves `y` where the
    // next one starts.
    float x = 0.0f;
    float width = 0.0f;
    float y = 0.0f;
    const auto kicker = [&](const char *words)
    {
        ui::text(list, fonts.semibold, words, x, y, 16.0f, tone::accent, gfx::Align::left, 3.0f);
        y += 36.0f;
    };
    const auto words = [&](const char *value, Color color, int lines, float size = 22.0f) {
        y = ui::paragraph(list, fonts.regular, value, x, y, size, width, size + 9.0f, color, lines);
    };
    const auto rule = [&]()
    {
        list.rounded_rect({x, y - 6.0f, width, 1.0f}, 0.0f, rule_color);
        y += 32.0f;
    };

    // ---- who made it, and whose work it stands on ----
    list.push_opacity(tween::stagger(page_age_, 1, 0.06f, 0.45f));
    draw_glass(canvas, theme, kCreditsPanel, 26.0f);
    x = kCreditsPanel.x + 40.0f;
    width = kCreditsPanel.w - 80.0f;
    y = kCreditsPanel.y + 56.0f;
    ui::text(list, fonts.semibold, "PROJECT CREDITS", x, y, 16.0f, tone::accent, gfx::Align::left,
             3.0f);
    draw_mark(list, x + 34.0f, y + 74.0f, 68.0f);
    paint.heading("ProsperoTV", x + 88.0f, y + 74.0f, 40.0f);
    ui::text(list, fonts.mono, "Version " + version_, x + 90.0f, y + 108.0f, 20.0f,
             theme.text_muted);
    y += 164.0f;
    words("Live television for PS5 homebrew, brought to you by BlackBearReloaded. Unofficial, and "
          "free software under the GNU General Public License, version 3 or later.",
          theme.text, 3, 23.0f);
    rule();
    kicker("CHANNEL LIST");
    words("The built-in list is the public playlist of the iptv-org community project. All credit "
          "for it goes to its maintainers.",
          theme.text, 2);
    ui::text(list, fonts.semibold, "github.com/iptv-org/iptv", x, y, 22.0f, tone::accent);
    y += 31.0f;
    rule();
    kicker("THANKS");
    words("Thanks to JMUtechnologies for testing and feedback, to the whole PS5 homebrew "
          "community, and to every developer whose tools and libraries make ProsperoTV possible.",
          theme.text, 3);
    ui::text(list, fonts.regular, "Menu sound effects made with ElevenLabs.", x,
             kCreditsPanel.y + kCreditsPanel.h - 36.0f, 19.0f, theme.text_muted);
    list.pop_opacity();

    // ---- how channels get here ----
    const float in = tween::stagger(page_age_, 2, 0.06f, 0.45f);
    list.push_opacity(in);
    list.push_transform(1.0f, 0.0f, 0.0f, reduced ? 0.0f : 24.0f * (1.0f - in), 0.0f);
    draw_glass(canvas, theme, kGuidePanel, 26.0f);
    x = kGuidePanel.x + 40.0f;
    width = kGuidePanel.w - 80.0f;
    y = kGuidePanel.y + 56.0f;
    ui::text(list, fonts.semibold, "GETTING STARTED", x, y, 16.0f, tone::accent, gfx::Align::left,
             3.0f);
    paint.heading("Three ways to get channels", x - 2.0f, y + 66.0f, 40.0f);
    y += 126.0f;
    const float column = x + 150.0f;
    const auto way = [&](const char *label, const char *value)
    {
        ui::text(list, fonts.semibold, label, x, y, 16.0f, theme.text_muted, gfx::Align::left,
                 2.0f);
        ui::text(list, fonts.regular, fonts.regular.font->fit(value, 22.0f, x + width - column),
                 column, y, 22.0f, theme.text);
        y += 46.0f;
    };
    way("BUILT IN", "The iptv-org list, ready at the first launch");
    way("PLAYLIST", "Any M3U playlist, by its web address");
    way("ACCOUNT", "An Xtream Codes account from your provider");
    y -= 6.0f;
    words("Choose and set them up on the Sources tab.", theme.text_muted, 1, 21.0f);
    rule();
    kicker("PLAYBACK");
    words("H.264, HEVC and VP9 video, up to 4K, through the console's own decoder. When a channel "
          "lists several addresses they are tried in turn.",
          theme.text, 3);
    rule();
    kicker("GOOD TO KNOW");
    words("ProsperoTV hosts no streams of its own: every channel plays from the address its "
          "playlist gives, and channels come and go without notice. Your sources, favorites and "
          "recent channels stay on this console.",
          theme.text, 4);
    list.pop_transform();
    list.pop_opacity();
}

App::TuningBar App::tuning_bar()
{
    return {kTuningBar, tone::accent, kTuningStart};
}

void App::draw_tuning(Frame &frame, const std::string &channel_id, float t,
                      float preview_fill) const
{
    draw(frame);
    const ui::Theme &theme = shared_.theme;
    const ui::Fonts &fonts = shared_.fonts;
    const std::optional<iptv::ChannelView> channel = shared_.model.find(channel_id);
    ui::Canvas over{frame.overlay, fonts, glass_texture_, shared_.clock};
    gfx::DrawList &list = frame.overlay;
    const float in = tween::clamp01(t);
    const float eased = in * in * (3.0f - 2.0f * in);
    const bool reduced = shared_.settings.reduced_motion;

    list.push_opacity(eased);
    // The menu goes behind frosted glass, then into the dark.
    // A little larger than the screen: its hairline of light stays outside.
    draw_glass(over, theme, {-8.0f, -8.0f, kWidth + 16.0f, kHeight + 16.0f}, 0.0f);
    list.rounded_rect({0.0f, 0.0f, kWidth, kHeight}, 0.0f, tone::night.with_alpha(0.64f));

    // The channel's picture, lit in its own colour, settling into place.
    const Color accent = channel ? art_colors(channel->id).accent : tone::ember;
    const float lift = reduced ? 0.0f : 18.0f * (1.0f - eased);
    const float scale = reduced ? 1.0f : 0.94f + 0.06f * eased;
    const Rect set = tv_body(kTuningArt);
    list.glow(set.inset(-40.0f), theme.radius_card + 40.0f, 240.0f, accent.with_alpha(0.28f));
    list.push_transform(scale, kTuningArt.cx(), kTuningArt.cy(), 0.0f, lift);
    list.shadow({set.x, set.y + 28.0f, set.w, set.h}, theme.radius_card, 60.0f,
                Color::rgb(0x000000, 0.55f));
    if (channel)
        draw_channel_art(list, fonts, kTuningArt, theme.radius_card, *channel);
    list.pop_transform();

    // What is opening.
    const float cx = kWidth * 0.5f;
    ui::text(list, fonts.semibold, "TUNING IN", cx, 662.0f + lift * 0.5f, 18.0f, tone::accent,
             gfx::Align::center, 5.0f);
    if (channel)
    {
        const std::string name = shown_name(fonts, *channel);
        const ui::FontRef &face = title_face(fonts, name);
        const float size = face.measure(name, 60.0f) <= 1300.0f ? 60.0f : 46.0f;
        ui::text(list, face, face.font->fit(name, size, 1300.0f), cx, 740.0f + lift * 0.5f, size,
                 theme.text, gfx::Align::center);
        std::string line = readable(fonts.regular, place_line(*channel));
        const std::string picture = resolution_label(*channel);
        if (!picture.empty())
            line += "  \xC2\xB7  " + picture;
        ui::text(list, fonts.regular, fonts.regular.font->fit(line, 25.0f, 1300.0f), cx,
                 788.0f + lift * 0.5f, 25.0f, theme.text_muted, gfx::Align::center);
    }

    // The bar: its track here; the player fills it (in tone::accent) while the
    // channel opens.
    const float radius = kTuningBar.h * 0.5f;
    list.rounded_rect(kTuningBar, radius, kWhite.with_alpha(0.14f));
    if (preview_fill > 0.0f)
        list.rounded_rect({kTuningBar.x, kTuningBar.y,
                           std::max(kTuningBar.h, kTuningBar.w * tween::clamp01(preview_fill)),
                           kTuningBar.h},
                          radius, tone::accent);
    list.pop_opacity();
    frame.glass = true;
}

void App::draw_hints(ui::Canvas &canvas) const
{
    ui::Hint hints[8];
    int count = 0;
    switch (tabs_.active())
    {
    case kLive:
    case kFavorites:
        count = browse_.hints(hints, 6);
        break;
    case kSources:
        count = sources_.hints(hints, 6);
        break;
    case kSettings:
        if (form_.uses_horizontal())
            hints[count++] = {ui::Button::dpad, "Change"};
        else
            hints[count++] = {ui::Button::cross, "Choose"};
        break;
    default:
        break;
    }
    if (tabs_.active() < kSettings)
        hints[count++] = {ui::Button::options, "Update"};
    if (tabs_.active() != kLive && !(browsing() && shared_.model.filtering()))
        hints[count++] = {ui::Button::circle, "Live TV"};
    ui::HintLayout layout;
    layout.size = 36.0f;
    layout.text_size = 23.0f;
    layout.cy = kHintsY;
    layout.item_gap = 34.0f;
    ui::draw_hints(canvas.list, canvas.fonts, ui::GlyphStyle::dark(), hints, count,
                   kWidth - kMargin, true, layout);
    if (!remote_hint_.empty())
        ui::text(canvas.list, canvas.fonts.regular, remote_hint_, kMargin, 1062.0f, 20.0f,
                 shared_.theme.text_muted);
    // While the diagnostic log is on, every screen says so: it is easy to
    // forget, and whoever looks at a picture of the screen should know too.
    if (diag::enabled())
    {
        constexpr char kSign[] = "Diagnostic log on";
        const float width = canvas.fonts.semibold.measure(kSign, 20.0f);
        const float right = kWidth - kMargin;
        canvas.list.circle(right - width - 16.0f, 1055.0f, 5.0f, tone::ember);
        ui::text(canvas.list, canvas.fonts.semibold, kSign, right, 1062.0f, 20.0f, tone::accent,
                 gfx::Align::right);
    }
}

void App::draw(Frame &frame) const
{
    frame.reset();
    frame.glass_texture = glass_texture_;
    frame.backdrop = dusk_sky(lean_.value(), lean_dark_.value(), lean_amount_.value, drift_ * 0.6f);

    // The screens: nothing here frosts anything, so no blurred copy is needed.
    ui::Canvas canvas{frame.scene, shared_.fonts, 0, shared_.clock};
    draw_header(canvas);
    switch (tabs_.active())
    {
    case kLive:
    case kFavorites:
        browse_.draw(canvas);
        break;
    case kSources:
        sources_.draw(canvas);
        break;
    case kSettings:
        draw_settings(canvas);
        break;
    default:
        draw_about(canvas);
        break;
    }
    draw_hints(canvas);

    // What floats: glass over the screens.
    ui::Canvas over{frame.overlay, shared_.fonts, glass_texture_, shared_.clock};
    shared_.toasts.draw(over);
    announcements_.draw(over);
    search_.draw(over);
    failure_.draw(over);
    update_.draw(over);
    if (pairing_open_)
        draw_pairing(over);
    frame.glass = !frame.overlay.empty();
    if (intro_ >= 0.0f)
        draw_intro(over);
}

void App::draw_intro(ui::Canvas &canvas) const
{
    gfx::DrawList &list = canvas.list;
    const float t = intro_;
    const float arrive = tween::cubic_out(tween::inverse_lerp(0.0f, kIntroArrive, t));
    const float line = tween::cubic_out(tween::inverse_lerp(kIntroArrive, kIntroLine, t));
    const float open = tween::cubic_out(tween::inverse_lerp(kIntroLine, kIntroOpen, t));
    const float mark =
        tween::smoothstep(tween::inverse_lerp(kIntroOpen - 0.05f, kIntroOpen + 0.4f, t));
    const float dive = tween::cubic_in_out(tween::inverse_lerp(kIntroHold, kIntroEnd, t));

    // The room is dark until the view is through the glass.
    const float dark = 1.0f - tween::smoothstep(tween::inverse_lerp(0.35f, 0.9f, dive));
    list.rounded_rect({-8.0f, -8.0f, kWidth + 16.0f, kHeight + 16.0f}, 0.0f,
                      Color::rgb(0x070202, dark));

    // Where the screen is before the set is drawn, so the view can aim at it.
    const Rect body = tv_body(kIntroSet);
    const float bezel = body.h * 0.075f;
    const Rect glass{body.x + bezel, body.y + bezel, body.w - 2.0f * bezel - body.w * 0.135f,
                     body.h - 2.0f * bezel};
    // Into the screen: it grows until it is the whole view, and gives way.
    const float full = std::max(kWidth / glass.w, kHeight / glass.h) * 1.12f;
    const float scale = std::pow(full, dive);
    const float settle = 0.96f + 0.04f * arrive;
    list.push_opacity(arrive * (1.0f - tween::smoothstep(tween::inverse_lerp(0.5f, 1.0f, dive))));
    list.push_transform(scale * settle, glass.cx(), glass.cy(), (kWidth * 0.5f - glass.cx()) * dive,
                        (kHeight * 0.5f - glass.cy()) * dive + 14.0f * (1.0f - arrive));

    // The light the picture throws on the room.
    list.glow(body.inset(-30.0f), 60.0f, 260.0f, tone::ember.with_alpha(0.26f * open));
    list.shadow({body.x, body.y + 30.0f, body.w, body.h}, 40.0f, 70.0f, Color::rgb(0x000000, 0.6f));
    const ArtColors wood{Color::rgb(0xb4572a), Color::rgb(0x6e2a14),
                         gfx::mix(tone::ember, Color::rgb(0x3a1410), 1.0f - open)};
    const Rect screen = draw_tv_shell(list, kIntroSet, 44.0f, wood);
    const float corner = screen.h * 0.13f;

    // Off: dark glass. Then a line of light across it, which opens.
    list.rounded_rect(screen, corner, Color::rgb(0x0b0504));
    if (line > 0.0f)
    {
        const float lit_w = screen.w * (open > 0.0f ? 1.0f : 0.06f + 0.94f * line);
        const float lit_h = tween::lerp(5.0f, screen.h, open);
        const Rect lit{screen.cx() - lit_w * 0.5f, screen.cy() - lit_h * 0.5f, lit_w, lit_h};
        list.push_clip(lit);
        list.gradient_rect(screen, corner, Color::rgb(0xc8431f), tone::wine);
        // The picture: the app's mark and name, as a station's card.
        list.push_opacity(mark);
        list.circle(screen.cx(), screen.cy() - 26.0f, 150.0f, kWhite.with_alpha(0.06f));
        draw_mark(list, screen.cx(), screen.cy() - 34.0f, 132.0f);
        ui::text(list, shared_.fonts.display, "ProsperoTV", screen.cx(), screen.cy() + 96.0f, 54.0f,
                 kWhite.with_alpha(0.96f), gfx::Align::center);
        list.pop_opacity();
        // The tube warming up: white first, then the colour comes through.
        list.rounded_rect(screen, corner, kWhite.with_alpha(0.92f * (1.0f - open)));
        list.pop_clip();
        list.glow(lit, std::min(corner, lit.h * 0.5f), 36.0f,
                  kWhite.with_alpha(0.5f * (1.0f - open) * line));
    }
    list.gradient_rect({screen.x, screen.y, screen.w, screen.h * 0.46f}, corner,
                       kWhite.with_alpha(0.10f), kWhite.with_alpha(0.0f));
    list.bordered_rect(screen, corner, kClear, 2.0f, kWhite.with_alpha(0.16f));
    list.pop_transform();
    list.pop_opacity();
}

} // namespace ptv
