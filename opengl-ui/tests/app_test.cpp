// ProsperoTV - Tests of the interface: it is driven like a controller would, without OpenGL.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/save_file.hpp"
#include "host_platform.hpp"
#include "large_list.hpp"
#include "tv/app.hpp"
#include "tv/draw.hpp"
#include "tv/diag.hpp"
#include "tv/remote_input.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>

namespace
{

namespace fs = std::filesystem;
using hui::Action;
using hui::Direction;

constexpr float kDt = 1.0f / 60.0f;

// The baked fonts of the kit: text is measured with them, so the tests lay
// out what the console lays out. KIT_FONTS is set by tools/run-tests.sh.
struct FontSet
{
    hui::gfx::Font regular, semibold, display, mono, east_asian, korean;
    hui::ui::Fonts fonts;

    FontSet()
    {
        const char *dir = std::getenv("KIT_FONTS");
        load(dir, "inter-regular.huifont", &regular, &fonts.regular, 0xf0000001u);
        load(dir, "inter-semibold.huifont", &semibold, &fonts.semibold, 0xf0000002u);
        load(dir, "montserrat-medium.huifont", &display, &fonts.display, 0xf0000003u);
        load(dir, "dejavu-sans-mono.huifont", &mono, &fonts.mono, 0xf0000004u);
        load(dir, "noto-sans-east-asian.huifont", &east_asian, &fonts.hand, 0xf0000005u);
        load(dir, "noto-sans-korean.huifont", &korean, &fonts.pixel, 0xf0000006u);
    }

    static void load(const char *dir, const char *name, hui::gfx::Font *font, hui::ui::FontRef *ref,
                     std::uint32_t handle)
    {
        std::string data;
        const std::string path = std::string(dir != nullptr ? dir : ".") + "/" + name;
        if (!hui::save::read_file(path, &data, 64u << 20) || !font->load(data))
        {
            ADD_FAILURE() << "cannot load " << path;
            return;
        }
        ref->font = font;
        ref->texture = handle;
    }
};

const FontSet &font_set()
{
    static const FontSet set;
    return set;
}

// Sixty channels in five kinds, enough for several rows of the grid.
std::string playlist_text()
{
    static constexpr const char *kinds[] = {"General", "News", "Sports", "Kids", "Movies"};
    static constexpr const char *sizes[] = {"", " (720p)", " (1080p)", " (576p)"};
    std::string text = "#EXTM3U\n";
    for (int i = 0; i < 60; ++i)
    {
        char line[320];
        std::snprintf(line, sizeof(line),
                      "#EXTINF:-1 tvg-id=\"c%02d.xx\" tvg-country=\"%s\" tvg-language=\"%s\" "
                      "group-title=\"%s\",Channel %02d%s\n"
                      "https://streams.example.invalid/c%02d/index.m3u8\n",
                      i, i % 2 == 0 ? "US" : "DE", i % 2 == 0 ? "English" : "German", kinds[i % 5],
                      i, sizes[i % 4], i);
        text += line;
    }
    return text;
}

class AppTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        host::reset();
        char pattern[] = "/tmp/prosperotv-app-XXXXXX";
        dir_ = mkdtemp(pattern);
        playlist_ = dir_ + "/playlist.m3u";
        std::ofstream(playlist_) << playlist_text();
        host::set_network(true, playlist_);
        model_ = std::make_unique<ptv::Model>(dir_);
        ASSERT_TRUE(model_->open());
        for (int i = 0; i < 400 && model_->refreshing(); ++i)
        {
            model_->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(model_->has_catalog());
        model_->take_notices();
        make_app();
    }

    void TearDown() override
    {
        app_.reset();
        if (model_)
            model_->close();
        model_.reset();
        std::error_code error;
        fs::remove_all(dir_, error);
    }

    void make_app()
    {
        app_ = std::make_unique<ptv::App>(*model_, font_set().fonts, 7u, ptv::Settings{}, "test");
        idle(30);
    }

    // One frame: update with an input, then record the frame the way the
    // renderer would be handed it.
    void frame(const hui::InputFrame &input)
    {
        feedback_.clear();
        app_->update(input, kDt, feedback_);
        app_->draw(frame_);
        cues_ += static_cast<int>(feedback_.cues.size());
    }
    void idle(int frames = 20)
    {
        hui::InputFrame input;
        input.connected = true;
        for (int i = 0; i < frames; ++i)
            frame(input);
    }
    void press(Action action, int settle = 20)
    {
        hui::InputFrame input;
        input.connected = true;
        input.pressed = hui::action_bit(action);
        input.held = input.pressed;
        frame(input);
        idle(settle);
    }
    void move(Direction direction, int settle = 12)
    {
        hui::InputFrame input;
        input.connected = true;
        input.nav = direction;
        frame(input);
        idle(settle);
    }
    // A button that goes down and stays down for that many frames.
    void hold(Action action, int frames)
    {
        hui::InputFrame input;
        input.connected = true;
        input.pressed = hui::action_bit(action);
        input.held = input.pressed;
        frame(input);
        input.pressed = 0;
        for (int i = 1; i < frames; ++i)
            frame(input);
    }
    // Downloads another playlist and starts the interface again on it.
    void use_playlist(const std::string &text)
    {
        app_.reset();
        std::ofstream(playlist_) << text;
        model_->refresh();
        // A large list takes its time: three minutes at most.
        for (int i = 0; i < 36000 && model_->refreshing(); ++i)
        {
            model_->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        model_->take_notices();
        make_app();
    }
    int position() const
    {
        return model_->position_of(model_->view.focused_channel);
    }

    std::string dir_;
    std::string playlist_;
    std::unique_ptr<ptv::Model> model_;
    std::unique_ptr<ptv::App> app_;
    ptv::Frame frame_;
    hui::ui::Feedback feedback_;
    int cues_ = 0;
};

TEST_F(AppTest, OpensOnLiveTvWithTheWholeCatalog)
{
    EXPECT_EQ(app_->tab(), 0);
    EXPECT_EQ(model_->group(), ptv::Group::all);
    EXPECT_EQ(model_->visible_count(), 60u);
    EXPECT_FALSE(frame_.scene.empty());
    EXPECT_TRUE(frame_.overlay.empty());
    EXPECT_FALSE(frame_.glass);
    EXPECT_EQ(frame_.backdrop.mode, hui::gfx::BackdropMode::aurora);
    EXPECT_EQ(model_->view.focused_channel, model_->channel(0).id);
}

TEST_F(AppTest, CrossQueuesTheChannelInFocus)
{
    move(Direction::right);
    move(Direction::down);
    press(Action::confirm);
    ptv::PlayRequest request;
    ASSERT_TRUE(model_->take_play_request(&request));
    // One to the right and one row of five down.
    EXPECT_EQ(request.channel_id, model_->channel(6).id);
    EXPECT_EQ(model_->view.focused_channel, request.channel_id);
    EXPECT_GT(cues_, 0);
}

TEST_F(AppTest, PhoneDirectionsAndEnterSelectTheVisibleChannel)
{
    frame(ptv::remote_input(IPTV_INPUT_RIGHT));
    frame(ptv::remote_input(IPTV_INPUT_DOWN));
    EXPECT_EQ(position(), 6);
    frame(ptv::remote_input(IPTV_INPUT_LEFT));
    frame(ptv::remote_input(IPTV_INPUT_UP));
    EXPECT_EQ(position(), 0);
    frame(ptv::remote_input(IPTV_INPUT_CROSS));
    ptv::PlayRequest request;
    ASSERT_TRUE(model_->take_play_request(&request));
    EXPECT_EQ(request.channel_id, model_->channel(0).id);
}

TEST_F(AppTest, PhoneTextReplacesOpenKeyboardAndClearKeepsFilters)
{
    model_->set_country("US");
    frame(ptv::remote_input(IPTV_INPUT_TRIANGLE));
    ASSERT_TRUE(app_->searching());
    frame(ptv::remote_input(IPTV_INPUT_CROSS));
    ASSERT_GT(host::keyboard_requests(), 0);
    host::set_keyboard_text("stale keyboard text");
    ASSERT_TRUE(app_->remote_search("Channel 10"));
    idle();
    EXPECT_FALSE(app_->searching());
    EXPECT_EQ(model_->query(), "Channel 10");
    EXPECT_EQ(model_->visible_count(), 1u);
    ASSERT_TRUE(app_->remote_search(""));
    idle();
    EXPECT_EQ(model_->country(), "US");
    EXPECT_EQ(model_->visible_count(), 30u);
}

TEST_F(AppTest, PhoneBackDismissesSearchAndShouldersChangeTabs)
{
    frame(ptv::remote_input(IPTV_INPUT_TRIANGLE));
    ASSERT_TRUE(app_->searching());
    frame(ptv::remote_input(IPTV_INPUT_CROSS));
    host::set_keyboard_text("cancelled by Back");
    frame(ptv::remote_input(IPTV_INPUT_CIRCLE));
    idle();
    EXPECT_FALSE(app_->searching());
    EXPECT_TRUE(model_->query().empty());
    frame(ptv::remote_input(IPTV_INPUT_R1));
    EXPECT_EQ(app_->tab(), 1);
    frame(ptv::remote_input(IPTV_INPUT_R1));
    EXPECT_EQ(app_->tab(), 2);
    EXPECT_FALSE(app_->remote_search("Channel"));
    frame(ptv::remote_input(IPTV_INPUT_L1));
    EXPECT_EQ(app_->tab(), 1);
}

TEST_F(AppTest, TheMenuComesBackWhereItWas)
{
    press(Action::page_next); // Favorites
    press(Action::page_prev);
    move(Direction::down);
    move(Direction::down);
    move(Direction::right);
    const std::string focused = model_->view.focused_channel;
    EXPECT_EQ(focused, model_->channel(11).id);

    // A channel plays: the interface is thrown away and made again.
    app_.reset();
    model_->close();
    ASSERT_TRUE(model_->open());
    make_app();
    EXPECT_EQ(model_->view.focused_channel, focused);
    press(Action::confirm);
    ptv::PlayRequest request;
    ASSERT_TRUE(model_->take_play_request(&request));
    EXPECT_EQ(request.channel_id, focused);
}

TEST_F(AppTest, ShouldersTurnTheTabsAndCircleLeadsHome)
{
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 1);
    EXPECT_EQ(model_->group(), ptv::Group::favorites);
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 2);
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 3);
    EXPECT_EQ(model_->view.tab, 3);
    press(Action::page_next); // About
    EXPECT_EQ(app_->tab(), 4);
    press(Action::page_next); // the last tab: nothing further
    EXPECT_EQ(app_->tab(), 4);
    // About only reads: nothing on it answers Cross.
    press(Action::confirm);
    EXPECT_EQ(app_->tab(), 4);
    EXPECT_FALSE(frame_.scene.empty());
    press(Action::back);
    EXPECT_EQ(app_->tab(), 0);
    EXPECT_EQ(model_->group(), ptv::Group::all);
}

TEST_F(AppTest, SquareStarsAndTheFavoritesTabListsIt)
{
    move(Direction::right);
    press(Action::west);
    EXPECT_TRUE(model_->is_favorite(model_->channel(1)));
    EXPECT_FALSE(frame_.overlay.empty()); // the toast
    EXPECT_TRUE(frame_.glass);

    press(Action::page_next);
    EXPECT_EQ(model_->visible_count(), 1u);
    EXPECT_EQ(model_->view.focused_channel, model_->channel(1).id);
    press(Action::west);
    EXPECT_EQ(model_->visible_count(), 0u);
    // An empty list offers the way back to Live TV.
    press(Action::confirm);
    EXPECT_EQ(app_->tab(), 0);
}

TEST_F(AppTest, TheListsAreReachedFromTheTopRow)
{
    move(Direction::up);    // onto the chips
    move(Direction::right); // Recent (empty)
    EXPECT_EQ(model_->group(), ptv::Group::recent);
    EXPECT_EQ(model_->visible_count(), 0u);
    move(Direction::right); // News
    EXPECT_EQ(model_->group(), ptv::Group::news);
    EXPECT_EQ(model_->visible_count(), 12u);
    move(Direction::down); // back into the grid
    press(Action::confirm);
    ptv::PlayRequest request;
    ASSERT_TRUE(model_->take_play_request(&request));
    EXPECT_EQ(request.channel_id, model_->channel(1).id);
    EXPECT_EQ(model_->view.live_group, ptv::Group::news);
}

TEST_F(AppTest, BackGoesToTheTopThenToTheFirstList)
{
    move(Direction::up);
    move(Direction::right);
    move(Direction::right); // News
    move(Direction::down);
    move(Direction::down);
    move(Direction::right);
    EXPECT_EQ(model_->view.focused_channel, model_->channel(model_->visible(6)).id);
    press(Action::back); // to the top of the list
    EXPECT_EQ(model_->view.focused_channel, model_->channel(model_->visible(0)).id);
    press(Action::back); // onto the chips
    press(Action::back); // to All
    EXPECT_EQ(model_->group(), ptv::Group::all);
    EXPECT_EQ(model_->visible_count(), 60u);
}

TEST_F(AppTest, TheSearchDrawerNarrowsTheListBehindIt)
{
    press(Action::north);
    ASSERT_TRUE(app_->searching());
    EXPECT_TRUE(frame_.glass);

    host::set_keyboard_text("channel 0");
    press(Action::confirm); // the keyboard
    EXPECT_EQ(model_->query(), "channel 0");
    EXPECT_EQ(model_->visible_count(), 10u); // 00..09

    move(Direction::down);  // Country
    press(Action::confirm); // its list
    move(Direction::down);
    press(Action::confirm); // the first country
    EXPECT_FALSE(model_->country().empty());
    const unsigned narrowed = model_->visible_count();
    EXPECT_EQ(narrowed, 5u);

    move(Direction::down);  // Category
    move(Direction::down);  // Language
    move(Direction::down);  // Picture size
    move(Direction::right); // SD
    EXPECT_EQ(model_->quality(), static_cast<unsigned>(ptv::kQualitySd));
    move(Direction::down); // Show the channels
    press(Action::confirm);
    EXPECT_FALSE(app_->searching());
    EXPECT_TRUE(model_->filtering());

    // Circle undoes the search before anything else.
    press(Action::back);
    EXPECT_FALSE(model_->filtering());
    EXPECT_EQ(model_->visible_count(), 60u);
    EXPECT_EQ(app_->tab(), 0);
}

TEST_F(AppTest, SquareInTheDrawerResetsAndCircleClosesIt)
{
    press(Action::north);
    host::set_keyboard_text("news");
    press(Action::confirm);
    EXPECT_TRUE(model_->filtering());
    press(Action::west);
    EXPECT_FALSE(model_->filtering());
    EXPECT_TRUE(app_->searching());
    press(Action::back);
    EXPECT_FALSE(app_->searching());
}

TEST_F(AppTest, AChannelThatFailedIsAskedAboutFirst)
{
    const std::string id(model_->channel(4).id);
    app_.reset();
    model_->close();
    ASSERT_TRUE(model_->open());
    model_->report_playback_failure(id.c_str(), "Channel 04", -5, 2, "The stream did not answer.");
    make_app();
    ASSERT_TRUE(app_->asking());
    // The dialog takes the input: the tabs do not turn under it.
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 0);
    press(Action::confirm); // Try again
    EXPECT_FALSE(app_->asking());
    ptv::PlayRequest request;
    ASSERT_TRUE(model_->take_play_request(&request));
    EXPECT_EQ(request.channel_id, id);
}

TEST_F(AppTest, ASourceIsSetUpFromItsRow)
{
    press(Action::page_next);
    press(Action::page_next); // Sources
    move(Direction::down);    // Custom playlist
    press(Action::confirm);   // not set up: its form
    EXPECT_EQ(host::keyboard_requests(), 1);
    EXPECT_EQ(host::keyboard_title(), "Playlist address");
    host::set_keyboard_text("https://lists.example.invalid/mine.m3u");
    idle(4);
    EXPECT_EQ(model_->active_source(), iptv::SourceKind::Custom);
    for (int i = 0; i < 200 && model_->refreshing(); ++i)
    {
        idle(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(model_->has_catalog());
    EXPECT_EQ(model_->view.focused_source, 1);
}

TEST_F(AppTest, SettingsAreChangedAndReported)
{
    press(Action::page_prev); // wraps to nothing: stays on Live TV
    EXPECT_EQ(app_->tab(), 0);
    press(Action::page_next);
    press(Action::page_next);
    press(Action::page_next);
    ASSERT_EQ(app_->tab(), 3);
    EXPECT_FALSE(app_->take_settings_changed());
    press(Action::confirm); // Reduce motion
    EXPECT_TRUE(app_->settings().reduced_motion);
    EXPECT_TRUE(app_->take_settings_changed());
    EXPECT_FALSE(app_->take_settings_changed());
    move(Direction::down);
    press(Action::confirm); // Interface sounds off
    EXPECT_FALSE(app_->settings().sounds);
    cues_ = 0;
    move(Direction::down);
    move(Direction::left); // Volume
    EXPECT_EQ(app_->settings().volume, 95);
    move(Direction::down);
    move(Direction::right); // Menu sharpness
    EXPECT_EQ(app_->settings().resolution, static_cast<int>(ptv::Settings::kFullHd));
    EXPECT_EQ(cues_, 0); // and nothing sounds any more
}

TEST_F(AppTest, PairingIsASettingsModalWithAnExplicitRequest)
{
    app_->set_pairing_info("http://192.0.2.1:8888", "123456", 120, 1);
    press(Action::page_next);
    press(Action::page_next);
    press(Action::page_next);
    for (int i = 0; i < 4; ++i)
        move(Direction::down);
    EXPECT_FALSE(app_->take_pair_phone_requested());
    press(Action::confirm);
    EXPECT_TRUE(app_->pairing_open());
    EXPECT_TRUE(app_->take_pair_phone_requested());
    EXPECT_FALSE(app_->take_pair_phone_requested());
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 3);
    app_->set_pairing_info("http://192.0.2.1:8888", "", 0, 1);
    press(Action::confirm);
    EXPECT_TRUE(app_->take_pair_phone_requested());
    press(Action::back);
    EXPECT_FALSE(app_->pairing_open());
    EXPECT_EQ(app_->tab(), 3);
    press(Action::confirm);
    EXPECT_TRUE(app_->pairing_open());
    app_->phone_connected();
    EXPECT_FALSE(app_->pairing_open());
    EXPECT_EQ(app_->tab(), 3);
    move(Direction::down);
    press(Action::confirm);
    EXPECT_TRUE(app_->take_forget_phones_requested());
}

std::vector<std::string> g_traced;

bool traced(const std::string &part)
{
    for (const std::string &line : g_traced)
        if (line.find(part) != std::string::npos)
            return true;
    return false;
}

TEST_F(AppTest, TheDiagnosticLogIsASwitchInSettingsOffByDefault)
{
    g_traced.clear();
    ptv::diag::set_forced(false);
    ptv::diag::set_sink([](const char *line) { g_traced.emplace_back(line); });
    EXPECT_FALSE(app_->settings().diagnostics);
    EXPECT_FALSE(ptv::diag::enabled());

    // Off: the viewer moves about and nothing is written.
    press(Action::page_next);
    press(Action::page_next);
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 3);
    EXPECT_TRUE(g_traced.empty());

    // The switch is the last row of Settings.
    for (int i = 0; i < 12; ++i)
        move(Direction::down);
    press(Action::confirm);
    EXPECT_TRUE(app_->settings().diagnostics);
    EXPECT_TRUE(app_->take_settings_changed());
    EXPECT_TRUE(ptv::diag::enabled());
    EXPECT_TRUE(traced("diagnostic log turned on in Settings"));
    EXPECT_TRUE(traced("diagnostics=1"));

    // On: what the viewer does and what the app answers are both there.
    press(Action::page_prev);
    EXPECT_TRUE(traced("input L1 on Settings"));
    EXPECT_TRUE(traced("tab Sources"));
    press(Action::page_prev);
    press(Action::page_prev);
    EXPECT_EQ(app_->tab(), 0);
    move(Direction::right);
    EXPECT_TRUE(traced("input right on Live TV"));
    press(Action::confirm);
    EXPECT_TRUE(traced("play asked: "));
    EXPECT_TRUE(traced("  address: https://"));
    // No address is written whole: the path of the stream stays out of the log.
    EXPECT_FALSE(traced("index.m3u8"));

    // And off again, with its own last line.
    g_traced.clear();
    press(Action::page_next);
    press(Action::page_next);
    press(Action::page_next);
    for (int i = 0; i < 12; ++i)
        move(Direction::down);
    press(Action::confirm);
    EXPECT_FALSE(app_->settings().diagnostics);
    EXPECT_FALSE(ptv::diag::enabled());
    EXPECT_TRUE(traced("diagnostic log turned off in Settings"));
    g_traced.clear();
    press(Action::page_prev);
    EXPECT_TRUE(g_traced.empty());
    ptv::diag::set_sink(nullptr);
}

TEST_F(AppTest, PhoneVolumeUpdatesTheSliderWithoutOverwritingOtherSettings)
{
    press(Action::page_next);
    press(Action::page_next);
    press(Action::page_next);
    press(Action::confirm); // reduce motion
    app_->take_settings_changed();
    app_->set_volume(30);
    EXPECT_FALSE(app_->take_settings_changed());
    EXPECT_TRUE(app_->settings().reduced_motion);
    move(Direction::down);
    move(Direction::down);
    move(Direction::left);
    EXPECT_EQ(app_->settings().volume, 25);
    EXPECT_TRUE(app_->take_settings_changed());
}

TEST_F(AppTest, VolumeSettingsPersistAndClamp)
{
    EXPECT_EQ(ptv::load_settings(dir_).volume, 100);
    ptv::Settings settings;
    settings.volume = 35;
    ASSERT_TRUE(ptv::save_settings(dir_, settings));
    EXPECT_EQ(ptv::load_settings(dir_).volume, 35);
    std::ofstream(dir_ + "/prosperotv-interface-v1.txt") << "volume=-20\n";
    EXPECT_EQ(ptv::load_settings(dir_).volume, 0);
    std::ofstream(dir_ + "/prosperotv-interface-v1.txt") << "volume=200\n";
    EXPECT_EQ(ptv::load_settings(dir_).volume, 100);
}

TEST_F(AppTest, OptionsStartsAnUpdateOnce)
{
    host::set_network(true, playlist_, 200);
    const int before = host::fetch_count();
    press(Action::menu, 2);
    EXPECT_TRUE(model_->refreshing());
    press(Action::menu, 2); // a second press while it runs starts nothing more
    for (int i = 0; i < 200 && model_->refreshing(); ++i)
    {
        idle(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(model_->refreshing());
    EXPECT_EQ(host::fetch_count(), before + 1);
    EXPECT_EQ(model_->view.focused_channel, model_->channel(0).id);
}

// Seven channels under each of A, C, M and T, and two that start with a digit.
std::string lettered_playlist()
{
    std::string text = "#EXTM3U\n";
    int id = 0;
    for (const char *word : {"Tern", "Alder", "Moss", "Cedar"})
        for (int i = 0; i < 7; ++i, ++id)
        {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "#EXTINF:-1 tvg-id=\"l%02d.xx\",%s %d\n"
                          "https://streams.example.invalid/l%02d/index.m3u8\n",
                          id, word, i, id);
            text += line;
        }
    text += "#EXTINF:-1 tvg-id=\"n1.xx\",1 Plus\nhttps://streams.example.invalid/n1/index.m3u8\n";
    text += "#EXTINF:-1 tvg-id=\"n2.xx\",24 Hours\nhttps://streams.example.invalid/n2/index.m3u8\n";
    return text;
}

TEST_F(AppTest, RightFromTheLastColumnLeadsOntoTheLetters)
{
    use_playlist(lettered_playlist());
    ASSERT_EQ(model_->visible_count(), 30u);
    EXPECT_EQ(position(), 0); // "1 Plus", under '#'
    for (int i = 0; i < 4; ++i)
        move(Direction::right);
    EXPECT_FALSE(app_->on_letters());
    EXPECT_EQ(position(), 4);
    move(Direction::right);
    EXPECT_TRUE(app_->on_letters());
    EXPECT_EQ(position(), 4); // nothing moved yet: Alder 2 is under A

    // Down goes to the next letter that has channels: C, then M, then T.
    move(Direction::down);
    EXPECT_EQ(position(), model_->letter_start(3));
    EXPECT_EQ(model_->channel(model_->visible(static_cast<unsigned>(position()))).name, "Cedar 0");
    move(Direction::down);
    EXPECT_EQ(position(), model_->letter_start(13));
    move(Direction::down);
    EXPECT_EQ(position(), model_->letter_start(20));
    move(Direction::down); // nothing after T
    EXPECT_EQ(position(), model_->letter_start(20));
    move(Direction::up);
    EXPECT_EQ(position(), model_->letter_start(13));

    // Cross on a letter does not play: it hands the focus to its channels.
    press(Action::confirm);
    ptv::PlayRequest request;
    EXPECT_FALSE(model_->take_play_request(&request));
    EXPECT_FALSE(app_->on_letters());
    press(Action::confirm);
    ASSERT_TRUE(model_->take_play_request(&request));
    EXPECT_EQ(request.channel_name, "Moss 0");
}

TEST_F(AppTest, CircleAndLeftLeaveTheLetters)
{
    use_playlist(lettered_playlist());
    for (int i = 0; i < 5; ++i)
        move(Direction::right);
    ASSERT_TRUE(app_->on_letters());
    move(Direction::up); // '#'
    EXPECT_EQ(position(), 0);
    press(Action::back);
    EXPECT_FALSE(app_->on_letters());
    EXPECT_EQ(app_->tab(), 0);
    for (int i = 0; i < 5; ++i)
        move(Direction::right);
    ASSERT_TRUE(app_->on_letters());
    move(Direction::left);
    EXPECT_FALSE(app_->on_letters());
    // A held Right stops at the end of the row instead of slipping onto them.
    hui::InputFrame input;
    input.connected = true;
    input.nav = Direction::right;
    input.nav_repeat = true;
    for (int i = 0; i < 8; ++i)
    {
        frame(input);
        idle(4);
    }
    EXPECT_FALSE(app_->on_letters());
    EXPECT_EQ(position() % 5, 4);
}

TEST_F(AppTest, AHeldTriggerKeepsTurningPages)
{
    // One press is one page of ten.
    press(Action::jump_next);
    EXPECT_EQ(position(), 10);
    press(Action::jump_prev);
    EXPECT_EQ(position(), 0);

    // Held for a second: the first page at once, a wait, then a steady run.
    hold(Action::jump_next, 60);
    const int after = position();
    EXPECT_GE(after, 40);
    EXPECT_LT(after, 60);
    // Let go: it stops where it is.
    idle(40);
    EXPECT_EQ(position(), after);

    // Held to the end of the list: it stops on the last channel and stays.
    hold(Action::jump_next, 200);
    EXPECT_EQ(position(), 59);
    // And back the same way.
    hold(Action::jump_prev, 240);
    EXPECT_EQ(position(), 0);
}

TEST_F(AppTest, AHoldEndsWhenSomethingElseTakesTheController)
{
    hold(Action::jump_next, 10);
    EXPECT_EQ(position(), 10);
    // The drawer opens with R2 still down; closing it turns no page.
    hui::InputFrame input;
    input.connected = true;
    input.held = hui::action_bit(Action::jump_next);
    input.pressed = hui::action_bit(Action::north);
    frame(input);
    ASSERT_TRUE(app_->searching());
    input.pressed = 0;
    for (int i = 0; i < 60; ++i)
        frame(input);
    input.pressed = hui::action_bit(Action::back);
    frame(input);
    input.pressed = 0;
    for (int i = 0; i < 60; ++i)
        frame(input);
    EXPECT_FALSE(app_->searching());
    EXPECT_EQ(position(), 10);
}

// Returning from a channel far down a long list: its rows are there at once,
// not after the wave of the entrance has walked down from the first row.
TEST_F(AppTest, AListOpenedDeepShowsItsTilesAtOnce)
{
    std::string text = "#EXTM3U\n";
    for (int i = 0; i < 3000; ++i)
    {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "#EXTINF:-1 tvg-id=\"d%04d.xx\",Channel %04d\n"
                      "https://streams.example.invalid/d%04d/index.m3u8\n",
                      i, i, i);
        text += line;
    }
    use_playlist(text);
    ASSERT_EQ(model_->visible_count(), 3000u);
    idle(60);
    const std::size_t at_the_top = frame_.scene.instances().size();

    app_.reset();
    model_->view.focused_channel = model_->channel(model_->visible(2990)).id;
    make_app();
    EXPECT_EQ(position(), 2990);
    EXPECT_GT(frame_.scene.instances().size() * 10, at_the_top * 6);
}

// A hundred and twenty thousand channels: the grid still draws one screen of
// them, the letters and a held trigger cross the whole list, and a frame
// costs what it costs with sixty.
TEST_F(AppTest, AListOfMoreThanAHundredThousandChannelsIsOneScreenAtATime)
{
    idle(60);
    const std::size_t with_sixty = frame_.scene.instances().size();
    constexpr unsigned kChannels = 120000;
    use_playlist(large_list::playlist(kChannels));
    ASSERT_EQ(model_->visible_count(), kChannels);
    idle(60);
    EXPECT_EQ(position(), 0);
    EXPECT_LT(frame_.scene.instances().size(), with_sixty * 2);

    // Deep in the list at once.
    app_.reset();
    model_->view.focused_channel = model_->channel(model_->visible(100000)).id;
    make_app();
    EXPECT_EQ(position(), 100000);
    EXPECT_LT(frame_.scene.instances().size(), with_sixty * 2);

    // A held R2 turns pages for as long as it is held.
    hold(Action::jump_next, 180);
    idle(20);
    const int after_pages = position();
    EXPECT_GT(after_pages, 100000);

    // The letters: from the last column onto the rail, then to another letter.
    for (int i = 0; i < 6 && !app_->on_letters(); ++i)
        move(Direction::right);
    ASSERT_TRUE(app_->on_letters());
    const int letter = model_->letter_at(static_cast<unsigned>(position()));
    move(Direction::up);
    idle(30);
    EXPECT_EQ(model_->letter_at(static_cast<unsigned>(position())), letter - 1);
    EXPECT_EQ(position(), model_->letter_start(letter - 1));
    press(Action::back);

    // A search narrows it and the grid follows.
    model_->set_query("net 077777");
    idle(30);
    ASSERT_EQ(model_->visible_count(), 1u);
    EXPECT_EQ(position(), 0);
    model_->set_query("");
    idle(30);
    EXPECT_EQ(model_->visible_count(), kChannels);
}

iptv::Channel written(const char *name, const char *id = "")
{
    iptv::Channel channel;
    channel.name = name;
    channel.tvg_id = id;
    return channel;
}

// ---- the update -------------------------------------------------------------

ptv::platform::UpdateOffer newer_version()
{
    ptv::platform::UpdateOffer offer;
    offer.installable = true;
    offer.version = "01.000.020";
    offer.installed = "01.000.015";
    offer.available = "01.000.020";
    offer.size = 40u * 1024u * 1024u;
    return offer;
}

ptv::platform::UpdateProgress progress(ptv::platform::UpdatePhase phase, std::uint64_t done = 0,
                                       std::uint64_t total = 0)
{
    ptv::platform::UpdateProgress value;
    value.phase = phase;
    value.done = done;
    value.total = total;
    return value;
}

using Stage = ptv::UpdateSheet::Stage;
using ptv::platform::UpdatePhase;

TEST_F(AppTest, ANewerVersionIsOfferedAndLaterLeavesEverythingAlone)
{
    host::offer_update(newer_version());
    idle(30);
    ASSERT_EQ(app_->update_sheet().stage(), Stage::offer);
    EXPECT_FALSE(frame_.overlay.empty());
    EXPECT_TRUE(frame_.glass);
    // The offer has the controller: the tabs do not turn behind it.
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 0);
    move(Direction::right);
    EXPECT_EQ(app_->update_sheet().focus(), 1);
    press(Action::confirm);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closed);
    EXPECT_EQ(host::update_calls().begin, 0);
    EXPECT_FALSE(app_->wants_quit());
    // It is offered once a launch.
    idle(60);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closed);
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 1);
}

// Release notes as the catalog gives them: headings, list items, callouts.
const char *release_notes()
{
    return "Highlights\n"
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
}

TEST_F(AppTest, AReleaseWithNotesOffersWhatsNewAndTheNotesScroll)
{
    ptv::platform::UpdateOffer offer = newer_version();
    offer.notes = release_notes();
    offer.notes_truncated = true;
    host::offer_update(offer);
    idle(30);
    const ptv::UpdateSheet &sheet = app_->update_sheet();
    ASSERT_EQ(sheet.stage(), Stage::offer);
    // Three answers now: Update now, What's new, Later.
    move(Direction::right);
    move(Direction::right);
    EXPECT_EQ(sheet.focus(), 2);
    move(Direction::right); // nothing further
    EXPECT_EQ(sheet.focus(), 2);
    move(Direction::left);
    press(Action::confirm);
    ASSERT_EQ(sheet.stage(), Stage::notes);
    EXPECT_EQ(host::update_calls().begin, 0);

    // The text is longer than its window: it scrolls, and stops at its end.
    ASSERT_GT(sheet.notes_max_scroll(), 0.0f);
    EXPECT_EQ(sheet.notes_scroll(), 0.0f);
    move(Direction::up); // already at the top
    EXPECT_EQ(sheet.notes_scroll(), 0.0f);
    move(Direction::down);
    EXPECT_GT(sheet.notes_scroll(), 0.0f);
    for (int i = 0; i < 40; ++i)
        move(Direction::down, 2);
    EXPECT_EQ(sheet.notes_scroll(), sheet.notes_max_scroll());
    press(Action::jump_prev);
    EXPECT_LT(sheet.notes_scroll(), sheet.notes_max_scroll());

    // Circle goes back to the offer, on the button that led here.
    press(Action::back);
    ASSERT_EQ(sheet.stage(), Stage::offer);
    EXPECT_EQ(sheet.focus(), 1);
    EXPECT_EQ(app_->tab(), 0);

    // And the update can be started from the notes.
    press(Action::confirm);
    ASSERT_EQ(sheet.stage(), Stage::notes);
    host::set_update_progress(progress(UpdatePhase::starting));
    press(Action::confirm);
    EXPECT_EQ(sheet.stage(), Stage::working);
    EXPECT_EQ(host::update_calls().begin, 1);
}

TEST_F(AppTest, WithoutNotesTheOfferKeepsItsTwoAnswers)
{
    host::offer_update(newer_version());
    idle(30);
    move(Direction::right);
    move(Direction::right);
    EXPECT_EQ(app_->update_sheet().focus(), 1);
    press(Action::confirm); // Later
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closed);
}

TEST_F(AppTest, TheOpeningPlaysOnceKeepsTheControllerAndEndsByItself)
{
    app_->play_intro();
    ASSERT_TRUE(app_->intro_playing());
    // The controller is not read while it plays: the tab stays.
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 0);
    // That button took it to its last moment; it is over within a second.
    idle(70);
    EXPECT_FALSE(app_->intro_playing());
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 1);

    // Left alone it runs its whole length, then hands over.
    app_->play_intro();
    idle(120);
    EXPECT_TRUE(app_->intro_playing());
    idle(100);
    EXPECT_FALSE(app_->intro_playing());
}

TEST_F(AppTest, WithReduceMotionThereIsNoOpening)
{
    ptv::Settings settings;
    settings.reduced_motion = true;
    app_ = std::make_unique<ptv::App>(*model_, font_set().fonts, 7u, settings, "test");
    app_->play_intro();
    EXPECT_FALSE(app_->intro_playing());
}

TEST_F(AppTest, CircleOnTheOfferMeansLater)
{
    host::offer_update(newer_version());
    idle(30);
    press(Action::back);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closed);
    EXPECT_EQ(host::update_calls().begin, 0);
}

TEST_F(AppTest, AnUpdateDownloadsUnpacksAndClosesTheApp)
{
    host::offer_update(newer_version());
    idle(30);
    host::set_update_progress(progress(UpdatePhase::starting));
    press(Action::confirm);
    ASSERT_EQ(app_->update_sheet().stage(), Stage::working);
    EXPECT_EQ(host::update_calls().begin, 1);
    host::set_update_progress(progress(UpdatePhase::downloading, 10u << 20, 40u << 20));
    idle(30);
    host::set_update_progress(progress(UpdatePhase::unpacking, 30u << 20, 90u << 20));
    idle(30);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::working);
    EXPECT_FALSE(app_->wants_quit());
    host::set_update_progress(progress(UpdatePhase::ready));
    idle(5);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closing);
    EXPECT_EQ(host::update_calls().apply, 1);
    // The closing picture is shown before the app goes, and nothing answers.
    EXPECT_FALSE(app_->wants_quit());
    press(Action::back);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closing);
    idle(240);
    EXPECT_TRUE(app_->wants_quit());
    EXPECT_EQ(host::update_calls().apply, 1);
    EXPECT_EQ(host::update_calls().cancel, 0);
}

TEST_F(AppTest, CancelStopsAnUpdateAndTheMenuComesBack)
{
    host::offer_update(newer_version());
    idle(30);
    host::set_update_progress(progress(UpdatePhase::downloading, 1u << 20, 40u << 20));
    press(Action::confirm);
    ASSERT_EQ(app_->update_sheet().stage(), Stage::working);
    press(Action::confirm); // the one button there is: Cancel
    EXPECT_EQ(app_->update_sheet().stage(), Stage::cancelling);
    EXPECT_EQ(host::update_calls().cancel, 1);
    host::set_update_progress(progress(UpdatePhase::cancelled));
    idle(10);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closed);
    EXPECT_EQ(host::update_calls().finish, 1);
    EXPECT_EQ(host::update_calls().apply, 0);
    EXPECT_FALSE(app_->wants_quit());
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 1);
}

TEST_F(AppTest, AFailedUpdateSaysSoAndCanBeTriedAgain)
{
    host::offer_update(newer_version());
    idle(30);
    host::refuse_update(true, false);
    press(Action::confirm);
    ASSERT_EQ(app_->update_sheet().stage(), Stage::failed);
    host::refuse_update(false, false);
    ptv::platform::UpdateProgress broken = progress(UpdatePhase::failed);
    broken.error = "The download stopped.";
    host::set_update_progress(broken);
    press(Action::confirm); // Try again
    EXPECT_EQ(app_->update_sheet().stage(), Stage::failed);
    EXPECT_EQ(host::update_calls().begin, 2);
    move(Direction::right);
    press(Action::confirm); // Close
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closed);
    EXPECT_FALSE(app_->wants_quit());
}

TEST_F(AppTest, AStagedUpdateThatCannotBePutInPlaceFails)
{
    host::offer_update(newer_version());
    idle(30);
    host::refuse_update(false, true);
    host::set_update_progress(progress(UpdatePhase::ready));
    press(Action::confirm);
    idle(5);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::failed);
    EXPECT_FALSE(app_->wants_quit());
}

TEST_F(AppTest, AVersionTheAppCannotInstallIsOnlyAnnounced)
{
    ptv::platform::UpdateOffer offer = newer_version();
    offer.installable = false;
    host::offer_update(offer);
    idle(30);
    EXPECT_EQ(app_->update_sheet().stage(), Stage::closed);
    EXPECT_FALSE(frame_.overlay.empty()); // the notice at the top right
    press(Action::page_next);
    EXPECT_EQ(app_->tab(), 1);
}

TEST(ShownNames, AreWrittenInTheScriptsTheFontsHold)
{
    const hui::ui::Fonts &fonts = font_set().fonts;
    // Accents, Cyrillic and Greek are baked: the name is shown as written.
    EXPECT_EQ(ptv::shown_name(fonts, written("Ni\xC3\xB1os T\xC3\xA9l\xC3\xA9 (720p)")),
              "Ni\xC3\xB1os T\xC3\xA9l\xC3\xA9");
    const char *cyrillic =
        "\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB2\xD1\x8B\xD0\xB9 \xD0\xBA\xD0\xB0\xD0\xBD\xD0\xB0\xD0\xBB";
    EXPECT_EQ(ptv::shown_name(fonts, written(cyrillic)), cyrillic);
    const char *greek = "\xCE\x95\xCE\xA1\xCE\xA4 1";
    EXPECT_EQ(ptv::shown_name(fonts, written(greek)), greek);
    // Every letter of a shown name can be drawn by the face it is drawn in.
    for (const char *name : {cyrillic, greek, "Ni\xC3\xB1os"})
    {
        const hui::ui::FontRef &face = ptv::title_face(fonts, name);
        const std::string_view text(name);
        for (std::size_t index = 0; index < text.size();)
        {
            const std::uint32_t codepoint = hui::gfx::next_codepoint(text, &index);
            EXPECT_TRUE(codepoint <= 0x20 || face.font->has_glyph(codepoint)) << name;
        }
    }
}

TEST(ShownNames, ChineseJapaneseAndKoreanAreWrittenAsTheyAre)
{
    const hui::ui::Fonts &fonts = font_set().fonts;
    const char *chinese = "\xE4\xB8\xAD\xE6\x96\x87\xE9\xA2\x91\xE9\x81\x93";     // 中文频道
    const char *mixed = "CCTV-5 \xE9\xAB\x98\xE6\xB8\x85";                              // CCTV-5 高清
    const char *japanese =
        "\xE3\x83\x86\xE3\x83\xAC\xE3\x83\x93\xE6\x9C\x9D\xE6\x97\xA5";        // テレビ朝日
    const char *korean = "\xEC\x97\xB0\xED\x95\xA9\xEB\x89\xB4\xEC\x8A\xA4TV";         // 연합뉴스TV
    EXPECT_EQ(ptv::shown_name(fonts, written(chinese, "CCTV4.cn@SD")), chinese);
    EXPECT_EQ(ptv::shown_name(fonts, written(mixed, "CCTV5.cn")), mixed);
    EXPECT_EQ(ptv::shown_name(fonts, written(japanese)), japanese);
    EXPECT_EQ(ptv::shown_name(fonts, written(korean)), korean);
    // Each is written with the face that holds all of it; Latin names keep theirs.
    EXPECT_EQ(&ptv::face_for(fonts, fonts.semibold, chinese), &fonts.hand);
    EXPECT_EQ(&ptv::face_for(fonts, fonts.semibold, mixed), &fonts.hand);
    EXPECT_EQ(&ptv::face_for(fonts, fonts.semibold, japanese), &fonts.hand);
    EXPECT_EQ(&ptv::face_for(fonts, fonts.semibold, korean), &fonts.pixel);
    EXPECT_EQ(&ptv::face_for(fonts, fonts.semibold, "France 24"), &fonts.semibold);
    EXPECT_EQ(&ptv::title_face(fonts, "France 24"), &fonts.display);
    EXPECT_EQ(&ptv::title_face(fonts, mixed), &fonts.hand);
    // A name of one script only gets its first character on its screen.
    EXPECT_EQ(ptv::monogram(written(chinese)), "\xE4\xB8\xAD");
    EXPECT_EQ(ptv::monogram(written(mixed)), "C5");
    // A note in Chinese is kept too.
    std::vector<std::string> notes;
    const std::string noted = std::string("Alder [") + chinese + "] [Not 24/7]";
    EXPECT_EQ(ptv::shown_name(fonts, written(noted.c_str()), &notes), "Alder");
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0], chinese);
}

TEST(ShownNames, FallBackWhenAScriptIsNotBaked)
{
    const hui::ui::Fonts &fonts = font_set().fonts;
    const char *thai = "\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2\xE0\xB8\x97\xE0\xB8\xB5\xE0\xB8\xA7"
                       "\xE0\xB8\xB5"; // ไทยทีวี
    // Most of a mixed name survives: it is kept without the letters that cannot be drawn.
    EXPECT_EQ(
        ptv::shown_name(fonts, written((std::string("Thai PBS ") + thai).c_str(), "ThaiPBS.th")),
              "Thai PBS");
    // Nothing survives: the playlist's own id for the channel stands in.
    EXPECT_EQ(ptv::shown_name(fonts, written(thai, "Channel3.th@SD")), "Channel3");
    // And without an id the tile still says something.
    EXPECT_EQ(ptv::shown_name(fonts, written(thai)), "Channel");
    // Notes are filtered the same way; one that cannot be written is dropped.
    std::vector<std::string> notes;
    const std::string noted = std::string("Alder [") + thai + "] [Not 24/7]";
    EXPECT_EQ(ptv::shown_name(fonts, written(noted.c_str()), &notes), "Alder");
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0], "Not 24/7");
    EXPECT_EQ(ptv::readable(fonts.regular, std::string("News | ") + thai), "News");
}

// Whatever a player presses, in whatever order: nothing may fault, the tab
// must stay a tab and every frame must record.
TEST_F(AppTest, RandomInputNeverBreaksIt)
{
    std::mt19937 random(20261002u);
    static constexpr Action actions[] = {
        Action::confirm,   Action::back,      Action::north,     Action::west, Action::page_prev,
        Action::page_next, Action::jump_prev, Action::jump_next, Action::menu, Action::touch};
    static constexpr Direction directions[] = {Direction::up, Direction::down, Direction::left,
                                               Direction::right};
    for (int step = 0; step < 4000; ++step)
    {
        hui::InputFrame input;
        input.connected = true;
        const unsigned roll = random() % 100;
        if (roll < 45)
        {
            input.nav = directions[random() % 4];
            input.nav_repeat = random() % 4 == 0;
        }
        else if (roll < 80)
        {
            input.pressed = hui::action_bit(actions[random() % std::size(actions)]);
            input.held = input.pressed;
        }
        if (random() % 40 == 0)
            host::set_keyboard_text(random() % 2 == 0 ? "1" : "nothing matches this");
        if (random() % 300 == 0)
            host::set_network(random() % 2 == 0, playlist_, 20);
        if (random() % 250 == 0)
            host::offer_update(newer_version());
        if (random() % 60 == 0)
        {
            static constexpr UpdatePhase phases[] = {
                UpdatePhase::starting, UpdatePhase::downloading, UpdatePhase::unpacking,
                UpdatePhase::failed,   UpdatePhase::cancelled,   UpdatePhase::idle};
            host::set_update_progress(
                progress(phases[random() % std::size(phases)], random() % 5000, random() % 5000));
        }
        frame(input);
        // A channel was chosen: the frame loop would play it and come back.
        ptv::PlayRequest request;
        if (model_->take_play_request(&request) && random() % 3 == 0)
        {
            app_.reset();
            model_->close();
            ASSERT_TRUE(model_->open());
            if (random() % 2 == 0)
                model_->report_playback_failure(request.channel_id.c_str(),
                                                request.channel_name.c_str(), -1, 1, "");
            make_app();
        }
        ASSERT_GE(app_->tab(), 0);
        ASSERT_LT(app_->tab(), 5);
        ASSERT_FALSE(frame_.scene.empty());
    }
}

} // namespace
