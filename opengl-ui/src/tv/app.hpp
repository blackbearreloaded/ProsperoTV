// ProsperoTV - The interface: tabs, the screens, and everything that floats.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "tv/browse_screen.hpp"
#include "tv/search_sheet.hpp"
#include "tv/shared.hpp"
#include "tv/sources_screen.hpp"
#include "tv/update_sheet.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/form.hpp"
#include "ui/components/tabs.hpp"

#include <string>
#include <utility>

namespace ptv
{

// The whole menu as one object: the frame loop gives it the controller and
// the elapsed time, it gives back a frame to draw and the sounds to play.
// It is made when the menu opens and thrown away when a channel starts; what
// must outlive that (the catalog, the filters, where the focus was) is in the
// Model it is given.
class App
{
  public:
    // glass_texture is the renderer's blurred copy of the frame (panels frost
    // it); version is shown on the About tab.
    App(Model &model, const ui::Fonts &fonts, std::uint32_t glass_texture, const Settings &settings,
        std::string version);

    void update(const InputFrame &input, float dt, ui::Feedback &feedback);
    bool accepts_remote_search() const;
    bool remote_search(const char *query);
    void set_volume(int volume);
    void set_pairing_info(std::string url, std::string code, unsigned seconds, unsigned phones);
    bool pairing_open() const { return pairing_open_; }
    void phone_connected();
    bool take_pair_phone_requested() { return std::exchange(pair_requested_, false); }
    bool take_forget_phones_requested() { return std::exchange(forget_requested_, false); }
    void remote_notice(const char *message);
    void set_remote_hint(std::string hint)
    {
        remote_hint_ = std::move(hint);
    }
    // The opening, once per launch: an old television switches on, the view
    // goes into its screen and the app is there. Any button ends it; with
    // Reduce motion it is not played. Until it ends the controller is not read.
    void play_intro();
    bool intro_playing() const
    {
        return intro_ >= 0.0f;
    }
    // The opening at a moment of its own, in seconds (pictures made on a PC only).
    void set_intro_time(float seconds)
    {
        intro_ = seconds;
    }
    // Records the frame. It changes nothing: it may run more than once.
    void draw(Frame &frame) const;

    // The screen a channel opens on. The menu gives way to it as t goes from
    // 0 to 1; its last picture, the bar still empty, is what the player keeps
    // on the television until the channel's first picture. preview_fill draws
    // the bar filled that far (pictures made on a PC only).
    void draw_tuning(Frame &frame, const std::string &channel_id, float t,
                     float preview_fill = 0.0f) const;
    struct TuningBar
    {
        Rect rect;      // in the 1920 x 1080 picture
        Color fill;     // what the player fills it with
        float start;    // how full it is when the player takes over
    };
    static TuningBar tuning_bar();

    const Settings &settings() const
    {
        return shared_.settings;
    }
    // True once after the player changed a setting: time to save them.
    bool take_settings_changed();
    // True once a newer version is staged: the app must close itself now, so
    // its files can be replaced.
    bool wants_quit() const
    {
        return update_.wants_quit();
    }

    // ---- where the interface is, for tests ----
    int tab() const
    {
        return tabs_.active();
    }
    bool searching() const
    {
        return search_.is_open();
    }
    bool asking() const
    {
        return failure_.is_open();
    }
    const UpdateSheet &update_sheet() const
    {
        return update_;
    }
    bool on_letters() const
    {
        return browsing() && browse_.on_letters();
    }

  private:
    enum Tab : int
    {
        kLive,
        kFavorites,
        kSources,
        kSettings,
        kAbout,
        kTabCount,
    };

    bool browsing() const
    {
        return tabs_.active() <= kFavorites;
    }
    void show_tab(int index, bool glide);
    void tab_changed();
    void refresh(ui::Feedback &feedback);
    void open_failure(ui::Feedback &feedback);
    void handle_screen(const InputFrame &input, ui::Feedback &feedback);
    void apply_settings();
    void follow_channel(float dt);

    void draw_header(ui::Canvas &canvas) const;
    void draw_status(ui::Canvas &canvas) const;
    void draw_settings(ui::Canvas &canvas) const;
    void draw_pairing(ui::Canvas &canvas) const;
    void draw_about(ui::Canvas &canvas) const;
    void draw_hints(ui::Canvas &canvas) const;
    void draw_intro(ui::Canvas &canvas) const;
    void step(const InputFrame &input, float dt, ui::Feedback &feedback);

    Shared shared_;
    BrowseScreen browse_;
    SourcesScreen sources_;
    SearchSheet search_;
    ui::TabBar tabs_;
    ui::Form form_;
    ui::Dialog failure_;
    UpdateSheet update_;
    // What the app itself announces (a newer version): top right, for longer.
    ui::ToastStack announcements_;

    std::uint32_t glass_texture_ = 0;
    std::string version_;
    std::string remote_hint_;
    std::string pair_url_, pair_code_;
    std::vector<bool> pair_qr_;
    int pair_qr_size_ = 0;
    unsigned pair_seconds_ = 0;
    bool pairing_open_ = false, pair_requested_ = false, forget_requested_ = false;
    bool settings_changed_ = false;
    bool failure_seen_ = false;
    float page_age_ = 10.0f; // seconds since the tab changed
    // The backdrop leans toward the colour of the channel in focus.
    ui::SpringColor lean_;
    ui::SpringColor lean_dark_;
    tween::Spring lean_amount_;
    float drift_ = 0.0f;
    float intro_ = -1.0f; // seconds into the opening; negative: not playing
};

} // namespace ptv
