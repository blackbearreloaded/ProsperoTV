// ProsperoTV - Application entry point: the menu and the player take turns.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The menu is drawn with OpenGL and the video is presented by the player's
// own AGC presenter. The two must never own the display at the same time, so
// the process alternates: open the menu (display, renderer, controller,
// sounds), run it until a channel is chosen, close all of it, play the
// channel, and open the menu again where it was.

#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/version.hpp"
#include "gfx/canvas.hpp"
#include "gfx/renderer.hpp"
#include "iptv_ime.h"
#include "iptv_native_backend.h"
#include "iptv_player.h"
#include "iptv_remote.h"
#include "iptv_native_backend.h"
#include "iptv_store.h"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "tv/app.hpp"
#include "tv/remote_input.hpp"
#include "tv_build_options.h"
#include "tv_dev.hpp"
#include "tv_paths.h"
#include "tv_storage.hpp"
#include "tv_tuning.h"
#include "tv_update.hpp"
#include "tv/platform.hpp"
#include "tv/stream_sniff.hpp"

#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifndef IPTV_AUTOTEST_ENABLED
#define IPTV_AUTOTEST_ENABLED 0
#endif
#ifndef IPTV_REMOTE_CAPTURE
#define IPTV_REMOTE_CAPTURE 0
#endif

extern "C" int sceKernelUsleep(std::uint32_t microseconds);
extern "C" int sceSysmoduleLoadModule(std::uint32_t id);
extern "C" int sceCommonDialogInitialize(void);
// The player's decoders, loaded and woken (ps5/patch_tree.py adds it to the
// player's backend): [0] the video module, [1] its compute part, [2..4] H.264,
// HEVC and VP9, [5] the audio module and its AAC library.
extern "C" void iptv_native_backend_warm(std::int32_t results[6]);
extern "C" int sceKernelSendNotificationRequest(std::uint32_t device, void *request,
                                                std::size_t size, int blocking);
extern "C" long write(int descriptor, const void *buffer, std::size_t bytes);
extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);

namespace
{

using namespace hui;

// The system keyboard (iptv_ime.c uses the same number).
constexpr std::uint32_t kKeyboardModule = 0x0096;

// Whether Cross is down right now, whatever it is mapped to.
bool g_cross_held = false;
bool g_splash_hidden = false;
std::uint64_t g_menu_sessions = 0;

struct NotificationRequest
{
    std::uint8_t reserved[45];
    char message[3075];
};

// A failure the player must see even though nothing can be drawn.
void notify_failure(const char *stage)
{
    NotificationRequest request{};
    std::snprintf(request.message, sizeof(request.message), "ProsperoTV could not open: %s",
                  stage != nullptr ? stage : "unknown stage");
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

void log_heap(const char *when)
{
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    sys::log("[TV] heap %s live=%zu peak=%zu blocks=%zu failures=%zu", when, live, peak, blocks,
             failures);
}

// Pictures of a scripted run: half size is enough to read every label.
constexpr int kCaptureWidth = 960;
constexpr int kCaptureHeight = 540;

// The bound framebuffer, bottom row first, as a 24-bit BMP.
bool save_picture(const std::string &path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y)
    {
        const unsigned char *in = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            line[static_cast<std::size_t>(x) * 3 + 0] = in[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = in[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok;
}

bool load_font(gfx::Renderer &renderer, const char *name, gfx::Font *font, ui::FontRef *ref)
{
    std::string data;
    const std::string path = tv::storage::app_file(std::string("assets/fonts/") + name);
    if (!save::read_file(path, &data, 64u << 20) || !font->load(data))
    {
        sys::log("[TV] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// The sharpest mode the settings allow, then 1080p.
bool open_display(ps5::Display &display, const ptv::Settings &settings)
{
    if (settings.resolution == ptv::Settings::kBest && ps5::Display::supports_display_modes())
    {
        if (display.open(3840, 2160))
            return true;
        sys::log("[TV] 2160p menu failed, using 1080p");
    }
    return display.open(1920, 1080);
}

// One frame of the menu, onto the television.
void render(gfx::Renderer &renderer, const ps5::Display &display, const ptv::Frame &frame)
{
    renderer.begin();
    renderer.backdrop(frame.backdrop);
    renderer.draw(frame.scene);
    if (frame.glass)
        renderer.glass();
    renderer.draw(frame.overlay);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    renderer.present(0, display.width(), display.height());
}

// The channel is chosen: the menu gives way to the tuning screen, and a
// picture of it (its bar still empty) goes to the player, which keeps it on
// the television, the bar moving, until the channel's first picture.
void hand_over_to_channel(const ptv::App &app, gfx::Renderer &renderer, ps5::Display &display,
                          ptv::Frame &frame, const std::string &channel_id, bool reduced)
{
    const std::int64_t started = sys::monotonic_us();
    const int steps = reduced ? 1 : 22;
    for (int step = 1; step <= steps; ++step)
    {
        app.draw_tuning(frame, channel_id, static_cast<float>(step) / static_cast<float>(steps));
        render(renderer, display, frame);
        if (!display.swap())
            break;
    }

    // The last frame once more, at the picture's size, into memory.
    constexpr int kPictureWidth = 1920;
    constexpr int kPictureHeight = 1080;
    std::vector<unsigned char> rgba(static_cast<std::size_t>(kPictureWidth) * kPictureHeight * 4);
    gfx::Canvas target;
    bool taken = target.create(kPictureWidth, kPictureHeight, 1);
    if (taken)
    {
        target.bind();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(target.framebuffer(), kPictureWidth, kPictureHeight);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
        glReadPixels(0, 0, kPictureWidth, kPictureHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        taken = glGetError() == GL_NO_ERROR;
    }
    if (taken)
    {
        // OpenGL hands the bottom row over first; the picture starts at the top.
        const std::size_t row = static_cast<std::size_t>(kPictureWidth) * 4;
        std::vector<unsigned char> swap(row);
        for (int y = 0; y < kPictureHeight / 2; ++y)
        {
            unsigned char *top = rgba.data() + static_cast<std::size_t>(y) * row;
            unsigned char *bottom =
                rgba.data() + static_cast<std::size_t>(kPictureHeight - 1 - y) * row;
            std::memcpy(swap.data(), top, row);
            std::memcpy(top, bottom, row);
            std::memcpy(bottom, swap.data(), row);
        }
        const ptv::App::TuningBar bar = ptv::App::tuning_bar();
        const auto byte = [](float value)
        {
            return static_cast<std::uint8_t>(value <= 0.0f   ? 0
                                             : value >= 1.0f ? 255
                                                             : value * 255.0f + 0.5f);
        };
        const std::uint8_t fill[3] = {byte(bar.fill.r), byte(bar.fill.g), byte(bar.fill.b)};
        tv_tuning_set_picture(rgba.data(), bar.rect.x, bar.rect.y, bar.rect.w, bar.rect.h, fill,
                              bar.start);
    }
    else
    {
        tv_tuning_clear();
    }
    sys::log("[TV] tuning picture %s in %lld ms", taken ? "taken" : "not available",
             static_cast<long long>((sys::monotonic_us() - started) / 1000));
}

// What the last channel left behind for the menu to say.
struct LastPlayback
{
    std::string channel_id;
    std::string channel_name;
    int result = 0;
    unsigned attempts = 0;
};

// One menu session. Returns true with a channel to play; false when the menu
// could not be opened at all.
bool run_menu(ptv::Model &model, ptv::Settings *settings, const LastPlayback &last,
              ptv::PlayRequest *request, tv_dev::Script &script)
{
    *request = {};
    ++g_menu_sessions;
    const std::int64_t opened = sys::monotonic_us();
    sys::log("[TV] menu open session=%llu", static_cast<unsigned long long>(g_menu_sessions));

    ps5::Display display;
    if (!open_display(display, *settings))
    {
        sys::log("[TV] fatal: display open failed");
        notify_failure("display");
        return false;
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    gfx::Font east_asian;
    gfx::Font korean;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono))
    {
        sys::log("[TV] fatal: renderer init failed");
        notify_failure("renderer");
        renderer.release();
        display.close();
        return false;
    }
    // Dusk uses four faces; the other two slots are for the scripts channel
    // names need: Chinese and Japanese in one, Korean in the other. Without
    // their files the names fall back to what the four can write.
    // Those two are tens of megabytes: each is loaded when the list first
    // shows a name that needs it (see the frame loop), not at every menu.
    fonts.pixel = fonts.mono;
    fonts.hand = fonts.regular;
    bool east_asian_loaded = false;
    bool korean_loaded = false;
    const auto load_wide_faces = [&]()
    {
        if (!east_asian_loaded && model.uses_east_asian())
        {
            east_asian_loaded = true;
            const std::int64_t began = sys::monotonic_us();
            const bool ok =
                load_font(renderer, "noto-sans-east-asian.huifont", &east_asian, &fonts.hand);
            sys::log("[TV] Chinese and Japanese face loaded=%d in %lld ms", ok ? 1 : 0,
                     static_cast<long long>((sys::monotonic_us() - began) / 1000));
        }
        if (!korean_loaded && model.uses_korean())
        {
            korean_loaded = true;
            const std::int64_t began = sys::monotonic_us();
            const bool ok = load_font(renderer, "noto-sans-korean.huifont", &korean, &fonts.pixel);
            sys::log("[TV] Korean face loaded=%d in %lld ms", ok ? 1 : 0,
                     static_cast<long long>((sys::monotonic_us() - began) / 1000));
        }
    };

    // The controller opens the user service, which the keyboard needs too.
    ps5::Pad pad;
    const bool pad_ready = pad.open();

    audio::Mixer mixer;
    int menu_volume = settings->volume;
    mixer.set_master_gain(static_cast<float>(menu_volume) / 100.0f);
    ps5::AudioOut audio_out;
    const bool audio_ready = audio_out.start(mixer);
    audio::SoundBank sounds;
    const audio::SoundBank::Stats bank = sounds.load(tv::storage::app_file("assets/audio/sfx"));

    const bool model_ready = model.open();
    if (last.result < 0)
        model.report_playback_failure(last.channel_id.c_str(), last.channel_name.c_str(),
                                      last.result, last.attempts, iptv_player_last_error());
    sys::log("[TV] menu ready display=%dx%d pad=%d audio=%d sounds=%d keyboard=%d model=%d "
             "catalog=%u in %lld ms",
             display.width(), display.height(), pad_ready ? 1 : 0, audio_ready ? 1 : 0, bank.files,
             model.keyboard_ready() ? 1 : 0, model_ready ? 1 : 0, model.channel_count(),
             static_cast<long long>((sys::monotonic_us() - opened) / 1000));

    const std::string version = read_content_version(tv::storage::app_file("sce_sys/param.json"));
    script.menu_opened(g_menu_sessions);
    bool chosen = false;
    {
        // On the heap: the interface holds every screen and is no small object.
        const std::unique_ptr<ptv::App> owned =
            std::make_unique<ptv::App>(model, fonts, renderer.glass_texture(), *settings,
                                       (version.empty() ? std::string("unknown") : version) +
                                           (TV_DEBUG_TRACE != 0 ? " debug trace" : ""));
        ptv::App &app = *owned;
        app.set_remote_hint(iptv_remote_hint());
        if (g_menu_sessions == 1)
            app.play_intro();
        InputTracker tracker;
        ui::Feedback feedback;
        ptv::Frame frame;
        FrameStats stats;
        PadSample samples[64];
        gfx::Canvas capture;
        std::uint64_t frames = 0;
        std::int64_t previous = sys::monotonic_us();
        std::int64_t last_frame_start = previous;
        while (!chosen)
        {
            load_wide_faces(); // two flags a frame; a list in another script arrives at any time
            const std::int64_t now = sys::monotonic_us();
            // Animation time is start-to-start (one full frame), and a hitch
            // must not teleport the animations.
            float dt =
                frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
            last_frame_start = now;
            if (dt > 0.05f)
                dt = 0.05f;
            std::size_t count = pad.read(samples);
            if (script.active())
            {
                // The script is the controller: one sample a frame, nothing else.
                const std::uint32_t buttons = script.step(dt, model, app);
                samples[0] = PadSample{};
                samples[0].buttons = buttons;
                samples[0].l2 = (buttons & pad_bits::kL2) != 0 ? 255 : 0;
                samples[0].r2 = (buttons & pad_bits::kR2) != 0 ? 255 : 0;
                samples[0].connected = true;
                samples[0].timestamp_us = static_cast<std::uint64_t>(now);
                count = 1;
            }
            InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                                    static_cast<std::uint64_t>(now));
            iptv_remote_enable_search(app.accepts_remote_search());
            iptv_remote_poll();
            if (iptv_remote_take_connected())
                app.phone_connected();
            if (app.settings().volume != settings->volume)
                app.set_volume(settings->volume);
            if (menu_volume != settings->volume)
            {
                menu_volume = settings->volume;
                mixer.set_master_gain(static_cast<float>(menu_volume) / 100.0f);
            }
            iptv_input_event_t remote_event;
            if (iptv_remote_next(&remote_event))
            {
                const InputFrame remote = ptv::remote_input(remote_event.action);
                input.pressed |= remote.pressed;
                input.held |= remote.held;
                if (remote.nav != Direction::none)
                    input.nav = remote.nav;
                input.connected = true;
                input.focus_lost = false;
            }
            char remote_query[IPTV_IME_MAX_TEXT_BYTES];
            if (iptv_remote_search(remote_query))
                app.remote_search(remote_query);
            if (count > 0)
                g_cross_held = samples[count - 1].connected &&
                               (samples[count - 1].buttons & pad_bits::kCross) != 0;

            feedback.clear();
            app.update(input, dt, feedback);
            if (app.take_pair_phone_requested())
                if (!iptv_remote_begin_pairing())
                    app.remote_notice("Phone pairing is unavailable");
            if (!app.pairing_open())
                iptv_remote_cancel_pairing();
            if (app.take_forget_phones_requested())
                app.remote_notice(iptv_remote_forget_phones() ? "Paired phones forgotten"
                                                           : "Could not forget phones. Try again.");
            app.set_pairing_info(iptv_remote_url(), iptv_remote_pairing_code(),
                                 iptv_remote_pairing_seconds(), iptv_remote_paired_count());
            for (const audio::CueEvent &event : feedback.cues)
                sounds.play(
                    mixer, event.set == audio::SoundSet::count ? audio::SoundSet::glass : event.set,
                    event);
            if (feedback.rumble_strength > 0.0f)
                pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
            pad.tick(dt);
            if (app.take_settings_changed())
            {
                if (!ptv::save_settings(tv::storage::config_dir(), app.settings()))
                {
                    sys::log("[TV] settings could not be saved");
                    app.set_volume(settings->volume);
                    app.remote_notice("Could not save settings. Try again.");
                }
                else
                {
                    *settings = app.settings();
                    iptv_native_set_volume(static_cast<unsigned>(settings->volume));
                    iptv_remote_set_volume(static_cast<unsigned>(settings->volume));
                }
            }
            chosen = model.take_play_request(request);

            app.draw(frame);
            renderer.begin();
            renderer.backdrop(frame.backdrop);
            renderer.draw(frame.scene);
            if (frame.glass)
                renderer.glass();
            renderer.draw(frame.overlay);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            renderer.present(0, display.width(), display.height());
            std::string capture_path = script.capture();
#if IPTV_REMOTE_CAPTURE
            if (frames % 30 == 0 && std::remove(tv_data_file("remote-capture.request")) == 0)
                capture_path = tv_data_file("remote-capture.bmp");
#endif
            if (!capture_path.empty())
            {
                // The same frame once more, into a small off-screen target:
                // reading the display surface back is slow.
                if (capture.texture() == 0)
                    capture.create(kCaptureWidth, kCaptureHeight, 1);
                capture.bind();
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                renderer.present(capture.framebuffer(), kCaptureWidth, kCaptureHeight);
                glBindFramebuffer(GL_FRAMEBUFFER, capture.framebuffer());
                const bool saved = save_picture(capture_path, kCaptureWidth, kCaptureHeight);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (!script.capture().empty())
                script.capture_done(saved);
                last_frame_start = sys::monotonic_us(); // saving is slow; the frame was not
            }
            if (!display.swap())
            {
                sys::log("[TV] fatal: swap failed frame=%llu error=%s",
                         static_cast<unsigned long long>(frames),
                         ps5::egl_error_name(display.last_error()));
                notify_failure("display swap");
                sys::park();
            }
            ++frames;
            const std::int64_t presented = sys::monotonic_us();
            if (frames == 1)
            {
                sys::log("[TV] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                         renderer.last_draw_calls());
                if (!g_splash_hidden)
                {
                    g_splash_hidden = true;
                    sys::log("[TV] splash hidden=%d", sys::hide_splash_screen() ? 1 : 0);
                }
                log_heap("first frame");
            }
            else
            {
                stats.add(static_cast<double>(presented - previous) / 1000.0);
            }
            previous = presented;
            if (stats.count() == 600)
            {
                char summary[160];
                stats.format(summary, sizeof(summary));
                sys::log("[TV] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                         renderer.last_instances());
                stats.reset();
            }
            if (app.wants_quit())
            {
                // The new version is staged and its helper waits for this
                // process to end: the app closes the way the system would
                // close it, and its files are replaced behind it.
                sys::log("[TV] closing: the update is staged");
                script.closing("the update is staged");
                model.close();
                audio_out.stop();
                pad.close();
                sys::quit();
            }
            if (script.wants_quit())
            {
                // A scripted run ends the app itself, the way the system
                // would close it: nothing is killed.
                sys::log("[TV] closing: the test script ended");
                model.close();
                audio_out.stop();
                pad.close();
                sys::quit();
            }
        }
        iptv_remote_enable_search(false);
        if (chosen)
            hand_over_to_channel(app, renderer, display, frame, request->channel_id,
                                 settings->reduced_motion);
        sys::log("[TV] menu closing frames=%llu channel=%s addresses=%zu",
                 static_cast<unsigned long long>(frames), request->channel_id.c_str(),
                 request->urls.size());
    }
    // Let the sound of the choice end before its port closes.
    for (int wait = 0; audio_ready && wait < 40 && mixer.active_voices() > 0; ++wait)
        sceKernelUsleep(10000);

    // Everything the menu holds goes before the player starts: its download,
    // the keyboard, the sound port, the controller, every GL object and
    // finally the display itself.
    model.close();
    audio_out.stop();
    pad.close();
    g_cross_held = false;
    renderer.release();
    display.close();
    log_heap("menu closed");
    sys::log("[TV] menu closed");
    return chosen;
}

struct PlaybackOutcome
{
    int result = -1;
    unsigned attempts = 0;
    unsigned selected = 0;
};

constexpr unsigned kAutotestMaxCandidates = 8;
constexpr unsigned kAutotestMaxCancelMs = 10u * 60u * 1000u;
// Where the app's data is (tv_paths.h): the pointers are settled in main.
const char *const kAutotestArchivePath = tv_data_file("iptv-autotest-receipts.txt");
const char *const kLatestReceiptPath = tv_data_file("iptv-last-receipt.txt");

void write_stdout(const char *text, std::size_t bytes)
{
    while (text != nullptr && bytes != 0)
    {
        const long written = write(1, text, bytes);
        if (written <= 0)
            return;
        text += static_cast<std::size_t>(written);
        bytes -= static_cast<std::size_t>(written);
    }
}

void write_autotest_marker(const char *archive_path, const char *marker, int marker_bytes)
{
    if (archive_path == nullptr || marker == nullptr || marker_bytes <= 0)
        return;
    const std::size_t bytes = std::strlen(marker);
    write_stdout(marker, bytes);
    if (std::FILE *archive = std::fopen(archive_path, "ab"))
    {
        std::fwrite(marker, 1, bytes, archive);
        std::fclose(archive);
    }
}

void append_autotest_receipt(const char *archive_path, const char *receipt_path)
{
    std::FILE *archive = std::fopen(archive_path, "ab");
    if (archive == nullptr)
        return;
    if (std::FILE *receipt = std::fopen(receipt_path, "rb"))
    {
        char chunk[1024]{};
        std::size_t bytes = 0;
        while ((bytes = std::fread(chunk, 1, sizeof(chunk), receipt)) != 0)
            std::fwrite(chunk, 1, bytes, archive);
        std::fclose(receipt);
    }
    else
    {
        std::fputs("IPTV_RECEIPT_MISSING\n", archive);
    }
    std::fclose(archive);
}

// Plays the channel's addresses in order until one opens.
PlaybackOutcome play_candidates(const ptv::PlayRequest &request, unsigned stop_after_ms,
                                const char *archive_path)
{
    PlaybackOutcome outcome{};
    for (std::size_t candidate = 0; candidate < request.urls.size(); ++candidate)
    {
        ++outcome.attempts;
        char marker[128]{};
        if (archive_path != nullptr)
        {
            const int marker_bytes =
                std::snprintf(marker, sizeof(marker), "IPTV_AUTOTEST_ATTEMPT_BEGIN candidate=%u\n",
                              static_cast<unsigned>(candidate + 1u));
            write_autotest_marker(archive_path, marker, marker_bytes);
            std::remove(kLatestReceiptPath);
        }
        outcome.result =
            stop_after_ms != 0
                ? iptv_player_run_controlled(request.urls[candidate].c_str(),
                                             request.channel_name.c_str(), stop_after_ms)
                : iptv_player_run_with_headers(
                      request.urls[candidate].c_str(), request.channel_name.c_str(),
                      request.user_agent.empty() ? nullptr : request.user_agent.c_str(),
                      request.referrer.empty() ? nullptr : request.referrer.c_str(),
                      request.reconnect_live ? 1 : 0);
        if (archive_path != nullptr)
        {
            append_autotest_receipt(archive_path, kLatestReceiptPath);
            const int marker_bytes = std::snprintf(
                marker, sizeof(marker), "IPTV_AUTOTEST_ATTEMPT_END candidate=%u result=%d\n",
                static_cast<unsigned>(candidate + 1u), outcome.result);
            write_autotest_marker(archive_path, marker, marker_bytes);
        }
        if (outcome.result >= 0)
        {
            outcome.selected = static_cast<unsigned>(candidate + 1u);
            break;
        }
    }
    return outcome;
}

#if TV_DEBUG_TRACE
// The debug build's record of one channel, appended to logs/debug-trace.txt:
// which channel, where its addresses point (without what identifies their
// owner), how it went, and everything the player wrote down about it. When it
// did not play, each address is asked once more for its first bytes, which say
// what kind of stream it really is.
void debug_trace_playback(const ptv::PlayRequest &request, const PlaybackOutcome &outcome,
                          long long seconds)
{
    static unsigned count = 0;
    const std::string path = tv::storage::logs_dir() + "/debug-trace.txt";
    std::FILE *out = std::fopen(path.c_str(), "a");
    if (out == nullptr)
    {
        sys::log("[TV] debug trace: cannot open %s", path.c_str());
        return;
    }
    if (count == 0)
        std::fprintf(out, "\n######## ProsperoTV debug trace: app started (built %s %s) ########\n",
                     __DATE__, __TIME__);
    std::fprintf(out, "\n==== channel %u of this launch, unix time %lld ====\n", ++count,
                 static_cast<long long>(std::time(nullptr)));
    std::fprintf(out, "name=%s\n", request.channel_name.c_str());
    std::fprintf(out, "result=%d  (1: stopped by the viewer, 0: ended, below 0: did not play)\n",
                 outcome.result);
    std::fprintf(out, "addresses=%zu tried=%u played=%u seconds=%lld\n", request.urls.size(),
                 outcome.attempts, outcome.selected, seconds);
    const char *reason = iptv_player_last_error();
    std::fprintf(out, "reason=%s\n", reason != nullptr ? reason : "");
    std::fprintf(out, "own user agent=%s own referrer=%s\n",
                 request.user_agent.empty() ? "no" : "yes",
                 request.referrer.empty() ? "no" : "yes");
    for (std::size_t i = 0; i < request.urls.size(); ++i)
        std::fprintf(out, "address %zu: %s\n", i + 1, ptv::redact_address(request.urls[i]).c_str());
    if (outcome.result < 0)
    {
        // The player has closed the network behind it.
        const iptv::http::Status network = ptv::platform::network_init();
        if (network != iptv::http::Status::ok)
            std::fprintf(out, "first bytes: the network did not start (%d)\n",
                         static_cast<int>(network));
        std::vector<char> head(4097);
        for (std::size_t i = 0;
             network == iptv::http::Status::ok && i < request.urls.size() && i < 3; ++i)
        {
            const iptv::http::FetchResult fetched = ptv::platform::fetch(
                request.urls[i].c_str(), head.data(), head.size(), 4096, nullptr);
            std::fprintf(
                out,
                         "first bytes of address %zu: fetch status=%d http=%d native=0x%08x bytes=%zu "
                         "-> %s\n",
                         i + 1, static_cast<int>(fetched.status), fetched.http_status,
                         static_cast<unsigned>(fetched.native_error), fetched.bytes,
                         ptv::describe_bytes(reinterpret_cast<const unsigned char *>(head.data()),
                                             fetched.bytes)
                             .c_str());
        }
        if (network == iptv::http::Status::ok)
            ptv::platform::network_shutdown();
        std::fputs(
            "(fetch status: 0 ok, 2 not an http/https address, 6 no connection, 7 the server "
                   "answered with an error, 8 more than the 4096 bytes asked for, which is normal "
                   "for a stream, 9 read failed, 10 too slow)\n",
                   out);
    }
    std::string receipt;
    if (save::read_file(kLatestReceiptPath, &receipt, 65536))
    {
        std::fputs("---- the player's receipt ----\n", out);
        std::fwrite(receipt.data(), 1, receipt.size(), out);
        if (!receipt.empty() && receipt.back() != '\n')
            std::fputc('\n', out);
    }
    else
    {
        std::fputs("---- the player left no receipt ----\n", out);
    }
    std::fclose(out);
    sys::log("[TV] debug trace: channel %u written to %s", count, path.c_str());
}
#endif

char *trim_field(char *field)
{
    while (field != nullptr && (*field == ' ' || *field == '\t'))
        ++field;
    if (field == nullptr)
        return nullptr;
    char *end = field + std::strlen(field);
    while (end != field && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return field;
}

struct AutotestCase
{
    char *urls[kAutotestMaxCandidates]{};
    unsigned count = 0;
    unsigned cancel_ms = 0;
};

bool parse_autotest_case(char *line, AutotestCase *test)
{
    if (line == nullptr || test == nullptr)
        return false;
    *test = {};
    bool first_field = true;
    for (char *field = line; field != nullptr;)
    {
        char *next = std::strchr(field, '\t');
        if (next != nullptr)
            *next++ = '\0';
        char *value = trim_field(field);
        if (value == nullptr || *value == '\0')
            return false;
        static constexpr char kCancelPrefix[] = "@cancel-ms=";
        if (first_field && std::strncmp(value, kCancelPrefix, sizeof(kCancelPrefix) - 1u) == 0)
        {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(value + sizeof(kCancelPrefix) - 1u, &end, 10);
            if (end == nullptr || *end != '\0' || parsed == 0 || parsed > kAutotestMaxCancelMs)
                return false;
            test->cancel_ms = static_cast<unsigned>(parsed);
        }
        else
        {
            if (test->count == kAutotestMaxCandidates)
                return false;
            test->urls[test->count++] = value;
        }
        first_field = false;
        field = next;
    }
    return test->count != 0;
}

// The controlled acceptance run of test builds: plays the addresses listed in
// iptv-autotest.txt beside the app before the menu opens and keeps their
// receipts.
bool run_autotest_if_present()
{
    std::FILE *file = std::fopen(tv::storage::app_file("iptv-autotest.txt").c_str(), "rb");
    if (file == nullptr)
        return false;
    if (std::FILE *archive = std::fopen(kAutotestArchivePath, "wb"))
    {
        std::fputs("IPTV_AUTOTEST_RECEIPTS_V2\n", archive);
        std::fclose(archive);
    }

    char line[4097]{};
    unsigned test_index = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        std::size_t bytes = std::strlen(line);
        const bool oversized = bytes == sizeof(line) - 1u && bytes != 0 && line[bytes - 1u] != '\n';
        if (oversized)
        {
            int discarded = 0;
            while ((discarded = std::fgetc(file)) != EOF && discarded != '\n')
            {
            }
        }
        while (bytes != 0 && (line[bytes - 1u] == '\r' || line[bytes - 1u] == '\n' ||
                              line[bytes - 1u] == ' ' || line[bytes - 1u] == '\t'))
            line[--bytes] = '\0';
        char *content = line;
        while (*content == ' ' || *content == '\t')
            ++content;
        if (*content == '\0' || *content == '#')
            continue;
        ++test_index;
        AutotestCase test{};
        const bool parsed = !oversized && parse_autotest_case(content, &test);
        char marker[192]{};
        int marker_bytes = std::snprintf(
            marker, sizeof(marker), "IPTV_AUTOTEST_BEGIN index=%u candidates=%u cancel_ms=%u\n",
            test_index, test.count, test.cancel_ms);
        write_autotest_marker(kAutotestArchivePath, marker, marker_bytes);
        PlaybackOutcome outcome{};
        if (parsed)
        {
            ptv::PlayRequest request{};
            char name[64]{};
            std::snprintf(name, sizeof(name), "controlled acceptance %u", test_index);
            request.channel_name = name;
            request.urls.reserve(test.count);
            for (unsigned candidate = 0; candidate < test.count; ++candidate)
                request.urls.emplace_back(test.urls[candidate]);
            outcome = play_candidates(request, test.cancel_ms, kAutotestArchivePath);
        }
        marker_bytes =
            std::snprintf(marker, sizeof(marker),
                          "IPTV_AUTOTEST_END index=%u result=%d attempts=%u selected=%u\n",
                          test_index, outcome.result, outcome.attempts, outcome.selected);
        write_autotest_marker(kAutotestArchivePath, marker, marker_bytes);
        sceKernelUsleep(250000);
    }
    std::fclose(file);
    return test_index != 0;
}

} // namespace

// The keyboard waits for Cross to be let go before it opens, so the press
// that asked for it is not typed into it.
extern "C" bool iptv_ime_confirm_held(void)
{
    return g_cross_held;
}

int main()
{
    sys::log("[TV] entry");

    // The system modules the app will ask for, loaded before anything else.
    // With filesystem access the process no longer sees the sandbox the
    // system loader resolves modules in, and once the OpenGL runtime has
    // started the video decoder's module no longer loads either (0x80020016 on
    // hardware). Loaded first, they stay for the life of the process, and the
    // player's and the keyboard's own loads and unloads only count. The
    // decoders load parts of themselves at their first use: they are asked
    // now, so those parts are there too.
    std::int32_t decoders[6] = {};
    iptv_native_backend_warm(decoders);
    const int dialogs = sceCommonDialogInitialize();
    const int keyboard = sceSysmoduleLoadModule(kKeyboardModule);

    // Filesystem access, and with it where every file of the app is. It must
    // be asked for while the process has a single thread: nothing above or in
    // it starts one.
    tv::storage::initialize();
    iptv_remote_set_pairing_store((tv::storage::config_dir() + "/phone-pairing-v1.txt").c_str());
    iptv_remote_set_icon(tv::storage::app_file("sce_sys/icon0.png").c_str());
    iptv_remote_start(8888);
    // Said after it: the log moved with the app's data.
    sys::log("[TV] modules videodec2=0x%08x compute=0x%08x h264=0x%08x hevc=0x%08x vp9=0x%08x "
             "audiodec=0x%08x dialogs=0x%08x keyboard=0x%08x",
             static_cast<unsigned>(decoders[0]), static_cast<unsigned>(decoders[1]),
             static_cast<unsigned>(decoders[2]), static_cast<unsigned>(decoders[3]),
             static_cast<unsigned>(decoders[4]), static_cast<unsigned>(decoders[5]),
             static_cast<unsigned>(dialogs), static_cast<unsigned>(keyboard));

    // Acceptance fixtures are opt-in test builds. Production must ignore a
    // stale iptv-autotest.txt left in an already-mounted development folder.
    if (IPTV_AUTOTEST_ENABLED != 0)
        (void)run_autotest_if_present();

    static ptv::Model model(tv::storage::config_dir(), tv::storage::cache_dir());
    ptv::Settings settings = ptv::load_settings(tv::storage::config_dir());
    iptv_native_set_volume(static_cast<unsigned>(settings.volume));
    iptv_remote_set_volume(static_cast<unsigned>(settings.volume));
    iptv_remote_set_volume_handler(
        [](unsigned volume, void *context) -> bool
        {
            auto &current = *static_cast<ptv::Settings *>(context);
            ptv::Settings next = current;
            next.volume = static_cast<int>(volume);
            if (!ptv::save_settings(tv::storage::config_dir(), next))
                return false;
            current = next;
            iptv_native_set_volume(volume);
            return true;
        }, &settings);
    // A request a PC left beside the test title turns this launch into a
    // scripted run (see tv_dev.hpp). Release builds never read one.
    static tv_dev::Script script;
    static const std::string dev_dir = tv::storage::logs_dir() + "/dev";
    if (TV_DEV_SCRIPTS != 0 && script.load(tv::storage::app_file("dev/request.txt"), dev_dir))
    {
        sys::log("[TV] scripted run: the controller is not read");
        // dev/force-field-blend.txt: time the interlaced blend with any channel.
        std::string unused;
        if (save::read_file(tv::storage::app_file("dev/force-field-blend.txt"), &unused, 64))
        {
            iptv_native_backend_force_field_blend(1);
            sys::log("[TV] every picture is blended as if it were interlaced");
        }
        // Pictures of the tuning screen as the television showed it.
        tv_tuning_set_dump_dir(dev_dir.c_str());
    }
    tv::start_update_check();
    LastPlayback last;
    for (;;)
    {
        ptv::PlayRequest request;
        if (!run_menu(model, &settings, last, &request, script))
            sys::park();

        // The menu's display and the player's must never overlap: give the
        // closed one a moment to let go.
        sceKernelUsleep(100000);
        const std::int64_t started = sys::monotonic_us();
        // The menu is closed while playing; keep Favorite tied to the channel
        // being watched, even if removing it changes the favorites list.
        struct PlaybackFavorite
        {
            ptv::Model &model;
            unsigned index;
        } favorite{model, model.channel_count()};
        for (unsigned index = 0; index < model.channel_count(); ++index)
            if (model.channel(index).id == request.channel_id)
            {
                favorite.index = index;
                break;
            }
        iptv_remote_set_playback_favorite(
            [](void *context) -> int
            {
                auto &current = *static_cast<PlaybackFavorite *>(context);
                const auto result = current.model.toggle_favorite(current.index);
                return result == ptv::Model::Starred::failed ? -1
                       : result == ptv::Model::Starred::added ? 1 : 0;
            },
            &favorite);
        // A scripted run plays each channel for a set time; the player stops it.
        const PlaybackOutcome outcome = play_candidates(request, script.watch_ms(), nullptr);
        iptv_remote_set_playback_favorite(nullptr, nullptr);
        const long long seconds = (sys::monotonic_us() - started) / 1000000;
        // 1: the viewer stopped it; 0: it ended; below 0: it did not play, and why.
        const char *reason = outcome.result < 0 ? iptv_player_last_error() : nullptr;
        sys::log("[TV] playback result=%d attempts=%u selected=%u seconds=%lld%s%s%s",
                 outcome.result, outcome.attempts, outcome.selected, seconds,
                 reason != nullptr && reason[0] != '\0' ? " reason=\"" : "",
                 reason != nullptr && reason[0] != '\0' ? reason : "",
                 reason != nullptr && reason[0] != '\0' ? "\"" : "");
#if TV_DEBUG_TRACE
        debug_trace_playback(request, outcome, seconds);
#endif
        if (script.active())
        {
            script.note("played \"%s\" result=%d attempts=%u selected=%u seconds=%lld",
                        request.channel_name.c_str(), outcome.result, outcome.attempts,
                        outcome.selected, seconds);
            // What the player wrote down about it: a few lines in the report,
            // the whole of it beside the pictures.
            static unsigned played = 0;
            std::string receipt;
            if (save::read_file(kLatestReceiptPath, &receipt, 65536))
            {
                char name[200];
                std::snprintf(name, sizeof(name), "%s/receipt-%02u.txt", dev_dir.c_str(), ++played);
                save::write_atomic(name, receipt);
                int lines = 0;
                for (std::size_t from = 0; from < receipt.size() && lines < 14; ++lines)
                {
                    std::size_t to = receipt.find('\n', from);
                    if (to == std::string::npos)
                        to = receipt.size();
                    script.note("receipt %s", receipt.substr(from, to - from).c_str());
                    from = to + 1;
                }
            }
        }
        tv_tuning_clear();
        const iptv::StoreStatus history =
            iptv::RecordPlaybackResult(iptv::kDefaultPlaybackHistoryPath, request.source_id,
                                       request.channel_id, outcome.result >= 0, outcome.result);
        if (history != iptv::StoreStatus::ok)
            sys::log("[TV] history channel=%s result=%d store=%u", request.channel_id.c_str(),
                     outcome.result, static_cast<unsigned>(history));
        last = {};
        last.result = outcome.result;
        if (outcome.result < 0)
        {
            last.channel_id = request.channel_id;
            last.channel_name = request.channel_name;
            last.attempts = outcome.attempts;
        }
        sceKernelUsleep(100000);
    }
}
