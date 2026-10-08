// ProsperoTV - Scripted runs for hardware tests of the test title.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A PC leaves dev/request.txt beside the installed test title (FTP into its
// folder). Its first line is "run <token>"; every other line is a step:
//
//   wait <seconds>
//   until catalog <seconds>     the channel list is loaded and not updating
//   until menu <seconds>        the menu has opened again after a channel
//   press <button> [times]      up down left right cross circle square triangle
//                               l1 r1 l2 r2 r3 options touchpad
//   hold <button> <seconds>     the button stays down
//   query <text>                the search text, as the keyboard would set it ("-" clears)
//   watch <seconds>             how long the next channel plays before it stops
//   shot <name>                 the frame drawn now, as <name>.bmp (960x540)
//   status                      a line about the catalog, the focus and the frames
//   quit [seconds]              write the report, wait, then close the app
//
// While a script runs the controller is not read. The output (handled.txt,
// report.txt, the pictures) goes to /data/prosperotv/logs/dev; without
// filesystem access it goes to /download0/prosperotv/dev, which a PC reads at
// /mnt/sandbox/<TITLE>_000/download0/prosperotv/dev only while the title
// runs: that is why "quit" waits before it closes the app. A token is
// honoured once.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ptv
{
class App;
class Model;
} // namespace ptv

namespace tv_dev
{

class Script
{
  public:
    bool load(const std::string &request_path, const std::string &out_dir);
    bool active() const
    {
        return active_;
    }
    // The menu has opened (session counts them): "until menu" waits for this.
    void menu_opened(std::uint64_t session);
    // One frame of the menu: the buttons to press now.
    std::uint32_t step(float dt, ptv::Model &model, const ptv::App &app);
    // How long the next channel plays, in ms (0: as long as the viewer wants).
    unsigned watch_ms() const
    {
        return active_ ? watch_ms_ : 0u;
    }
    // Picture asked for this frame (empty: none) and how it went.
    const std::string &capture() const
    {
        return capture_;
    }
    void capture_done(bool ok);
    bool wants_quit() const
    {
        return quit_;
    }
    void note(const char *format, ...) __attribute__((format(printf, 2, 3)));
    // The app is about to close for a reason of its own (an update): the
    // report is written now, with why.
    void closing(const char *why);

  private:
    enum class Kind
    {
        wait,
        until,
        press,
        hold,
        query,
        watch,
        shot,
        status,
        quit,
    };
    struct Step
    {
        Kind kind = Kind::wait;
        std::string text;
        std::uint32_t buttons = 0;
        float seconds = 0.0f;
        int times = 1;
    };

    void next();
    void finish();
    void status_line(const ptv::Model &model, const ptv::App &app);

    std::vector<Step> steps_;
    std::vector<std::string> notes_;
    std::string token_;
    std::string out_dir_;
    std::string capture_;
    std::size_t at_ = 0;
    float clock_ = 0.0f;
    float total_ = 0.0f;
    int frame_ = 0;
    unsigned watch_ms_ = 15000;
    std::uint64_t session_ = 0;
    std::uint64_t step_session_ = 0;
    bool active_ = false;
    bool reported_ = false;
    bool quit_ = false;
    // Frame times since the last status line.
    int frames_ = 0;
    int slow_frames_ = 0;
    float frame_sum_ = 0.0f;
    float frame_worst_ = 0.0f;
};

} // namespace tv_dev
