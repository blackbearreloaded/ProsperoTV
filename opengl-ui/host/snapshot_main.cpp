// ProsperoTV - Headless PC renderer: plays the interface and writes PNG files.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: tv_snapshots <fonts dir> <playlist.m3u> <output dir> [width height]
//
// Runs the same interface code as the console build against the PC's stand-in
// keyboard and network, through Mesa's surfaceless EGL, with a fixed 60 Hz
// clock. A script of controller inputs walks through every screen and state;
// the frames it names are written as pictures. A picture from here shows what
// the code draws. It is not a console result.

#include "core/save_file.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "host_platform.hpp"
#include "host_preview.hpp"
#include "tv/app.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using hui::Action;
using hui::Direction;

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data, 64u << 20) || !font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// One step of the walk: an optional change to the stand-in console, one frame
// of input, a wait, and then perhaps a picture.
struct Step
{
    float wait = 0.5f;
    std::uint32_t press = 0;
    Direction nav = Direction::none;
    const char *capture = nullptr;
    void (*before)() = nullptr;
    bool hold = false; // the button stays down for the whole wait
};

constexpr std::uint32_t bit(Action action)
{
    return hui::action_bit(action);
}

Step look(float wait, const char *capture)
{
    return {wait, 0, Direction::none, capture, nullptr};
}
Step press(Action action, float wait = 0.35f, const char *capture = nullptr)
{
    return {wait, bit(action), Direction::none, capture, nullptr};
}
Step move(Direction direction, float wait = 0.3f, const char *capture = nullptr)
{
    return {wait, 0, direction, capture, nullptr};
}
Step hold(Action action, float wait, const char *capture = nullptr)
{
    return {wait, bit(action), Direction::none, capture, nullptr, true};
}
Step change(void (*before)(), float wait = 0.5f, const char *capture = nullptr)
{
    return {wait, 0, Direction::none, capture, before};
}

std::string g_playlist;
std::string g_roadmap_playlist, g_roadmap_guide, g_roadmap_logo;
ptv::Model *g_model = nullptr;

const Step kWalk[] = {
    // ---- the first launch: nothing saved yet ----
    look(0.5f, "01-first-download"),
    look(1.4f, "02-live-tv"),
    press(Action::touch, 0.6f, "42-provider-categories"),
    move(Direction::down),
    press(Action::west, 0.5f, "43-category-hidden"),
    press(Action::west),
    press(Action::back),
    // ---- Live TV ----
    move(Direction::right, 0.16f, "03-live-tv-moving"),
    look(0.7f, "04-live-tv-focus"),
    move(Direction::right, 0.25f),
    press(Action::west, 0.9f, "05-favorite-added"),
    move(Direction::down, 0.3f),
    move(Direction::right, 0.3f),
    press(Action::west, 0.4f),
    press(Action::jump_next, 0.3f),
    press(Action::jump_next, 0.3f),
    press(Action::jump_next, 0.9f, "06-live-tv-scrolled"),
    press(Action::back, 0.8f),
    // ---- the letters: Right from the last column, then up and down ----
    move(Direction::right, 0.2f),
    move(Direction::right, 0.2f),
    move(Direction::right, 0.2f),
    move(Direction::right, 0.3f),
    move(Direction::right, 0.7f, "28-letters"),
    move(Direction::down, 0.2f),
    move(Direction::down, 0.2f),
    move(Direction::down, 0.2f),
    move(Direction::down, 0.2f),
    move(Direction::down, 0.2f),
    move(Direction::down, 0.9f, "29-letter-chosen"),
    move(Direction::left, 0.7f, "30-letter-channels"),
    // ---- R2 held: the pages keep turning ----
    hold(Action::jump_next, 2.4f, "31-pages-held"),
    press(Action::back, 0.8f),
    // ---- the lists of Live TV ----
    move(Direction::up, 0.5f, "07-lists-focused"),
    move(Direction::right, 0.3f),
    move(Direction::right, 0.9f, "08-news"),
    move(Direction::left, 0.4f),
    look(0.6f, "09-recent-empty"),
    press(Action::back, 0.8f),
    move(Direction::down, 0.4f),
    // ---- the search drawer ----
    press(Action::north, 0.9f, "10-search"),
    change([]() { host::set_keyboard_text("sport"); }, 0.1f),
    press(Action::confirm, 0.9f, "11-search-typed"),
    move(Direction::down, 0.3f),
    press(Action::confirm, 0.7f, "12-search-countries"),
    move(Direction::down, 0.2f),
    move(Direction::down, 0.2f),
    press(Action::confirm, 0.8f),
    move(Direction::down, 0.25f),
    move(Direction::down, 0.25f),
    move(Direction::down, 0.3f),
    move(Direction::right, 0.25f),
    move(Direction::right, 0.25f),
    move(Direction::right, 0.8f, "13-search-filters"),
    move(Direction::down, 0.3f),
    press(Action::confirm, 1.0f, "14-search-results"),
    press(Action::north, 0.7f),
    move(Direction::up, 0.25f),
    move(Direction::up, 0.25f),
    move(Direction::up, 0.25f),
    move(Direction::up, 0.25f),
    change([]() { host::set_keyboard_text("zzzz"); }, 0.1f),
    press(Action::confirm, 0.6f),
    press(Action::back, 0.9f, "15-no-match"),
    press(Action::back, 1.0f, "16-search-cleared"),
    // ---- Favorites ----
    press(Action::page_next, 1.1f, "17-favorites"),
    change(
        []()
        {
            g_model->create_folder("Football");
            g_model->create_folder("Kids");
        }),
    press(Action::touch, 0.6f, "44-favorite-folders"),
    press(Action::back),
    press(Action::west, 0.4f),
    press(Action::west, 1.0f, "18-favorites-empty"),
    // ---- Sources ----
    press(Action::page_next, 1.1f, "19-sources"),
    move(Direction::down, 0.7f, "20-sources-playlist"),
    move(Direction::down, 0.7f, "21-sources-account"),
    move(Direction::up, 0.3f),
    move(Direction::up, 0.3f),
    change([]() { host::set_network(true, g_playlist, 2500); }, 0.1f),
    press(Action::menu, 1.0f, "22-sources-updating"),
    look(2.6f, "23-sources-updated"),
    // ---- Settings ----
    press(Action::page_next, 0.7f, "53-vod-no-account"),
    press(Action::page_next, 1.1f, "24-settings"),
    press(Action::confirm, 0.5f),
    move(Direction::down, 0.25f),
    move(Direction::down, 0.7f, "25-settings-changed"),
    move(Direction::up, 0.2f),
    move(Direction::up, 0.2f),
    press(Action::confirm, 0.4f),
    // The end of the page: Troubleshooting and its switch, then back to the top.
    move(Direction::down, 0.15f),
    move(Direction::down, 0.15f),
    move(Direction::down, 0.15f),
    move(Direction::down, 0.15f),
    move(Direction::down, 0.15f),
    move(Direction::down, 0.15f),
    move(Direction::down, 0.15f),
    move(Direction::down, 0.15f),
    move(Direction::down, 0.7f, "25a-settings-diagnostic-log"),
    move(Direction::down, 0.15f),
    move(Direction::down, 0.7f, "45-settings-playback"),
    move(Direction::down, 0.7f, "62-settings-live-preview"),
    // Turned on: the page says so at the bottom, and so does every other one.
    press(Action::confirm, 0.7f, "25b-settings-diagnostic-log-on"),
    press(Action::confirm, 0.4f),
    move(Direction::up, 0.15f),
    move(Direction::up, 0.15f),
    move(Direction::up, 0.15f),
    move(Direction::up, 0.15f),
    move(Direction::up, 0.15f),
    move(Direction::up, 0.15f),
    move(Direction::up, 0.15f),
    move(Direction::up, 0.15f),
    move(Direction::up, 0.3f),
    // ---- About ----
    press(Action::page_next, 1.1f, "32-about"),
    // ---- the states that are hard to reach on purpose ----
    press(Action::back, 0.9f),
    change(
        []()
        {
            const iptv::ChannelView channel = g_model->channel(g_model->visible(3));
            g_model->report_playback_failure(channel.id.data(), channel.name.data(), -5, 2,
                                             "The stream did not answer.");
        },
        0.9f, "26-channel-failed"),
    press(Action::back, 0.8f),
    change([]() { host::set_network(false, "", 400); }, 0.1f),
    press(Action::menu, 1.6f, "27-update-failed"),
    look(4.0f, nullptr),
    change(
        []()
        {
            host::set_unix_time(
                static_cast<std::uint64_t>(ptv::xmltv_time("20261008163000 +0000")));
            host::set_network(true, g_roadmap_playlist, 100);
            host::set_network_response("https://guide.example.invalid/demo.xml", g_roadmap_guide);
            host::set_network_response("https://logos.example.invalid/demo.png", g_roadmap_logo);
            g_model->refresh();
        },
        2.0f, "46-now-next"),
    press(Action::touch, 0.6f, "47-category-countries"),
    move(Direction::down),
    press(Action::confirm, 0.6f, "48-category-children"),
    move(Direction::down),
    move(Direction::down),
    press(Action::confirm, 0.6f, "49-category-grandchildren"),
    press(Action::back),
    press(Action::back),
    press(Action::back),
    press(Action::r3, 0.6f, "50-programme-guide"),
    move(Direction::left, 0.6f, "51-catch-up-guide"),
    press(Action::back),
    look(2.0f, "52-channel-logos"),
    change([]() { g_model->edit_saved_source(3); }, 0.2f),
    change([]() { host::set_keyboard_text("https://provider.example.invalid"); }, 0.2f),
    change([]() { host::set_keyboard_text("viewer"); }, 0.2f),
    change([]() { host::set_keyboard_text("demo"); }, 2.0f),
    press(Action::page_next),
    press(Action::page_next, 0.7f, "54-sources-with-portals"),
    press(Action::page_next, 0.7f, "55-vod-home"),
    press(Action::confirm, 2.0f, "56-vod-categories"),
    move(Direction::down),
    press(Action::confirm, 0.5f, "57-vod-child-categories"),
    press(Action::confirm, 1.2f, "58-vod-movies"),
    press(Action::back),
    press(Action::back),
    press(Action::back),
    move(Direction::down),
    press(Action::confirm, 1.2f),
    press(Action::confirm, 1.2f, "59-vod-shows"),
    press(Action::confirm, 1.2f, "60-vod-seasons"),
    move(Direction::down),
    press(Action::confirm),
    press(Action::confirm, 0.8f, "61-vod-episodes"),
    change([]() { g_model->use_saved_source(1); }, 0.7f),
    press(Action::back, 0.7f),
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <fonts dir> <playlist.m3u> <output dir> [width height]\n",
                     argv[0]);
        return 2;
    }
    const std::string fonts_dir = argv[1];
    g_playlist = argv[2];
    const std::string output = argv[3];
    const int width = argc > 5 ? std::atoi(argv[4]) : 1920;
    const int height = argc > 5 ? std::atoi(argv[5]) : 1080;

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    std::fprintf(stderr, "GL %s / %s\n", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    hui::gfx::set_glsl_prefix("#version 450 core\n");

    hui::gfx::Renderer renderer;
    hui::gfx::Font regular;
    hui::gfx::Font semibold;
    hui::gfx::Font display;
    hui::gfx::Font mono;
    hui::gfx::Font east_asian;
    hui::gfx::Font korean;
    hui::ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, fonts_dir + "/inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, fonts_dir + "/inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, fonts_dir + "/montserrat-medium.huifont", &display, &fonts.display) ||
        !load_font(renderer, fonts_dir + "/dejavu-sans-mono.huifont", &mono, &fonts.mono))
        return 1;
    // Dusk uses four faces; the two slots left are for the scripts channel
    // names need.
    fonts.pixel = fonts.mono;
    fonts.hand = fonts.regular;
    (void)load_font(renderer, fonts_dir + "/noto-sans-east-asian.huifont", &east_asian,
                    &fonts.hand);
    (void)load_font(renderer, fonts_dir + "/noto-sans-korean.huifont", &korean, &fonts.pixel);

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;

    // A console that has never run the app: an empty data folder.
    hui::save::ensure_directory(output);
    const std::string data_dir = output + "/data";
    hui::save::ensure_directory(data_dir);
    for (const char *name :
         {"prosperotv-catalog.sqlite3", "prosperotv-custom-catalog.sqlite3",
          "prosperotv-xtream-catalog.sqlite3", "prosperotv-playback-history.sqlite3",
          "iptv-favorites-v1.bin", "iptv-history-v1.bin", "iptv-custom-source-v1.txt",
          "iptv-active-source-v1.txt", "prosperotv-xtream-v1.txt", "prosperotv-interface-v1.txt"})
        std::remove((data_dir + "/" + name).c_str());
    std::remove((data_dir + "/prosperotv-library.sqlite3").c_str());
    std::remove((data_dir + "/prosperotv-catalog.sqlite3.guide.sqlite3").c_str());
    g_roadmap_playlist = output + "/roadmap.m3u";
    g_roadmap_guide = output + "/roadmap.xml";
    g_roadmap_logo = output + "/test-logo.png";
    {
        std::ofstream playlist(g_roadmap_playlist), xml(g_roadmap_guide);
        playlist << "#EXTM3U x-tvg-url=\"https://guide.example.invalid/demo.xml\" "
                    "catchup=\"default\" catchup-days=\"7\"\n";
        xml << "<tv>";
        constexpr const char *shows[] = {"Football: Afternoon match",
                                         "Around the world",
                                         "The big interview",
                                         "Basketball tonight",
                                         "Family favourites",
                                         "Evening report",
                                         "At the movies"};
        for (int i = 0; i < 14; ++i)
        {
            playlist
                << "#EXTINF:-1 tvg-logo=\"https://logos.example.invalid/demo.png\" tvg-id=\"demo"
                << i << "\" group-title=\"US / "
                << (i == 13 ? "News"
                    : i % 2 ? "Sports / Basketball"
                            : "Sports / Football")
                << "\" catchup-source=\"https://archive.example.invalid/" << i
                << "?start={utc}&end={utcend}\",Channel " << (i < 10 ? "0" : "") << i
                << "\nhttps://stream.example.invalid/" << i << ".ts\n";
            for (int hour = 14; hour < 22; ++hour)
                xml << "<programme channel=\"demo" << i << "\" start=\"20261008" << hour
                    << "0000 +0000\" stop=\"20261008" << hour + 1 << "0000 +0000\"><title>"
                    << shows[(i + hour) % 7]
                    << "</title><desc>Coverage, interviews and highlights from today's "
                       "events.</desc></programme>";
        }
        xml << "</tv>";
    }

    host::reset();
    host::set_network(true, g_playlist, 600);
    {
        const iptv::XtreamCredentials account{"https://provider.example.invalid", "viewer", "demo"};
        int number = 0;
        const auto reply = [&](const std::string &url, std::string_view body)
        {
            const auto file = output + "/vod-fixture-" + std::to_string(number++) + ".json";
            std::ofstream(file) << body;
            host::set_network_response(url, file);
        };
        const auto response = [&](std::string_view action, std::string_view body)
        {
            std::string url;
            iptv::BuildXtreamApiUrl(account, action, &url);
            reply(url, body);
        };
        response("", R"({"user_info":{"auth":1,"status":"Active"}})");
        response("get_live_categories", R"([{"category_id":1,"category_name":"US / News"}])");
        response(
            "get_live_streams",
            R"([{"stream_id":1,"name":"Channel 1","category_id":1},{"stream_id":2,"name":"Channel 2","category_id":1},{"stream_id":3,"name":"Channel 3","category_id":1}])");
        response(
            "get_vod_categories",
            R"([{"category_id":1,"category_name":"US"},{"category_id":2,"category_name":"Drama","parent_id":1},{"category_id":3,"category_name":"Family","parent_id":1}])");
        response(
            "get_vod_streams",
            R"([{"stream_id":10,"name":"A journey through the valley","container_extension":"mp4","category_id":2,"stream_icon":"https://logos.example.invalid/demo.png"},{"stream_id":11,"name":"Midnight on the coast","container_extension":"mkv","category_id":2},{"stream_id":12,"name":"The little astronomer","container_extension":"mp4","category_id":3}])");
        response("get_series_categories", R"([{"category_id":4,"category_name":"Documentaries"}])");
        response(
            "get_series",
            R"([{"series_id":20,"name":"Our changing planet","category_id":4,"cover":"https://logos.example.invalid/demo.png"}])");
        std::string series;
        iptv::BuildXtreamSeriesUrl(account, "20", &series);
        reply(
            series,
            R"({"episodes":{"1":[{"id":200,"title":"The world beneath our feet","episode_num":1,"container_extension":"mp4"},{"id":201,"title":"Ocean currents","episode_num":2,"container_extension":"mp4"},{"id":202,"title":"Following the seasons","episode_num":3,"container_extension":"mp4"}],"2":[{"id":210,"title":"A new chapter","episode_num":1,"container_extension":"mp4"}]}})");
    }
    ptv::Model model(data_dir);
    g_model = &model;
    model.open();
    ptv::App app(model, fonts, renderer.glass_texture(), ptv::load_settings(data_dir),
                 "host build");
    app.configure_images(
        [&renderer](const ptv::ImagePixels &pixels) {
            return renderer.batch().create_texture(pixels.width, pixels.height, pixels.rgba.data());
        },
        [](std::uint32_t texture) { glDeleteTextures(1, &texture); });
    app.configure_preview(
        [&renderer](std::uint32_t texture, const ptv::ImagePixels &pixels)
        {
            if (!texture)
                return renderer.batch().create_texture(pixels.width, pixels.height,
                                                       pixels.rgba.data());
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pixels.width, pixels.height, GL_RGBA,
                            GL_UNSIGNED_BYTE, pixels.rgba.data());
            return texture;
        },
        [](std::uint32_t texture) { glDeleteTextures(1, &texture); });

    ptv::Frame frame;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    stbi_flip_vertically_on_write(1);
    bool ok = true;
    // Renders the frame as it was last recorded.
    const auto render_drawn = [&](const std::string &name)
    {
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass)
            renderer.glass();
        renderer.draw(frame.overlay);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = output + "/" + name + ".png";
        ok = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
        std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n", path.c_str(),
                     renderer.last_instances(), renderer.last_draw_calls(), glGetError());
    };
    const auto render = [&](const std::string &name)
    {
        app.draw(frame);
        render_drawn(name);
    };

    constexpr float kDt = 1.0f / 60.0f;
    hui::ui::Feedback feedback;
    long frames = 0;
    for (const Step &step : kWalk)
    {
        if (step.before != nullptr)
            step.before();
        const int count = std::max(1, static_cast<int>(step.wait / kDt + 0.5f));
        for (int i = 0; i < count; ++i, ++frames)
        {
            hui::InputFrame input;
            input.connected = true;
            if (i == 0)
            {
                input.pressed = step.press;
                input.held = step.press;
                input.nav = step.nav;
            }
            else if (step.hold)
            {
                input.held = step.press;
            }
            feedback.clear();
            app.update(input, kDt, feedback);
            // The stand-in download runs in real time on its own thread: one
            // simulated frame takes a real one, so both clocks agree.
            std::this_thread::sleep_for(std::chrono::microseconds(16667));
        }
        if (step.capture != nullptr)
            render(step.capture);
    }
    // A synthetic moving picture exercises the same preview texture path.
    host::set_preview(true);
    app.stop_preview();
    for (int i = 0; i < 120; ++i)
    {
        hui::InputFrame input;
        app.update(input, kDt, feedback);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    render("63-live-preview");
    app.stop_preview();
    host::set_preview(false);
    {
        // The opening, at six of its moments.
        const std::pair<float, const char *> moments[] = {
            {0.50f, "00a-intro-set"}, {0.80f, "00b-intro-line"}, {1.08f, "00c-intro-opening"},
            {2.00f, "00d-intro-on"},  {2.72f, "00e-intro-into"}, {2.98f, "00f-intro-through"}};
        for (const auto &[seconds, name] : moments)
        {
            app.set_intro_time(seconds);
            render(name);
        }
        app.set_intro_time(-1.0f);
    }
    // The tuning screen a channel opens on: halfway through the hand-over,
    // and as the player shows it with its bar part filled.
    {
        const std::string id(g_model->channel(g_model->visible(2)).id);
        app.draw_tuning(frame, id, 0.5f);
        render_drawn("33-tuning-handover");
        app.draw_tuning(frame, id, 1.0f, 0.62f);
        render_drawn("34-tuning");
    }
    // The update: offered, downloading, unpacking, ready to close, and failed.
    {
        const auto run =
            [&](int count, std::uint32_t press = 0, hui::Direction nav = hui::Direction::none)
        {
            for (int i = 0; i < count; ++i)
            {
                hui::InputFrame input;
                input.connected = true;
                if (i == 0)
                {
                    input.pressed = press;
                    input.held = press;
                    input.nav = nav;
                }
                feedback.clear();
                app.update(input, kDt, feedback);
            }
        };
        const auto at = [](ptv::platform::UpdatePhase phase, std::uint64_t done,
                           std::uint64_t total, const char *left = "")
        {
            ptv::platform::UpdateProgress progress;
            progress.phase = phase;
            progress.done = done;
            progress.total = total;
            progress.time_left = left;
            return progress;
        };
        using ptv::platform::UpdatePhase;
        const std::uint32_t cross = hui::action_bit(hui::Action::confirm);
        ptv::platform::UpdateOffer offer;
        offer.installable = true;
        offer.version = "01.000.020";
        offer.installed = "01.000.015";
        offer.available = "01.000.020";
        offer.size = 41u * 1024u * 1024u;
        offer.notes =
            "Highlights\n"
            "- The alphabet beside every list: Right from the last column, then up and down.\n"
            "- Hold L2 or R2 and the pages keep turning.\n"
            "- A tuning screen from Cross to the channel's first picture.\n"
            "\n"
            "Warning: this version moves your sources and favorites to /data/prosperotv the first "
            "time it starts.\n"
            "\n"
            "Fixes\n"
            "- Channels play again after the menu has been drawn with OpenGL.\n"
            "- Greek channel names read as written.\n"
            "- The launch picture stays until the menu is there.\n"
            "- The player's messages no longer appear as notifications.\n"
            "\n"
            "Note: the update keeps everything you saved.\n"
            "\n"
            "Thanks\n"
            "To everyone who tested the new interface on their console and wrote back with what "
            "they saw, "
            "and to the maintainers of the public channel list.\n"
            "- More languages for channel names are next.\n"
            "- So is a way to sort a list by country.\n"
            "- And the guide, where a source provides one.";
        offer.notes_truncated = true;
        host::offer_update(offer);
        run(8);
        render("35-update-arriving");
        run(70);
        render("36-update-offer");
        // What's new: the release notes, then further down, then back.
        run(20, 0, hui::Direction::right);
        run(60, cross);
        render("36a-update-notes");
        run(10, 0, hui::Direction::down);
        run(10, 0, hui::Direction::down);
        run(50, 0, hui::Direction::down);
        render("36b-update-notes-scrolled");
        run(40, hui::action_bit(hui::Action::back));
        run(30, 0, hui::Direction::left);
        host::set_update_progress(at(UpdatePhase::starting, 0, 0));
        run(40, cross);
        render("37-update-starting");
        host::set_update_progress(
            at(UpdatePhase::downloading, 17u << 20, 41u << 20, "about 12 s left"));
        run(90);
        render("38-update-downloading");
        host::set_update_progress(at(UpdatePhase::unpacking, 52u << 20, 96u << 20));
        run(90);
        render("39-update-unpacking");
        host::set_update_progress(at(UpdatePhase::ready, 0, 0));
        run(50);
        render("40-update-ready");
        // And when it does not work.
        host::reset();
        host::offer_update(offer);
        ptv::platform::UpdateProgress broken = at(UpdatePhase::failed, 0, 0);
        broken.error = "The download stopped before the end.";
        host::set_update_progress(broken);
        run(200);
        run(60, cross);
        render("41-update-failed");
    }
    std::fprintf(stderr, "walk: %ld frames simulated\n", frames);
    model.close();
    return ok ? 0 : 1;
}
