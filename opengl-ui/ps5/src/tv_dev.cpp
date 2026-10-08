// ProsperoTV - Scripted runs for hardware tests of the test title.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv_dev.hpp"

#include "core/input.hpp"
#include "core/save_file.hpp"
#include "platform/ps5/system.hpp"
#include "tv/app.hpp"

#include <dirent.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tv_dev
{

namespace
{

// A press is held for a few frames, then let go long enough for the
// interface to have moved before the next one.
constexpr int kPressFrames = 4;
constexpr int kPressCycle = 20;
// A frame that took longer than this missed the display's 60 Hz.
constexpr float kSlowFrame = 0.0185f;

struct Name
{
    const char *name;
    std::uint32_t bit;
};
constexpr Name kButtons[] = {
    {"up", hui::pad_bits::kUp},           {"down", hui::pad_bits::kDown},
    {"left", hui::pad_bits::kLeft},       {"right", hui::pad_bits::kRight},
    {"cross", hui::pad_bits::kCross},     {"circle", hui::pad_bits::kCircle},
    {"square", hui::pad_bits::kSquare},   {"triangle", hui::pad_bits::kTriangle},
    {"l1", hui::pad_bits::kL1},           {"r1", hui::pad_bits::kR1},
    {"l2", hui::pad_bits::kL2},           {"r2", hui::pad_bits::kR2},
    {"options", hui::pad_bits::kOptions}, {"touchpad", hui::pad_bits::kTouchpad},
    {"r3", hui::pad_bits::kR3},
};

std::uint32_t button(const std::string &name)
{
    for (const Name &entry : kButtons)
        if (name == entry.name)
            return entry.bit;
    return 0;
}

const char *kTabNames[] = {"live", "favorites", "sources", "vod", "settings", "about"};

} // namespace

bool Script::load(const std::string &request_path, const std::string &out_dir)
{
    std::string request;
    if (!hui::save::read_file(request_path, &request, 16384))
        return false;
    std::vector<std::string> lines;
    for (std::size_t from = 0; from < request.size();)
    {
        std::size_t to = request.find('\n', from);
        if (to == std::string::npos)
            to = request.size();
        std::string line = request.substr(from, to - from);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        from = to + 1;
    }
    char verb[16] = {};
    char word[96] = {};
    if (lines.empty() || std::sscanf(lines[0].c_str(), "%15s %95s", verb, word) != 2 ||
        std::strcmp(verb, "run") != 0)
        return false;
    token_ = word;

    hui::save::ensure_directory(out_dir);
    const std::string handled_path = out_dir + "/handled.txt";
    std::string handled;
    hui::save::read_file(handled_path, &handled, 128);
    if (handled == token_)
        return false;
    hui::save::write_atomic(handled_path, token_);
    // The pictures and receipts of an earlier run must not pass for this one's.
    if (DIR *folder = opendir(out_dir.c_str()))
    {
        std::vector<std::string> stale;
        while (const dirent *entry = readdir(folder))
        {
            const std::string name = entry->d_name;
            if ((name.size() > 4 && name.compare(name.size() - 4, 4, ".bmp") == 0) ||
                name.rfind("receipt-", 0) == 0 || name == "report.txt")
                stale.push_back(out_dir + "/" + name);
        }
        closedir(folder);
        for (const std::string &path : stale)
            std::remove(path.c_str());
    }

    out_dir_ = out_dir;
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
        Step step;
        float number = 0.0f;
        word[0] = '\0';
        const int fields = std::sscanf(lines[i].c_str(), "%15s %95s %f", verb, word, &number);
        if (fields < 1 || verb[0] == '#')
            continue;
        const std::string what = verb;
        step.text = word;
        if (what == "wait" && fields >= 2)
        {
            step.kind = Kind::wait;
            step.seconds = static_cast<float>(std::atof(word));
        }
        else if (what == "until" && fields == 3 && (step.text == "catalog" || step.text == "menu"))
        {
            step.kind = Kind::until;
            step.seconds = number;
        }
        else if (what == "press" && fields >= 2 && (step.buttons = button(step.text)) != 0)
        {
            step.kind = Kind::press;
            step.times = fields == 3 && number >= 1.0f ? static_cast<int>(number) : 1;
        }
        else if (what == "hold" && fields == 3 && (step.buttons = button(step.text)) != 0)
        {
            step.kind = Kind::hold;
            step.seconds = number;
        }
        else if (what == "query" && fields >= 2)
        {
            step.kind = Kind::query;
            // The rest of the line is the text.
            const std::size_t at = lines[i].find(word, std::strlen(verb));
            step.text = lines[i].substr(at);
        }
        else if (what == "watch" && fields >= 2)
        {
            step.kind = Kind::watch;
            step.seconds = static_cast<float>(std::atof(word));
        }
        else if (what == "shot" && fields >= 2)
            step.kind = Kind::shot;
        else if (what == "status")
            step.kind = Kind::status;
        else if (what == "quit")
        {
            step.kind = Kind::quit;
            step.seconds = fields >= 2 ? static_cast<float>(std::atof(word)) : 0.0f;
        }
        else
        {
            note("step not understood: %s", lines[i].c_str());
            continue;
        }
        steps_.push_back(std::move(step));
    }
    active_ = true;
    note("script token=%s steps=%zu", token_.c_str(), steps_.size());
    return true;
}

void Script::note(const char *format, ...)
{
    char text[400];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    char line[440];
    std::snprintf(line, sizeof(line), "t=%.1f %s", static_cast<double>(total_), text);
    notes_.emplace_back(line);
    hui::sys::log("[TV] dev %s", line);
}

void Script::closing(const char *why)
{
    if (out_dir_.empty())
        return;
    note("closing: %s", why);
    finish();
    active_ = false;
}

void Script::menu_opened(std::uint64_t session)
{
    // A press that chose a channel closed the menu in its first frame: it is
    // over, and must not press again in the menu that opens now.
    if (active_ && at_ < steps_.size() && steps_[at_].kind == Kind::press && frame_ > 0)
    {
        note("press %s: a channel started", steps_[at_].text.c_str());
        next();
    }
    session_ = session;
}

void Script::next()
{
    ++at_;
    clock_ = 0.0f;
    frame_ = 0;
    step_session_ = session_;
}

void Script::finish()
{
    std::string report = "finished " + token_ + "\n";
    for (const std::string &line : notes_)
        report += line + "\n";
    hui::save::write_atomic(out_dir_ + "/report.txt", report);
    reported_ = true;
}

void Script::capture_done(bool ok)
{
    note("shot %s %s", capture_.substr(capture_.rfind('/') + 1).c_str(), ok ? "saved" : "FAILED");
    capture_.clear();
}

void Script::status_line(const ptv::Model &model, const ptv::App &app)
{
    const int position = model.position_of(model.view.focused_channel);
    std::string focused = "-";
    if (position >= 0)
        focused = model.channel(model.visible(static_cast<unsigned>(position))).name;
    const int tab = app.tab();
    note("status tab=%s channels=%u visible=%u position=%d focused=\"%s\" letters=%d search=%d "
         "query=\"%s\" refreshing=%d level=%d frames=%d avg=%.2fms worst=%.2fms slow=%d",
         tab >= 0 && tab < 6 ? kTabNames[tab] : "?", model.channel_count(), model.visible_count(),
         position + 1, focused.c_str(), app.on_letters() ? 1 : 0, app.searching() ? 1 : 0,
         model.query().c_str(), model.refreshing() ? 1 : 0, static_cast<int>(model.level()),
         frames_, frames_ > 0 ? static_cast<double>(frame_sum_ / frames_ * 1000.0f) : 0.0,
         static_cast<double>(frame_worst_ * 1000.0f), slow_frames_);
    frames_ = 0;
    slow_frames_ = 0;
    frame_sum_ = 0.0f;
    frame_worst_ = 0.0f;
}

std::uint32_t Script::step(float dt, ptv::Model &model, const ptv::App &app)
{
    if (!active_)
        return 0;
    total_ += dt;
    ++frames_;
    frame_sum_ += dt;
    frame_worst_ = dt > frame_worst_ ? dt : frame_worst_;
    if (dt > kSlowFrame)
        ++slow_frames_;
    for (;;)
    {
        if (at_ >= steps_.size())
        {
            note("script done");
            finish();
            active_ = false; // the controller is read again
            return 0;
        }
        const Step &step = steps_[at_];
        switch (step.kind)
        {
        case Kind::wait:
            clock_ += dt;
            if (clock_ < step.seconds)
                return 0;
            next();
            break;
        case Kind::until:
        {
            clock_ += dt;
            const bool so = step.text == "catalog" ? model.has_catalog() && !model.refreshing()
                                                   : session_ > step_session_;
            const bool hopeless =
                step.text == "catalog" && model.catalog_failed() && !model.refreshing();
            if (!so && clock_ < step.seconds && !hopeless)
                return 0;
            note("until %s: %s after %.1f s", step.text.c_str(), so ? "reached" : "NOT REACHED",
                 static_cast<double>(clock_));
            next();
            break;
        }
        case Kind::press:
        {
            const int frame = frame_++;
            if (frame >= kPressCycle * step.times)
            {
                note("press %s x%d", step.text.c_str(), step.times);
                next();
                break;
            }
            return frame % kPressCycle < kPressFrames ? step.buttons : 0;
        }
        case Kind::hold:
            clock_ += dt;
            if (clock_ < step.seconds)
                return step.buttons;
            note("hold %s %.1f s", step.text.c_str(), static_cast<double>(step.seconds));
            next();
            return 0; // let go for a frame before the next step
        case Kind::query:
            model.set_query(step.text == "-" ? std::string_view() : std::string_view(step.text));
            note("query \"%s\": %u channels", model.query().c_str(), model.visible_count());
            next();
            break;
        case Kind::watch:
            watch_ms_ = static_cast<unsigned>(step.seconds * 1000.0f);
            note("watch %.0f s", static_cast<double>(step.seconds));
            next();
            break;
        case Kind::shot:
            capture_ = out_dir_ + "/" + step.text + ".bmp";
            next();
            return 0; // the frame drawn now is the one that is saved
        case Kind::status:
            status_line(model, app);
            next();
            break;
        case Kind::quit:
            // The report first, then the wait: the output is in the title's
            // storage, which a PC can read only while the app runs.
            if (!reported_)
            {
                note("quit in %.0f s", static_cast<double>(step.seconds));
                finish();
            }
            clock_ += dt;
            if (clock_ < step.seconds)
                return 0;
            active_ = false;
            quit_ = true;
            return 0;
        }
    }
}

} // namespace tv_dev
