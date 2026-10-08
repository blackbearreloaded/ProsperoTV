// ProsperoTV - Provider VOD, saved copies, controller navigation and playback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/vod.hpp"
#include "tv/vod_screen.hpp"
#include "host_platform.hpp"
#include "iptv_store.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
class VodTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        host::reset();
        char pattern[] = "/tmp/prosperotv-vod-XXXXXX";
        dir = mkdtemp(pattern);
        host::set_network(true, "");
        host::set_network_piece(1);
        response(
            "get_vod_categories",
            R"([{"category_id":1,"category_name":"US"},{"category_id":2,"category_name":"Movies","parent_id":1},{"category_id":3,"category_name":"Kids","parent_id":2}])");
        response(
            "get_vod_streams",
            R"([{"stream_id":10,"name":"Alpha movie","container_extension":"mkv","category_id":3,"stream_icon":"https://image.invalid/10.png"},{"stream_id":11,"name":"Beta movie","container_extension":"mp4","category_id":2},{"stream_id":10,"name":"Duplicate"}])");
        response("get_series_categories", R"([{"category_id":4,"category_name":"Comedy"}])");
        response(
            "get_series",
            R"([{"series_id":20,"name":"A show","cover":"https://image.invalid/show.jpg","category_id":4}])");
        std::string series;
        iptv::BuildXtreamSeriesUrl(account, "20", &series);
        reply(
            series,
            R"({"info":{"name":"A show"},"episodes":{"10":[{"id":200,"title":"Finale","episode_num":1,"container_extension":"mp4"}],"2":[{"id":201,"title":"Later","episode_num":10,"container_extension":"mkv"},{"id":202,"title":"Earlier","episode_num":2,"container_extension":"mp4"}]}})");
        ptv::Library library;
        ASSERT_TRUE(library.open(dir + "/prosperotv-library.sqlite3"));
        ptv::SavedSource source{3,
                                2,
                                "Test account",
                                account.server_url,
                                account.username,
                                account.password,
                                {},
                                ptv::RefreshSchedule::manual};
        ASSERT_TRUE(library.save_source(&source));
        ASSERT_TRUE(library.select_source(3));
        auto catalog =
            iptv::ParseExtendedM3u("#EXTM3U\n#EXTINF:-1,Live\nhttps://stream.invalid/live.ts\n",
                                   iptv::XtreamSourceId(account));
        ASSERT_EQ(iptv::SaveCatalog(dir + "/prosperotv-xtream-catalog.sqlite3", catalog),
                  iptv::StoreStatus::ok);
    }
    void TearDown() override
    {
        host::reset();
        std::filesystem::remove_all(dir);
    }
    void reply(const std::string &url, std::string_view body)
    {
        const auto path = dir + "/response" + std::to_string(number++);
        std::ofstream(path) << body;
        host::set_network_response(url, path);
    }
    void response(std::string_view action, std::string_view body)
    {
        std::string url;
        ASSERT_TRUE(iptv::BuildXtreamApiUrl(account, action, &url));
        reply(url, body);
    }
    bool settle(ptv::Model &model)
    {
        for (int i = 0; i < 500; ++i)
        {
            model.poll();
            if (!model.vod().busy() && !model.vod().requested() && !model.refreshing())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    const iptv::XtreamCredentials account{"https://provider.invalid/sub", "user@home", "p&ss"};
    std::string dir;
    int number = 0;
};

TEST_F(VodTest, DownloadsNestedCategoriesAndEscapesMovieAndEpisodeCredentials)
{
    iptv::Catalog catalog;
    std::string error;
    ASSERT_TRUE(ptv::load_vod(account, ptv::VodKind::movies, {}, &catalog, nullptr, &error))
        << error;
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(catalog[0].group_title, "US / Movies / Kids");
    EXPECT_EQ(catalog[0].url, "https://provider.invalid/sub/movie/user%40home/p%26ss/10.mkv");
    EXPECT_EQ(catalog[1].url, "https://provider.invalid/sub/movie/user%40home/p%26ss/11.mp4");
    EXPECT_EQ(catalog[0].tvg_logo, "https://image.invalid/10.png");
    const std::string movie_id(catalog[0].id);
    ASSERT_TRUE(ptv::load_vod(account, ptv::VodKind::episodes, "20", &catalog, nullptr, &error))
        << error;
    ASSERT_EQ(catalog.size(), 3u);
    EXPECT_EQ(catalog[2].url, "https://provider.invalid/sub/series/user%40home/p%26ss/202.mp4");
    EXPECT_EQ(catalog[2].group_title, "Season 2");
    EXPECT_EQ(catalog[2].source_line, 200002u);
    EXPECT_NE(catalog[0].id, movie_id);
}

TEST_F(VodTest, ControllerBrowsesChildrenAndStartsAMovieWithoutChangingLiveHistory)
{
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    ASSERT_FALSE(model.refreshing());
    const hui::ui::Fonts fonts{};
    ptv::Shared shared(model, fonts, {});
    ptv::VodScreen screen(shared);
    hui::ui::Feedback feedback;
    const auto press = [&](hui::Action action)
    {
        hui::InputFrame input;
        input.pressed = hui::action_bit(action);
        screen.handle(input, feedback);
    };
    const auto down = [&]
    {
        hui::InputFrame input;
        input.nav = hui::Direction::down;
        screen.handle(input, feedback);
    };
    screen.enter();
    press(hui::Action::confirm);
    ASSERT_TRUE(settle(model));
    ASSERT_TRUE(model.vod().loaded());
    screen.update();
    down();
    press(hui::Action::confirm);
    EXPECT_EQ(model.view.vod_category, "US");
    down();
    press(hui::Action::confirm);
    EXPECT_EQ(model.view.vod_category, "US / Movies");
    press(hui::Action::confirm); // Browse all below Movies, including Kids.
    press(hui::Action::confirm); // Alphabetical first movie.
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.channel_name, "Alpha movie");
    EXPECT_FALSE(request.record_channel_result);
    EXPECT_FALSE(request.reconnect_live);
    EXPECT_TRUE(model.find(request.channel_id).has_value());
    EXPECT_EQ(model.group_size(ptv::Group::recent), 0u);
    ASSERT_TRUE(model.hide_category("US", true));
    screen.update();
    press(hui::Action::confirm);
    EXPECT_FALSE(model.take_play_request(&request));
    ASSERT_TRUE(model.hide_category("US", false));
    screen.update();
    ASSERT_TRUE(screen.back());
    EXPECT_FALSE(model.view.vod_all);
    ASSERT_TRUE(screen.back());
    EXPECT_EQ(model.view.vod_category, "US");
    model.close();
}

TEST_F(VodTest, KeepsManualCachesAcrossRestartAndRetainsThemAfterMalformedRefresh)
{
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.vod().select(ptv::VodKind::movies);
        ASSERT_TRUE(settle(model));
        ASSERT_EQ(model.vod().catalog().size(), 2u);
        model.close();
    }
    const auto requests = host::requests().size();
    ptv::Model again(dir);
    ASSERT_TRUE(again.open());
    again.vod().select(ptv::VodKind::movies);
    EXPECT_FALSE(again.vod().requested());
    EXPECT_EQ(again.vod().catalog().size(), 2u);
    EXPECT_EQ(host::requests().size(), requests);
    response("get_vod_streams", "[{\"stream_id\":1,");
    again.vod().select(ptv::VodKind::movies, {}, true);
    ASSERT_TRUE(settle(again));
    EXPECT_EQ(again.vod().catalog().size(), 2u);
    EXPECT_NE(again.vod().status().find("saved titles"), std::string::npos);
    again.close();
}

TEST_F(VodTest, OpensARealSeasonAndOrdersEpisodesNumerically)
{
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.view.vod_kind = static_cast<int>(ptv::VodKind::series);
    const hui::ui::Fonts fonts{};
    ptv::Shared shared(model, fonts, {});
    ptv::VodScreen screen(shared);
    hui::ui::Feedback feedback;
    const auto confirm = [&]
    {
        hui::InputFrame input;
        input.pressed = hui::action_bit(hui::Action::confirm);
        screen.handle(input, feedback);
    };
    screen.enter();
    ASSERT_TRUE(settle(model));
    screen.update();
    confirm(); // Browse all shows.
    confirm(); // A show.
    EXPECT_EQ(model.view.vod_kind, static_cast<int>(ptv::VodKind::episodes));
    ASSERT_TRUE(settle(model));
    screen.update();
    hui::InputFrame down;
    down.nav = hui::Direction::down;
    screen.handle(down, feedback);
    confirm(); // Season 2 before Season 10.
    EXPECT_EQ(model.view.vod_category, "Season 2");
    confirm(); // All episodes in season 2.
    confirm(); // Episode 2 before Episode 10.
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.urls.front(),
              "https://provider.invalid/sub/series/user%40home/p%26ss/202.mp4");
    EXPECT_EQ(request.channel_name, "Episode 2 · Earlier");
    model.close();
}

TEST_F(VodTest, EmptyListsAreValidAndCancellationStopsTheWorker)
{
    response("get_vod_streams", "[]");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.vod().loaded());
    EXPECT_TRUE(model.vod().catalog().empty());
    std::string series;
    ASSERT_TRUE(iptv::BuildXtreamSeriesUrl(account, "20", &series));
    reply(series, "{\"episodes\":[]}");
    model.vod().select(ptv::VodKind::episodes, "20");
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.vod().loaded());
    EXPECT_TRUE(model.vod().catalog().empty());
    host::set_network(true, "", 5000);
    model.vod().select(ptv::VodKind::movies, {}, true);
    model.poll();
    ASSERT_TRUE(model.vod().busy());
    const auto began = std::chrono::steady_clock::now();
    model.use_saved_source(1);
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(2));
    EXPECT_FALSE(model.vod().available());
    EXPECT_FALSE(model.vod().busy());
    model.close();
}
} // namespace
