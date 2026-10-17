// ProsperoTV - Provider VOD, saved copies, controller navigation and playback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/vod.hpp"
#include "tv/vod_screen.hpp"
#include "tv/playback_osd.hpp"
#include "iptv_input.h"
#include "host_platform.hpp"
#include "iptv_store.h"
#include <gtest/gtest.h>
#include <chrono>
#include <algorithm>
#include <ctime>
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

TEST_F(VodTest, HomeResumesOfflineWithoutLoadingVodAndRechecksHiddenAndParentalRules)
{
    std::string id;
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.vod().select(ptv::VodKind::movies);
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.play_vod(0));
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        id = request.channel_id;
        EXPECT_FALSE(model.finish_vod(request, 12000000, 100000000, false));
        model.close();
    }
    host::set_network(false, "");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    EXPECT_EQ(model.vod().catalog().size(), 0u);
    auto rows = model.home_items();
    ASSERT_EQ(rows[0].size(), 1u);
    EXPECT_EQ(rows[0][0].title, "Alpha movie");
    EXPECT_EQ(rows[0][0].position_us, 12000000u);
    ASSERT_TRUE(model.play_home(id, true));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.start_position_us, 12000000u);
    EXPECT_EQ(request.urls[0], "https://provider.invalid/sub/movie/user%40home/p%26ss/10.mkv");
    ASSERT_TRUE(model.hide_category("US / Movies", true));
    EXPECT_TRUE(model.home_items()[0].empty());
    EXPECT_FALSE(model.play_home(id, true));
    ASSERT_TRUE(model.hide_category("US / Movies", false));
    model.parental_action(ptv::Model::ParentalAction::set_pin);
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    model.parental_action(ptv::Model::ParentalAction::unlock);
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    ASSERT_TRUE(model.set_category_rule("US / Movies", 1));
    model.parental_action(ptv::Model::ParentalAction::lock);
    EXPECT_TRUE(model.home_items()[0].empty());
    EXPECT_FALSE(model.play_home(id, true));
    model.parental_action(ptv::Model::ParentalAction::unlock);
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    EXPECT_FALSE(model.home_items()[0].empty());
    EXPECT_FALSE(model.finish_vod(request, 100000000, 100000000, true));
    EXPECT_TRUE(model.home_items()[0].empty());
    model.close();
}

TEST_F(VodTest, HomeEpisodesRetainTheirShowsParentalPolicyAcrossRestart)
{
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.view.vod_series_group = "Comedy";
        model.view.vod_series_adult = true;
        model.view.vod_series_name = "Adult show";
        model.vod().select(ptv::VodKind::episodes, "20");
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.play_vod(0));
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        EXPECT_FALSE(model.finish_vod(request, 10000000, 100000000, false));
        model.close();
    }
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    ASSERT_EQ(model.home_items()[0].size(), 1u);
    const auto id = model.home_items()[0][0].id;
    model.view.vod_series_group = "Another category";
    model.view.vod_series_adult = false;
    ASSERT_TRUE(model.play_home(id, true));
    EXPECT_EQ(model.view.vod_series_group, "Another category");
    EXPECT_FALSE(model.view.vod_series_adult);
    ptv::PlayRequest resumed;
    ASSERT_TRUE(model.take_play_request(&resumed));
    EXPECT_EQ(resumed.series_id, "20");
    model.parental_action(ptv::Model::ParentalAction::set_pin);
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    EXPECT_TRUE(model.home_items()[0].empty());
    EXPECT_FALSE(model.play_home(id, true));
    model.close();
}

TEST_F(VodTest, ColdHomeEpisodeAutoplayUsesIndependentSavedSeries)
{
    std::string id;
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.view.vod_series_group = "Comedy";
        model.vod().select(ptv::VodKind::episodes, "20");
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.play_vod(2)); // Season 2, episode 2.
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        id = request.channel_id;
        model.finish_vod(request, 12000000, 100000000, false);
        model.vod().select(ptv::VodKind::movies);
        ASSERT_TRUE(settle(model));
        model.close();
    }
    host::set_network(false, "");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_EQ(model.vod().catalog().size(), 2u);
    ASSERT_TRUE(model.play_home(id, true));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.start_position_us, 12000000u);
    EXPECT_EQ(model.vod().catalog()[0].name, "Alpha movie");
    model.close(); // Foreground playback closes the menu workers.
    ASSERT_TRUE(model.finish_vod(request, 100000000, 100000000, true));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_NE(request.channel_id, id);
    EXPECT_EQ(request.series_id, "20");
    EXPECT_EQ(request.start_position_us, 0u);
    ASSERT_TRUE(model.hide_category("Comedy", true));
    EXPECT_FALSE(model.finish_vod(request, 100000000, 100000000, true));
    ASSERT_TRUE(model.hide_category("Comedy", false));
    auto stale = request;
    stale.source_id++;
    EXPECT_FALSE(model.finish_vod(stale, 100000000, 100000000, true));
    ASSERT_TRUE(model.finish_vod(request, 100000000, 100000000, true));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_FALSE(model.finish_vod(request, 100000000, 100000000, true));
    EXPECT_EQ(model.vod().catalog()[0].name, "Alpha movie");
}

TEST_F(VodTest, MissingHomeSeriesFetchCanCancelAndFallsBackToSavedEpisode)
{
    std::string id;
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.vod().select(ptv::VodKind::episodes, "20");
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.play_vod(2)); // Season 2, episode 2.
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        id = request.channel_id;
        model.finish_vod(request, 12000000, 100000000, false);
        model.close();
    }
    const auto series_cache = dir + "/prosperotv-xtream-catalog.sqlite3.vod-2-" +
                              std::to_string(iptv::CustomSourceId("20")) + ".sqlite3";
    ASSERT_TRUE(std::filesystem::remove(series_cache));
    host::set_network(true, "", 15000);
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(model.play_home(id, true));
    ptv::PlayRequest request;
    model.poll(); // Cancellation must also stop an active worker.
    EXPECT_FALSE(model.take_play_request(&request));
    std::string phone_response;
    EXPECT_EQ(model.remote_browse(R"({"operation":"list"})", phone_response), 409);
    EXPECT_NE(phone_response.find("episode loading"), phone_response.npos);
    ASSERT_TRUE(model.cancel_home_play());
    model.poll();
    EXPECT_FALSE(model.take_play_request(&request));
    ASSERT_TRUE(model.play_home(id, true));
    model.poll();
    model.use_saved_source(1);
    EXPECT_FALSE(model.take_play_request(&request));
    model.close();
    model.use_saved_source(3);
    ASSERT_TRUE(model.play_home(id, true));
    const auto started = std::chrono::steady_clock::now();
    bool ready = false;
    for (int i = 0; i < 2500 && !ready; ++i)
    {
        model.poll();
        ready = model.take_play_request(&request);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(ready);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(12));
    EXPECT_EQ(request.channel_id, id);
    EXPECT_FALSE(model.finish_vod(request, 100000000, 100000000, true));
    EXPECT_TRUE(model.vod().catalog().empty());
    model.save_vod_progress(id, 12000000, 100000000);
    host::set_network(true, "");
    ASSERT_TRUE(model.play_home(id, true));
    ready = false;
    for (int i = 0; i < 500 && !ready; ++i)
    {
        model.poll();
        ready = model.take_play_request(&request);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(ready);
    model.close();
    EXPECT_TRUE(model.finish_vod(request, 100000000, 100000000, true));
    EXPECT_TRUE(model.take_play_request(&request));
    EXPECT_NE(request.channel_id, id);
}

TEST_F(VodTest, HomeSnapshotBoundAndSourceFingerprintSurviveRestart)
{
    ptv::Library library;
    ASSERT_TRUE(library.open(dir + "/prosperotv-library.sqlite3"));
    for (int i = 0; i < 55; ++i)
    {
        iptv::Channel item;
        item.id = "movie-" + std::to_string(i);
        item.name = item.id;
        item.url = "https://stream.invalid/movie.mkv";
        item.group_title = "General";
        item.alternate_group_titles = {"Restricted", "US"};
        item.http_user_agent = "Test player";
        item.alternate_urls = {"https://backup.invalid/movie.mkv"};
        ASSERT_TRUE(library.remember_vod(3, 42, item, "series-1", 1000 + i));
        ASSERT_TRUE(library.save_playback_progress(item.id, 10, 100));
    }
    ASSERT_TRUE(library.close());
    ASSERT_TRUE(library.open(dir + "/prosperotv-library.sqlite3"));
    auto rows = library.unfinished_vod(3, 42);
    ASSERT_EQ(rows.size(), 50u);
    EXPECT_EQ(rows.front().channel.id, "movie-54");
    EXPECT_EQ(rows.back().channel.id, "movie-5");
    EXPECT_EQ(rows.front().channel.alternate_group_titles[0], "Restricted");
    EXPECT_EQ(rows.front().channel.http_user_agent, "Test player");
    EXPECT_EQ(rows.front().channel.alternate_urls.size(), 1u);
    EXPECT_EQ(rows.front().series, "series-1");
    EXPECT_TRUE(library.unfinished_vod(3, 43).empty());
    EXPECT_TRUE(library.unfinished_vod(2, 42).empty());
    iptv::Channel bad = rows.front().channel;
    EXPECT_FALSE(library.remember_vod(3, 42, bad, "group\nshow\nextra", 2000));
    EXPECT_FALSE(library.remember_vod(3, 42, bad, "group\nshow\r", 2000));
    EXPECT_FALSE(library.remember_vod(3, 42, bad, "group\n" + std::string(2049, 'x'), 2000));
    bad.url = "file:///secret";
    EXPECT_FALSE(library.remember_vod(3, 42, bad, "", 2000));
    ASSERT_TRUE(library.save_playback_progress("movie-54", 0, 100));
    EXPECT_EQ(library.unfinished_vod(3, 42).size(), 49u);
    auto source = library.sources().front();
    ASSERT_EQ(source.id, 3);
    source.password = "changed";
    ASSERT_TRUE(library.save_source(&source));
    EXPECT_TRUE(library.unfinished_vod(3, 42).empty());
    library.close();
}

TEST_F(VodTest, UnifiedSearchLoadsBothKindsWithoutChangingBrowseAndRoutesSafely)
{
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    const auto browse_id = std::string(model.vod().catalog()[0].id);
    model.begin_search();
    for (int i = 0; i < 600 && model.search_loading(); ++i)
    {
        model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_FALSE(model.search_loading()) << model.search_status();
    EXPECT_EQ(model.vod().catalog()[0].id, browse_id);
    auto movies = model.search_results("movie");
    ASSERT_EQ(movies.size(), 2u);
    EXPECT_EQ(movies[0].kind, ptv::Model::SearchResult::Kind::movie);
    auto shows = model.search_results("show");
    ASSERT_EQ(shows.size(), 1u);
    EXPECT_EQ(shows[0].kind, ptv::Model::SearchResult::Kind::show);
    ASSERT_TRUE(model.activate_search_vod(movies[0]));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.urls[0], "https://provider.invalid/sub/movie/user%40home/p%26ss/10.mkv");
    ASSERT_TRUE(model.hide_category("US / Movies", true));
    EXPECT_TRUE(model.search_results("movie").empty());
    EXPECT_FALSE(model.activate_search_vod(movies[0]));
    auto stale = shows[0];
    ++stale.source_id;
    EXPECT_FALSE(model.activate_search_vod(stale));
    ASSERT_TRUE(model.activate_search_vod(shows[0]));
    EXPECT_EQ(model.view.vod_series, "20");
    EXPECT_EQ(model.view.vod_kind, static_cast<int>(ptv::VodKind::episodes));
    model.end_search();
    EXPECT_TRUE(model.search_results("show").empty());
    EXPECT_FALSE(model.activate_search_vod(shows[0]));
    model.close();
}

TEST_F(VodTest, OpenSearchFollowsInitialAndReplacementPlaylistDownloads)
{
    const std::string url = "https://provider.invalid/list.m3u";
    {
        ptv::Library library;
        ASSERT_TRUE(library.open(dir + "/prosperotv-library.sqlite3"));
        ptv::SavedSource source{2, 1, "Playlist", url, {}, {}, {}, ptv::RefreshSchedule::manual};
        ASSERT_TRUE(library.save_source(&source));
        ASSERT_TRUE(library.select_source(2));
    }
    reply(url, "#EXTM3U\n#EXTINF:120,First film\nhttps://stream.invalid/movie/first.mp4\n");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.begin_search(); // The playlist has not reached the UI thread yet.
    ASSERT_TRUE(settle(model));
    auto results = model.search_results("film");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].title, "First film");
    reply(url, "#EXTM3U\n#EXTINF:120,Replacement film\nhttps://stream.invalid/movie/second.mp4\n");
    model.refresh();
    model.begin_search(); // Reopen the sheet while the replacement is downloading.
    ASSERT_TRUE(settle(model));
    results = model.search_results("film");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].title, "Replacement film");
    ASSERT_TRUE(model.activate_search_vod(results[0]));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.urls[0], "https://stream.invalid/movie/second.mp4");
    model.close();
}

TEST_F(VodTest, ParentalPolicyAppliesToProviderFlagsAndEpisodesOfTheSelectedShow)
{
    response(
        "get_vod_categories",
        R"([{"category_id":3,"category_name":"Kids","is_adult":1},{"category_id":2,"category_name":"General"}])");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    ASSERT_EQ(model.vod().catalog().size(), 2u);
    EXPECT_TRUE(model.vod().catalog()[0].adult);
    model.parental_action(ptv::Model::ParentalAction::set_pin);
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    model.begin_search();
    for (int i = 0; i < 600 && model.search_loading(); ++i)
    {
        model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_FALSE(model.search_loading());
    EXPECT_TRUE(model.search_results("Alpha").empty());
    EXPECT_EQ(model.search_results("Beta").size(), 1u);
    model.end_search();
    EXPECT_FALSE(model.vod_allowed(0));
    EXPECT_FALSE(model.play_vod(0));
    EXPECT_TRUE(model.vod_allowed(1));
    model.parental_action(ptv::Model::ParentalAction::unlock);
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    ASSERT_TRUE(model.parental().unlocked());
    EXPECT_TRUE(model.play_vod(0));
    model.view.vod_series_group = "Kids";
    model.view.vod_series_adult = true;
    model.vod().select(ptv::VodKind::episodes, "20");
    ASSERT_TRUE(settle(model));
    ASSERT_EQ(model.vod().catalog().size(), 3u);
    model.parental_action(ptv::Model::ParentalAction::lock);
    EXPECT_FALSE(model.vod_allowed(0));
    EXPECT_FALSE(model.play_vod(0));
    model.parental_action(ptv::Model::ParentalAction::unlock);
    model.poll();
    host::set_keyboard_text("123456");
    model.poll();
    model.parental_action(ptv::Model::ParentalAction::kids);
    EXPECT_FALSE(model.vod_allowed(0));
    model.view.vod_series_adult = false;
    EXPECT_TRUE(model.vod_allowed(0));
    model.view.vod_series_group = "Comedy";
    EXPECT_FALSE(model.play_vod(0));
    model.close();
}

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

TEST_F(VodTest, KeepsTheFirstTitlesOfAListLongerThanTheLimit)
{
    response("get_vod_categories", R"([{"category_id":3,"category_name":"Movies"}])");
    response(
        "get_vod_streams",
        R"([{"stream_id":10,"name":"First","category_id":3},{"stream_id":11,"name":"Second","category_id":3},{"stream_id":12,"name":"Third","category_id":3}])");
    iptv::Catalog catalog;
    std::string error;
    bool truncated = true;
    ASSERT_TRUE(ptv::load_vod(account, ptv::VodKind::movies, {}, &catalog, nullptr, &error, nullptr,
                              nullptr, &truncated, 2))
        << error;
    EXPECT_TRUE(truncated);
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(catalog[0].name, "First");
    EXPECT_EQ(catalog[1].name, "Second");
    EXPECT_TRUE(error.empty());
    ASSERT_TRUE(ptv::load_vod(account, ptv::VodKind::movies, {}, &catalog, nullptr, &error, nullptr,
                              nullptr, &truncated, 3))
        << error;
    EXPECT_FALSE(truncated);
    EXPECT_EQ(catalog.size(), 3u);
}

TEST_F(VodTest, LeavesOutAnEntryTheReaderRefusesAndKeepsTheRest)
{
    response("get_vod_categories", R"([{"category_id":3,"category_name":"Movies"}])");
    const std::string oversized(iptv::kDefaultMaxFieldBytes + 1, 'k');
    response("get_vod_streams", R"([{"stream_id":10,"name":"First","category_id":3},)"
                                R"({"stream_id":11,"name":")" +
                                    oversized + R"(","category_id":3},)" +
                                    R"({"stream_id":12,"name":"Third","category_id":3}])");
    iptv::Catalog catalog;
    std::string error;
    bool truncated = true;
    ASSERT_TRUE(ptv::load_vod(account, ptv::VodKind::movies, {}, &catalog, nullptr, &error, nullptr,
                              nullptr, &truncated))
        << error;
    EXPECT_FALSE(truncated);
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(catalog[0].name, "First");
    EXPECT_EQ(catalog[1].name, "Third");
}

TEST_F(VodTest, LoadsBomPrefixedListsAndIndexedSeasonArrays)
{
    response("get_vod_categories",
             "\xef\xbb\xbf"
             R"([{"category_id":3,"category_name":"Movies","parent_id":null}])");
    response(
        "get_vod_streams",
        "\xef\xbb\xbf"
        R"({"data":[{"stream_id":10,"name":"A\u0009movie","container_extension":null,"stream_icon":null,"category_id":3},{"stream_id":null,"name":"Missing"}]})");
    iptv::Catalog catalog;
    std::string error;
    ASSERT_TRUE(ptv::load_vod(account, ptv::VodKind::movies, {}, &catalog, nullptr, &error))
        << error;
    ASSERT_EQ(catalog.size(), 1u);
    EXPECT_EQ(catalog[0].name, "A movie");
    EXPECT_TRUE(catalog[0].tvg_logo.empty());
    EXPECT_EQ(catalog[0].url, "https://provider.invalid/sub/movie/user%40home/p%26ss/10.mp4");
    std::string series;
    ASSERT_TRUE(iptv::BuildXtreamSeriesUrl(account, "20", &series));
    reply(
        series,
        "\xef\xbb\xbf"
        R"({"episodes":[[],[{"id":200,"title":"Pilot","episode_num":1,"container_extension":null}],[{"id":201,"title":"Return","episode_num":1,"season":5}]]})");
    ASSERT_TRUE(ptv::load_vod(account, ptv::VodKind::episodes, "20", &catalog, nullptr, &error))
        << error;
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(catalog[0].group_title, "Season 1");
    EXPECT_EQ(catalog[0].source_line, 100001u);
    EXPECT_EQ(catalog[1].group_title, "Season 5");
    EXPECT_TRUE(catalog[0].url.ends_with("/200.mp4"));
    reply(series, R"({"episodes":[[],[{"id":200,"title":"Pilot"}],false]})");
    EXPECT_FALSE(ptv::load_vod(account, ptv::VodKind::episodes, "20", &catalog, nullptr, &error));
    EXPECT_TRUE(catalog.empty());
}

TEST_F(VodTest, LongProviderTextIsCutShortInsteadOfFailingTheList)
{
    // One show of thousands with a cast list longer than is kept used to fail the whole list.
    std::string cast;
    for (int i = 0; cast.size() < 1268; ++i)
        cast += (cast.empty() ? "" : ", ") + std::string("Actor Number ") + std::to_string(i);
    response("get_series_categories", R"([{"category_id":4,"category_name":"Comedy"}])");
    response("get_series", R"([{"series_id":21,"name":"Long cast","category_id":4,"cast":")" +
                               cast + R"(","genre":")" + std::string(300, 'g') +
                               R"(","cover":"data:image/png;base64,)" + std::string(5000, 'A') +
                               R"("},{"series_id":22,"name":"Next show","category_id":4}])");
    iptv::Catalog catalog;
    ptv::VodMetadata metadata;
    std::string error;
    ASSERT_TRUE(
        ptv::load_vod(account, ptv::VodKind::series, {}, &catalog, nullptr, &error, &metadata))
        << error;
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(catalog[1].name, "Next show");
    const auto &details = metadata[std::string(catalog[0].id)];
    EXPECT_LE(details.cast.size(), 1024u);
    EXPECT_GT(details.cast.size(), 900u);
    EXPECT_EQ(cast.compare(0, details.cast.size(), details.cast), 0);
    EXPECT_NE(details.cast.back(), ' ');
    EXPECT_LE(details.genre.size(), 256u);
    EXPECT_TRUE(catalog[0].tvg_logo.empty());
}

TEST_F(VodTest, OverviewCountsTitlesAndDisablesOnlyConfirmedEmptyLists)
{
    response("get_series", "[]");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    const hui::ui::Fonts fonts{};
    ptv::Shared shared(model, fonts, {});
    ptv::VodScreen screen(shared);
    hui::ui::Feedback feedback;
    screen.enter();
    EXPECT_FALSE(model.vod_count(ptv::VodKind::movies).has_value());
    EXPECT_FALSE(model.vod_count(ptv::VodKind::series).has_value());
    for (int i = 0; i < 500 && model.search_loading(); ++i)
    {
        model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_FALSE(model.search_loading());
    screen.update();
    EXPECT_EQ(model.vod_count(ptv::VodKind::movies), 2u);
    EXPECT_EQ(model.vod_count(ptv::VodKind::series), 0u);
    hui::InputFrame input;
    input.nav = hui::Direction::down;
    screen.handle(input, feedback);
    input.nav = hui::Direction::none;
    input.pressed = hui::action_bit(hui::Action::confirm);
    screen.handle(input, feedback);
    EXPECT_EQ(model.view.vod_kind, -1);
    ASSERT_TRUE(model.hide_category("US", true));
    screen.update();
    EXPECT_EQ(model.vod_count(ptv::VodKind::movies), 0u);
    model.view.vod_focus = 0;
    screen.handle(input, feedback);
    EXPECT_EQ(model.view.vod_kind, -1);
    ASSERT_TRUE(model.hide_category("US", false));
    screen.update();
    screen.handle(input, feedback);
    EXPECT_EQ(model.view.vod_kind, static_cast<int>(ptv::VodKind::movies));
    ASSERT_TRUE(settle(model));
    // A refresh from inside Movies must be reflected when returning to the root.
    response("get_vod_streams", "[]");
    screen.refresh();
    ASSERT_TRUE(settle(model));
    ASSERT_TRUE(model.vod().catalog().empty()) << model.vod().status();
    ASSERT_TRUE(screen.back());
    ASSERT_EQ(model.view.vod_kind, -1);
    EXPECT_EQ(model.vod_count(ptv::VodKind::movies), 0u);
    model.use_saved_source(1);
    screen.update();
    EXPECT_EQ(model.vod_count(ptv::VodKind::movies), 0u);
    EXPECT_EQ(model.vod_count(ptv::VodKind::series), 0u);
    screen.handle(input, feedback);
    EXPECT_EQ(model.view.vod_kind, -1);
    model.close();
}

TEST_F(VodTest, OverviewFailureIsUnknownAndRefreshCanRecoverFromEmpty)
{
    response("get_vod_streams", "not json");
    response("get_series", "[]");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    const hui::ui::Fonts fonts{};
    ptv::Shared shared(model, fonts, {});
    ptv::VodScreen screen(shared);
    screen.enter();
    const auto wait = [&]
    {
        for (int i = 0; i < 500 && model.search_loading(); ++i)
        {
            model.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_FALSE(model.search_loading());
        screen.update();
    };
    wait();
    EXPECT_FALSE(model.vod_count(ptv::VodKind::movies).has_value());
    EXPECT_EQ(model.vod_count(ptv::VodKind::series), 0u);
    response("get_vod_streams",
             R"([{"stream_id":12,"name":"New film","container_extension":"mp4"}])");
    response("get_series", R"([{"series_id":21,"name":"New show"}])");
    screen.refresh();
    wait();
    EXPECT_EQ(model.vod_count(ptv::VodKind::movies), 1u);
    EXPECT_EQ(model.vod_count(ptv::VodKind::series), 1u);
    // Opening and closing search must not cancel the overview's shared loader.
    model.begin_search();
    model.end_search();
    EXPECT_FALSE(model.searching());
    EXPECT_EQ(model.vod_count(ptv::VodKind::series), 1u);
    model.close();
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
    EXPECT_FALSE(request.live);
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

TEST_F(VodTest, ResumeSurvivesRestartAndCompletedEpisodesAdvanceInNumericOrder)
{
    std::string movie;
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.vod().select(ptv::VodKind::movies);
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.play_vod(0));
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        EXPECT_TRUE(request.on_demand);
        movie = request.channel_id;
        EXPECT_EQ(request.start_position_us, 0u);
        EXPECT_FALSE(model.finish_vod(request, 12000000, 100000000, false));
        EXPECT_FALSE(model.save_vod_progress(request.channel_id, 100000001, 100000000));
        EXPECT_FALSE(model.save_vod_progress("", 12000000, 100000000));
        model.close();
    }
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    ASSERT_TRUE(model.play_vod(0));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.channel_id, movie);
    EXPECT_EQ(request.start_position_us, 12000000u);
    EXPECT_FALSE(model.finish_vod(request, 99000000, 100000000, true));
    ASSERT_TRUE(model.play_vod(0));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.start_position_us, 0u);
    model.vod().select(ptv::VodKind::episodes, "20");
    ASSERT_TRUE(settle(model));
    ASSERT_TRUE(model.play_vod(2)); // Season 2, episode 2.
    ASSERT_TRUE(model.take_play_request(&request));
    ASSERT_TRUE(model.finish_vod(request, 99000000, 100000000, true));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_TRUE(request.urls[0].ends_with("/201.mkv")); // Season 2, episode 10.
    ASSERT_TRUE(model.finish_vod(request, 99000000, 100000000, true));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_TRUE(request.urls[0].ends_with("/200.mp4")); // Season 10.
    EXPECT_FALSE(model.finish_vod(request, 99000000, 100000000, true));
    EXPECT_FALSE(model.take_play_request(&request));
    model.close();
}

TEST_F(VodTest, FilmControlsClampSkipsPauseAndSelectChapters)
{
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    ptv::PlayRequest request;
    request.on_demand = true;
    request.record_channel_result = false;
    ptv::PlaybackOsd controls(model, request);
    iptv_player_media_state_t state{};
    state.available = state.seekable = 1;
    state.position_us = 30000000;
    state.duration_us = 100000000;
    state.chapter_count = 2;
    state.chapter_positions[1] = 50000000;
    controls.set_media_state(state);
    // Seeking reopens the video: presses add up, and one seek is asked for once they stop.
    EXPECT_EQ(controls.input(IPTV_INPUT_RIGHT, 1), 1);
    EXPECT_FALSE(controls.take_media_seek());
    controls.input(-1, 600000);
    EXPECT_FALSE(controls.take_media_seek());
    controls.input(-1, 700001);
    EXPECT_EQ(controls.take_media_seek(), 40000000u);
    EXPECT_FALSE(controls.take_media_seek());
    // The next press goes on from there, though the picture has not got there yet.
    EXPECT_EQ(controls.input(IPTV_INPUT_RIGHT, 800000), 1);
    EXPECT_EQ(controls.input(IPTV_INPUT_RIGHT, 900000), 1);
    EXPECT_EQ(controls.input(IPTV_INPUT_LEFT, 1000000), 1);
    controls.input(-1, 1700001);
    EXPECT_EQ(controls.take_media_seek(), 50000000u);
    EXPECT_EQ(controls.input(IPTV_INPUT_L1, 2000000), 1);
    controls.input(-1, 2700001);
    EXPECT_EQ(controls.take_media_seek(), 0u);
    EXPECT_EQ(controls.input(IPTV_INPUT_R1, 3000000), 1);
    EXPECT_EQ(controls.input(IPTV_INPUT_R1, 3100000), 1);
    controls.input(-1, 3800001);
    EXPECT_EQ(controls.take_media_seek(), 99999999u); // clamped to the end
    // Once the picture is where it was sent, presses go on from the picture again.
    state.position_us = state.duration_us - 1;
    state.presented = 1;
    controls.set_media_state(state);
    state.position_us = 30000000;
    controls.set_media_state(state);
    EXPECT_EQ(controls.input(IPTV_INPUT_PLAY_PAUSE, 5), 1);
    EXPECT_EQ(controls.take_media_pause(), true);
    EXPECT_EQ(controls.input(IPTV_INPUT_OPTIONS, 6), 1);
    EXPECT_EQ(controls.input(IPTV_INPUT_UP, 7), 1);    // Chapters, the last row.
    EXPECT_EQ(controls.input(IPTV_INPUT_CROSS, 7), 1); // At the chapter playing.
    EXPECT_EQ(controls.input(IPTV_INPUT_DOWN, 8), 1);
    EXPECT_EQ(controls.input(IPTV_INPUT_CROSS, 9), 1);
    EXPECT_EQ(controls.take_media_seek(), 50000000u);
    EXPECT_EQ(controls.input(IPTV_INPUT_SQUARE, 10), 1);
    EXPECT_EQ(controls.take_media_seek(), 0u);
    for (const unsigned speed : {125u, 150u, 200u, 50u, 75u, 100u})
    {
        EXPECT_EQ(controls.input(IPTV_INPUT_GO_LIVE, 10), 1);
        EXPECT_EQ(controls.take_media_speed(), speed);
        EXPECT_FALSE(controls.take_media_speed());
    }
    state.seekable = 0;
    controls.set_media_state(state);
    EXPECT_EQ(controls.input(IPTV_INPUT_RIGHT, 10), 0);
    EXPECT_FALSE(controls.take_media_seek());
    model.close();
}

TEST_F(VodTest, SeekInsideTheOpenFilmShowsTheTargetUntilThePictureGetsThere)
{
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    ptv::PlayRequest request;
    request.on_demand = true;
    request.record_channel_result = false;
    ptv::PlaybackOsd controls(model, request);
    iptv_player_media_state_t state{};
    state.available = state.seekable = state.presented = 1;
    state.position_us = 30000000;
    state.duration_us = 100000000;
    controls.set_media_state(state);
    EXPECT_EQ(controls.input(IPTV_INPUT_RIGHT, 1), 1);
    controls.input(-1, 700001);
    EXPECT_EQ(controls.take_media_seek(), 40000000u);
    controls.set_media_state(state);
    EXPECT_EQ(controls.input(IPTV_INPUT_RIGHT, 800000), 1);
    controls.input(-1, 1500001);
    EXPECT_EQ(controls.take_media_seek(), 50000000u);
    state.position_us = 50000000;
    controls.set_media_state(state);
    state.position_us = 55000000;
    controls.set_media_state(state);
    EXPECT_EQ(controls.input(IPTV_INPUT_RIGHT, 2000000), 1);
    controls.input(-1, 2700001);
    EXPECT_EQ(controls.take_media_seek(), 65000000u);
    model.close();
}

TEST_F(VodTest, SoundDelayChangesOnlyInsideAudioMenuAndIsBounded)
{
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    ptv::PlaybackOsd controls(model, {});
    EXPECT_FALSE(controls.take_audio_delay());
    EXPECT_EQ(controls.input(IPTV_INPUT_OPTIONS, 1), 1);
    EXPECT_EQ(controls.input(IPTV_INPUT_GO_LIVE, 2), 1);
    EXPECT_EQ(controls.take_audio_delay(), 50);
    EXPECT_FALSE(controls.take_audio_delay());
    EXPECT_EQ(controls.input(IPTV_INPUT_PLAY_PAUSE, 3), 1);
    EXPECT_EQ(controls.take_audio_delay(), 0);
    for (unsigned i = 0; i < 45; ++i)
        controls.input(IPTV_INPUT_PLAY_PAUSE, 4 + i);
    EXPECT_EQ(controls.take_audio_delay(), -2000);
    for (unsigned i = 0; i < 90; ++i)
        controls.input(IPTV_INPUT_GO_LIVE, 50 + i);
    EXPECT_EQ(controls.take_audio_delay(), 2000);
    EXPECT_EQ(controls.input(IPTV_INPUT_SQUARE, 150), 1);
    EXPECT_EQ(controls.take_audio_delay(), 0);
    // Left and Right on Sound delay are fine steps; L2 and R2 coarse ones.
    EXPECT_EQ(controls.input(IPTV_INPUT_RIGHT, 151), 1);
    EXPECT_EQ(controls.take_audio_delay(), 10);
    controls.input(IPTV_INPUT_LEFT, 152);
    controls.input(IPTV_INPUT_LEFT, 153);
    EXPECT_EQ(controls.take_audio_delay(), -10);
    controls.input(IPTV_INPUT_PLAY_PAUSE, 154);
    EXPECT_EQ(controls.take_audio_delay(), -60);
    EXPECT_EQ(controls.input(IPTV_INPUT_SQUARE, 155), 1);
    EXPECT_EQ(controls.take_audio_delay(), 0);
    EXPECT_EQ(controls.input(IPTV_INPUT_CIRCLE, 156), 1);
    controls.input(IPTV_INPUT_GO_LIVE, 157);
    EXPECT_FALSE(controls.take_audio_delay());
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
    EXPECT_EQ(model.vod().catalog().size(), 0u);
    std::string series;
    ASSERT_TRUE(iptv::BuildXtreamSeriesUrl(account, "20", &series));
    reply(series, "{\"episodes\":[]}");
    model.vod().select(ptv::VodKind::episodes, "20");
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.vod().loaded());
    EXPECT_EQ(model.vod().catalog().size(), 0u);
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

TEST_F(VodTest, ProviderMetadataSurvivesCacheAndSeriesInfoInheritsIntoEpisodes)
{
    response(
        "get_vod_streams",
        R"([{"stream_id":10,"name":"Alpha","genre":"Drama / Mystery","releaseDate":"2024-03-09","rating":"8.2","rating_5based":1,"plot":"Provider synopsis","cast":["One","Two"]}])");
    iptv::Catalog catalog;
    ptv::VodMetadata metadata;
    std::string error;
    ASSERT_TRUE(
        ptv::load_vod(account, ptv::VodKind::movies, {}, &catalog, nullptr, &error, &metadata));
    ASSERT_EQ(catalog.size(), 1u);
    const auto id = std::string(catalog[0].id);
    ASSERT_EQ(metadata.at(id).year, "2024");
    EXPECT_EQ(catalog[0].media_kind, iptv::MediaKind::movie);
    EXPECT_EQ(metadata.at(id).cast, "One, Two");
    EXPECT_EQ(ptv::vod_genres(metadata.at(id).genre).size(), 2u);
    ASSERT_EQ(iptv::SaveCatalog(dir + "/details.sqlite3", catalog), iptv::StoreStatus::ok);
    ASSERT_TRUE(ptv::save_vod_metadata(dir + "/details.sqlite3", metadata));
    const auto cached = ptv::load_vod_metadata(dir + "/details.sqlite3", catalog);
    EXPECT_EQ(cached.at(id).plot, "Provider synopsis");
    EXPECT_EQ(cached.at(id).rating, "8.2");
    std::string url;
    ASSERT_TRUE(iptv::BuildXtreamSeriesUrl(account, "20", &url));
    reply(
        url,
        R"({"episodes":{"1":[{"id":200,"title":"Pilot","info":{"plot":"Episode synopsis"}},{"id":201,"title":"Return","info":null}]},"info":{"plot":"Show synopsis","cast":"Actor","genre":"Comedy","rating_5based":4.5,"releaseDate":"2022-01-01","cover":"https://image.invalid/show.png"}})");
    ASSERT_TRUE(
        ptv::load_vod(account, ptv::VodKind::episodes, "20", &catalog, nullptr, &error, &metadata));
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(metadata.at(std::string(catalog[0].id)).plot, "Episode synopsis");
    EXPECT_EQ(metadata.at(std::string(catalog[1].id)).plot, "Show synopsis");
    EXPECT_EQ(metadata.at(std::string(catalog[1].id)).year, "2022");
    EXPECT_EQ(metadata.at(std::string(catalog[1].id)).rating, "9");
    EXPECT_EQ(catalog[1].media_kind, iptv::MediaKind::episode);
    EXPECT_EQ(metadata.at(std::string(catalog[1].id)).poster, "https://image.invalid/show.png");
}
TEST_F(VodTest, MovieDetailsFetchLazilyAndRemainAvailableOfflineAfterRefresh)
{
    std::string info;
    ASSERT_TRUE(iptv::BuildXtreamApiUrl(account, "get_vod_info", &info));
    reply(
        info + "&vod_id=10",
        R"({"info":{"plot":"Full plot","cast":"Actor A","genre":"Drama","releasedate":"2023-05-02","rating":7.5,"movie_image":"https://image.invalid/poster.jpg"},"movie_data":{"stream_id":10}})");
    std::string id;
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.vod().select(ptv::VodKind::movies);
        ASSERT_TRUE(settle(model));
        id = model.vod().catalog()[0].id;
        EXPECT_FALSE(model.vod().details(id).complete);
        model.vod().request_details(0);
        ASSERT_TRUE(settle(model));
        EXPECT_TRUE(model.vod().details(id).complete);
        EXPECT_EQ(model.vod().details(id).plot, "Full plot");
        model.vod().select(ptv::VodKind::movies, {}, true);
        ASSERT_TRUE(settle(model));
        EXPECT_EQ(model.vod().details(id).plot, "Full plot");
        model.close();
    }
    host::set_network(false, "");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    EXPECT_EQ(model.vod().details(id).genre, "Drama");
    EXPECT_EQ(model.vod().details(id).year, "2023");
    EXPECT_EQ(model.vod().details(id).poster, "https://image.invalid/poster.jpg");
    model.close();
}
TEST_F(VodTest, DetailsAskedWhileTheListUpdatesArriveInsteadOfBeingDropped)
{
    // Issue 67: details asked for while the list was due an update, or while it or the guide
    // downloaded, were dropped, and the panel said the provider gave no synopsis.
    std::string info;
    ASSERT_TRUE(iptv::BuildXtreamApiUrl(account, "get_vod_info", &info));
    reply(info + "&vod_id=10", R"({"info":{"plot":"Arrives anyway"},"movie_data":{"stream_id":10}})");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    const std::string id(model.vod().catalog()[0].id);
    model.vod().select(ptv::VodKind::movies, {}, true); // an update of the list is now due
    ASSERT_TRUE(model.vod().requested());
    model.vod().request_details(0);
    EXPECT_TRUE(model.vod().details_loading(id));
    EXPECT_FALSE(model.vod().details_loading(std::string(model.vod().catalog()[1].id)));
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.vod().details(id).complete);
    EXPECT_EQ(model.vod().details(id).plot, "Arrives anyway");
    EXPECT_FALSE(model.vod().details_loading(id));
    EXPECT_EQ(model.vod().catalog().size(), 2u); // and the list update ran too
    // Asked for again while a download runs: still kept, and fetched after it.
    std::string other(model.vod().catalog()[1].id);
    reply(info + "&vod_id=11", R"({"info":{"plot":"Second"}})");
    model.vod().select(ptv::VodKind::movies, {}, true);
    for (int i = 0; i < 200 && !model.vod().busy(); ++i)
        model.poll();
    model.vod().request_details(1);
    EXPECT_TRUE(model.vod().details_loading(other));
    ASSERT_TRUE(settle(model));
    EXPECT_EQ(model.vod().details(other).plot, "Second");
    model.close();
}
TEST_F(VodTest, PlaylistFilmsFromAnXtreamServerGetTheirDetailsFromIt)
{
    // M3U Plus from an Xtream server names each film <server>/movie/<user>/<password>/<id>.<ext>;
    // the same server answers get_vod_info for it with that account.
    const std::string playlist = dir + "/plus.m3u";
    std::ofstream(playlist)
        << "#EXTM3U\n"
           "#EXTINF:-1 group-title=\"Movies\",Film\n"
           "https://provider.invalid/sub/movie/user%40home/p%26ss/42.mkv\n"
           "#EXTINF:-1 group-title=\"Movies\",Elsewhere\n"
           "https://cdn.invalid/films/elsewhere.mp4\n";
    const std::string url =
        "https://provider.invalid/sub/get.php?username=user%40home&password=p%26ss&type=m3u_plus";
    host::set_network_response(url, playlist);
    std::string info;
    ASSERT_TRUE(iptv::BuildXtreamApiUrl(account, "get_vod_info", &info));
    reply(info + "&vod_id=42", R"({"info":{"plot":"From the server","genre":"Drama"}})");
    {
        ptv::Library library;
        ASSERT_TRUE(library.open(dir + "/prosperotv-library.sqlite3"));
        ptv::SavedSource source{4, 1, "Plus", url, {}, {}, {}, ptv::RefreshSchedule::manual};
        ASSERT_TRUE(library.save_source(&source));
        ASSERT_TRUE(library.select_source(4));
    }
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(settle(model));
    auto &vod = model.vod();
    vod.select(ptv::VodKind::movies);
    ASSERT_EQ(vod.catalog().size(), 2u);
    for (unsigned i = 0; i < vod.catalog().size(); ++i)
    {
        const std::string id(vod.catalog()[i].id);
        const bool from_server = vod.catalog()[i].name == std::string_view("Film");
        vod.request_details(i);
        EXPECT_EQ(vod.details_loading(id), from_server);
        ASSERT_TRUE(settle(model));
        EXPECT_EQ(vod.details(id).plot, from_server ? "From the server" : "");
        EXPECT_EQ(vod.details(id).complete, from_server);
    }
    model.close();
}
TEST_F(VodTest, ControllerFiltersByProviderGenreAndYearSortsAndOpensDetails)
{
    response(
        "get_vod_streams",
        R"([{"stream_id":10,"name":"Alpha","genre":"Drama","year":"2020","rating":"5"},{"stream_id":11,"name":"Beta","genre":"Comedy","year":"2024","rating":"9"}])");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.view.vod_kind = static_cast<int>(ptv::VodKind::movies);
    model.view.vod_all = true;
    hui::gfx::Font font;
    std::ifstream font_file(std::string(std::getenv("KIT_FONTS")) + "/inter-regular.huifont",
                            std::ios::binary);
    const std::string font_bytes{std::istreambuf_iterator<char>(font_file), {}};
    ASSERT_TRUE(font.load(font_bytes));
    hui::ui::Fonts fonts{};
    fonts.regular.font = &font;
    fonts.regular.texture = 1;
    fonts.semibold = fonts.display = fonts.mono = fonts.hand = fonts.pixel = fonts.regular;
    ptv::Shared shared(model, fonts, {});
    ptv::VodScreen screen(shared);
    hui::ui::Feedback feedback;
    const auto press = [&](hui::Action action)
    {
        hui::InputFrame input;
        input.pressed = hui::action_bit(action);
        screen.handle(input, feedback);
    };
    const auto move = [&](hui::Direction direction)
    {
        hui::InputFrame input;
        input.nav = direction;
        screen.handle(input, feedback);
    };
    screen.enter();
    ASSERT_TRUE(settle(model));
    screen.update();
    ASSERT_EQ(screen.result_count(), 2u);
    press(hui::Action::r3);
    ASSERT_TRUE(screen.filters_open());
    hui::gfx::DrawList list;
    hui::ui::Canvas canvas{list, fonts, 0, 0};
    screen.draw(canvas);
    EXPECT_FALSE(list.empty());
    move(hui::Direction::right);
    EXPECT_EQ(model.view.vod_genre, "Comedy");
    EXPECT_EQ(screen.result_count(), 1u);
    press(hui::Action::west);
    move(hui::Direction::down);
    move(hui::Direction::right);
    EXPECT_EQ(model.view.vod_year, "2020");
    EXPECT_EQ(screen.result_count(), 1u);
    press(hui::Action::west);
    move(hui::Direction::down);
    move(hui::Direction::right);
    move(hui::Direction::right);
    move(hui::Direction::right);
    EXPECT_EQ(model.view.vod_sort, 3); // Newest year
    press(hui::Action::confirm);
    press(hui::Action::west);
    EXPECT_TRUE(screen.details_open());
    screen.draw(canvas);
    ASSERT_TRUE(screen.back());
    EXPECT_FALSE(screen.details_open());
    press(hui::Action::confirm);
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.channel_name, "Beta");
    model.close();
}

TEST_F(VodTest, NewestAdditionsComeFirstAndUndatedTitlesFollowByTitle)
{
    response("get_vod_streams",
             R"([{"stream_id":1,"name":"Undated","category_id":3},)"
             R"({"stream_id":2,"name":"Old","added":"1500000000","category_id":3},)"
             R"({"stream_id":3,"name":"Newest","added":1700000000,"category_id":2},)"
             R"({"stream_id":4,"name":"Zed tie","added":"1600000000","category_id":3},)"
             R"({"stream_id":5,"name":"Words","added":"yesterday","category_id":3},)"
             R"({"stream_id":6,"name":"Ace tie","added":"1600000000.0","category_id":3},)"
             R"({"stream_id":7,"name":"Negative","added":-5,"category_id":3},)"
             R"({"stream_id":8,"name":"Far off","added":"99999999999","category_id":3},)"
             R"({"stream_id":9,"name":"Null","added":null,"category_id":3},)"
             R"({"stream_id":10,"name":"Object","added":{"at":1},"category_id":3},)"
             R"({"stream_id":11,"name":"Overlong","added":"1600000000000000000000000000000000",)"
             R"("category_id":3},)"
             R"({"stream_id":12,"name":"Middle","added":"1650000000","category_id":3}])");
    response("get_series",
             R"([{"series_id":20,"name":"Old show","last_modified":"1500000000","category_id":4},)"
             R"({"series_id":21,"name":"Added only","added":"1600000000","category_id":4},)"
             R"({"series_id":22,"name":"New show","last_modified":1690000000,"added":"1",)"
             R"("category_id":4},{"series_id":23,"name":"A undated show","category_id":4}])");
    const std::vector<std::string> newest{"Newest", "Middle",   "Zed tie",  "Ace tie",
                                          "Old",    "Far off",  "Negative", "Null",
                                          "Object", "Overlong", "Undated",  "Words"};
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        const hui::ui::Fonts fonts{};
        ptv::Shared shared(model, fonts, {});
        ptv::VodScreen screen(shared);
        model.view.vod_kind = static_cast<int>(ptv::VodKind::movies);
        model.view.vod_all = true;
        screen.enter();
        ASSERT_TRUE(settle(model));
        screen.update();
        // The catalog keeps the provider's order; only the list shown is sorted.
        const auto &catalog = model.vod().catalog();
        ASSERT_EQ(catalog.size(), 12u);
        EXPECT_EQ(catalog[0].name, "Undated");
        EXPECT_EQ(model.vod().added(0), 0u);
        EXPECT_EQ(model.vod().added(1), 1500000000u);
        EXPECT_EQ(model.vod().added(2), 1700000000u);
        EXPECT_EQ(model.vod().added(5), 1600000000u);
        for (const unsigned undated : {4u, 6u, 7u, 8u, 9u, 10u})
            EXPECT_EQ(model.vod().added(undated), 0u) << catalog[undated].name;
        EXPECT_EQ(model.vod().added(99), 0u);
        EXPECT_EQ(model.view.vod_sort, 0);
        EXPECT_EQ(screen.result_titles(), newest);
        // One category, and a search, are in the same order.
        model.view.vod_all = false;
        model.view.vod_category = "US / Movies / Kids";
        screen.enter();
        auto kids = newest;
        kids.erase(kids.begin());
        EXPECT_EQ(screen.result_titles(), kids);
        model.view.vod_category.clear();
        model.view.vod_query = "tie";
        screen.enter();
        EXPECT_EQ(screen.result_titles(), (std::vector<std::string>{"Zed tie", "Ace tie"}));
        model.view.vod_query.clear();
        // Title order is still one choice away.
        model.view.vod_all = true;
        model.view.vod_sort = 1;
        screen.enter();
        auto titles = newest;
        std::sort(titles.begin(), titles.end());
        EXPECT_EQ(screen.result_titles(), titles);
        model.view.vod_sort = 0;
        // A show's date is when its list entry last changed, or else when it was added.
        model.view.vod_kind = static_cast<int>(ptv::VodKind::series);
        screen.enter();
        ASSERT_TRUE(settle(model));
        screen.update();
        EXPECT_EQ(screen.result_titles(), (std::vector<std::string>{"New show", "Added only",
                                                                    "Old show", "A undated show"}));
        model.close();
    }
    // The dates are kept with the saved list.
    host::set_network(false, "");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    const hui::ui::Fonts fonts{};
    ptv::Shared shared(model, fonts, {});
    ptv::VodScreen screen(shared);
    model.view.vod_kind = static_cast<int>(ptv::VodKind::movies);
    model.view.vod_all = true;
    screen.enter();
    ASSERT_TRUE(settle(model));
    screen.update();
    EXPECT_EQ(screen.result_titles(), newest);
    model.close();
}
TEST_F(VodTest, MalformedMovieDetailsKeepPreviousMetadataAndDoNotComplete)
{
    std::string info;
    ASSERT_TRUE(iptv::BuildXtreamApiUrl(account, "get_vod_info", &info));
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    const std::string id(model.vod().catalog()[0].id);
    std::string deep;
    for (int i = 0; i < 2000; ++i)
        deep += "{\"info\":";
    deep += "{\"plot\":\"Too deep\"}";
    deep += std::string(2000, '}');
    const std::string oversized = "{\"info\":{\"cast\":[\"" + std::string(257, 'x') +
                                  "\",\"Other actor\"],\"plot\":\"Invalid\"}}";
    for (const auto &payload :
         {deep, oversized, std::string(R"({"info":{"cast":["bad\q"],"plot":"Invalid"}})")})
    {
        reply(info + "&vod_id=10", payload);
        model.vod().request_details(0);
        ASSERT_TRUE(settle(model));
        EXPECT_FALSE(model.vod().details(id).complete);
        EXPECT_TRUE(model.vod().details(id).plot.empty());
        EXPECT_TRUE(model.vod().details(id).cast.empty());
    }
    reply(info + "&vod_id=10", R"({"info":{"cast":[{},["ignored"],"Good actor"],"plot":"Good"}})");
    model.vod().request_details(0);
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.vod().details(id).complete);
    EXPECT_EQ(model.vod().details(id).cast, "Good actor");
    model.close();
}

TEST_F(VodTest, AccountInfoComesFromNormalRefreshAndSurvivesOfflineCachedStartup)
{
    const auto expiry = std::time(nullptr) + 3 * 86400;
    response("", std::string(R"({"user_info":{"auth":1,"exp_date":)") + std::to_string(expiry) +
                     R"(,"max_connections":"4"}})");
    response("get_live_categories", R"([])");
    response("get_live_streams", R"([{"stream_id":1,"name":"Live"}])");
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.refresh();
        ASSERT_TRUE(settle(model));
        ASSERT_NE(model.saved_source(3), nullptr);
        EXPECT_EQ(model.saved_source(3)->account.expires_unix, expiry);
        EXPECT_EQ(model.saved_source(3)->account.max_connections, 4);
        std::string auth_url;
        ASSERT_TRUE(iptv::BuildXtreamApiUrl(account, "", &auth_url));
        const auto requests = host::requests();
        EXPECT_EQ(std::count_if(requests.begin(), requests.end(),
                                [&](const auto &request) { return request.url == auth_url; }),
                  1);
        const auto notices = model.take_notices();
        EXPECT_TRUE(std::any_of(notices.begin(), notices.end(), [](const auto &notice)
                                { return notice.title == "Account expires within a week"; }));
        const auto checked = model.saved_source(3)->account.checked_unix;
        host::set_unix_time(static_cast<std::uint64_t>(checked + 60));
        response("", R"({"user_info":{"auth":0}})");
        model.refresh();
        ASSERT_TRUE(settle(model));
        EXPECT_EQ(model.saved_source(3)->account.checked_unix, checked);
        host::set_unix_time(0);
        model.close();
    }
    host::set_network(false, "");
    const auto downloads = host::fetch_count();
    ptv::Model offline(dir);
    ASSERT_TRUE(offline.open());
    EXPECT_FALSE(offline.refreshing());
    EXPECT_EQ(offline.saved_source(3)->account.expires_unix, expiry);
    EXPECT_EQ(offline.saved_source(3)->account.max_connections, 4);
    EXPECT_EQ(host::fetch_count(), downloads);
    const auto notices = offline.take_notices();
    EXPECT_TRUE(std::any_of(notices.begin(), notices.end(), [](const auto &notice)
                            { return notice.title == "Account expires within a week"; }));
    offline.close();
}

TEST_F(VodTest, SourceHttpDefaultsReachAccountCatalogGuideAndVodRequestsAndPlayback)
{
    {
        ptv::Library library;
        ASSERT_TRUE(library.open(dir + "/prosperotv-library.sqlite3"));
        auto source = library.sources()[0];
        source.user_agent = "AccountPlayer/2";
        source.extra_headers = "X-Key: account-secret";
        source.referrer = "https://provider.invalid/watch";
        ASSERT_TRUE(library.save_source(&source));
    }
    response("", R"({"user_info":{"auth":1}})");
    response("get_live_categories", R"([])");
    response("get_live_streams", R"([{"stream_id":1,"name":"Live"}])");
    std::string details_url;
    ASSERT_TRUE(iptv::BuildXtreamApiUrl(account, "get_vod_info", &details_url));
    reply(details_url + "&vod_id=10", R"({"info":{"plot":"Protected details"}})");
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.refresh();
    ASSERT_TRUE(settle(model));
    ASSERT_TRUE(model.play(0));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.extra_headers, "X-Key: account-secret");
    EXPECT_EQ(request.user_agent, "AccountPlayer/2");
    EXPECT_EQ(request.referrer, "https://provider.invalid/watch");
    ASSERT_TRUE(model.set_schedule(model.selected_source_id(), ptv::RefreshSchedule::manual));
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    model.vod().request_details(0);
    ASSERT_TRUE(settle(model));
    EXPECT_EQ(model.vod().details(model.vod().catalog()[0].id).plot, "Protected details");
    ASSERT_TRUE(model.play_vod(0));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.extra_headers, "X-Key: account-secret");
    EXPECT_EQ(request.user_agent, "AccountPlayer/2");
    EXPECT_EQ(request.referrer, "https://provider.invalid/watch");
    model.vod().select(ptv::VodKind::episodes, "20");
    ASSERT_TRUE(settle(model));
    model.close();
    auto requests = host::requests();
    ASSERT_GE(requests.size(), 7u);
    bool guide = false, series = false, details = false;
    for (const auto &fetched : requests)
    {
        EXPECT_EQ(fetched.extra_headers, "X-Key: account-secret");
        EXPECT_EQ(fetched.user_agent, "AccountPlayer/2") << fetched.url;
        EXPECT_EQ(fetched.referrer, "https://provider.invalid/watch") << fetched.url;
        guide = guide || fetched.url.find("xmltv.php") != std::string::npos;
        series = series || fetched.url.find("get_series_info") != std::string::npos;
        details = details || fetched.url.find("get_vod_info") != std::string::npos;
    }
    EXPECT_TRUE(guide);
    EXPECT_TRUE(series);
    EXPECT_TRUE(details);
}

TEST_F(VodTest, HomeUsesFreshEpisodeUrlAndCurrentSourceHeadersAfterColdFetch)
{
    std::string id;
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.view.vod_series_group = "Comedy";
        model.vod().select(ptv::VodKind::episodes, "20");
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.play_vod(2));
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        ASSERT_TRUE(request.urls[0].ends_with("202.mp4"));
        id = request.channel_id;
        model.finish_vod(request, 12000000, 100000000, false);
        model.close();
    }
    ASSERT_TRUE(std::filesystem::remove(dir + "/prosperotv-xtream-catalog.sqlite3.vod-2-" +
                                        std::to_string(iptv::CustomSourceId("20")) + ".sqlite3"));
    std::string series;
    ASSERT_TRUE(iptv::BuildXtreamSeriesUrl(account, "20", &series));
    reply(
        series,
        R"({"episodes":{"2":[{"id":202,"title":"Earlier","episode_num":2,"container_extension":"mkv"}]}})");
    {
        ptv::Library library;
        ASSERT_TRUE(library.open(dir + "/prosperotv-library.sqlite3"));
        const auto sources = library.sources();
        auto source =
            *std::find_if(sources.begin(), sources.end(), [](const auto &s) { return s.id == 3; });
        source.user_agent = "CurrentAgent";
        source.extra_headers = "X-Key: current";
        ASSERT_TRUE(library.save_source(&source));
    }
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(model.play_home(id, true));
    ptv::PlayRequest resumed;
    bool ready = false;
    for (int i = 0; i < 500 && !ready; ++i)
    {
        model.poll();
        ready = model.take_play_request(&resumed);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(ready);
    if (ready)
    {
        EXPECT_TRUE(resumed.urls[0].ends_with("202.mkv"));
        EXPECT_EQ(resumed.user_agent, "CurrentAgent");
        EXPECT_EQ(resumed.extra_headers, "X-Key: current");
        EXPECT_EQ(resumed.start_position_us, 12000000u);
    }
    ASSERT_TRUE(model.hide_category("Comedy", true));
    EXPECT_FALSE(model.play_home(id, true));
    model.close();
}

TEST_F(VodTest, HomeMovieUsesMatchingLoadedUrlWithoutAnotherDownload)
{
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    ASSERT_TRUE(model.play_vod(0));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    const auto id = request.channel_id;
    ASSERT_TRUE(request.urls[0].ends_with("10.mkv"));
    model.finish_vod(request, 12000000, 100000000, false);
    response(
        "get_vod_streams",
        R"([{"stream_id":10,"name":"Alpha movie","container_extension":"mp4","category_id":3}])");
    model.vod().select(ptv::VodKind::movies, {}, true);
    ASSERT_TRUE(settle(model));
    const auto downloads = host::fetch_count();
    ASSERT_TRUE(model.play_home(id, true));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_TRUE(request.urls[0].ends_with("10.mp4"));
    EXPECT_EQ(request.start_position_us, 12000000u);
    EXPECT_EQ(host::fetch_count(), downloads);
    model.close();
}

TEST_F(VodTest, HomeInheritsNewLoadedParentPolicyAndKeepsItOffline)
{
    std::string id;
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        model.view.vod_series_group = "Comedy";
        model.vod().select(ptv::VodKind::episodes, "20");
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.play_vod(2));
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        id = request.channel_id;
        model.finish_vod(request, 12000000, 100000000, false);
        response("get_series_categories", R"([{"category_id":8,"category_name":"Restricted"}])");
        response("get_series",
                 R"([{"series_id":20,"name":"A show","category_id":8,"is_adult":1}])");
        model.vod().select(ptv::VodKind::series);
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.hide_category("Restricted", true));
        EXPECT_FALSE(model.play_home(id, true));
        ASSERT_TRUE(model.hide_category("Restricted", false));
        model.parental_action(ptv::Model::ParentalAction::set_pin);
        model.poll();
        host::set_keyboard_text("123456");
        model.poll();
        host::set_keyboard_text("123456");
        model.poll();
        EXPECT_FALSE(model.play_home(id, true));
        model.parental_action(ptv::Model::ParentalAction::unlock);
        model.poll();
        host::set_keyboard_text("123456");
        model.poll();
        ASSERT_TRUE(model.play_home(id, true));
        ASSERT_TRUE(model.take_play_request(&request));
        model.finish_vod(request, 12000000, 100000000, false);
        model.close();
    }
    host::set_network(false, "");
    ptv::Model offline(dir);
    ASSERT_TRUE(offline.open());
    EXPECT_TRUE(offline.vod().catalog().empty());
    EXPECT_TRUE(offline.home_items()[0].empty());
    EXPECT_FALSE(offline.play_home(id, true));
    offline.close();
}

TEST_F(VodTest, HomeParentPolicyWithLargeLoadedShowCatalog)
{
    constexpr unsigned count = 100000;
    host::set_network_piece(65536);
    std::string shows = "[";
    for (unsigned i = 0; i < count; ++i)
        shows += (i ? "," : "") + std::string(R"({"series_id":)") + std::to_string(i) +
                 R"(,"name":"Show","category_id":4})";
    shows += "]";
    response("get_series", shows);
    {
        ptv::Library library;
        ASSERT_TRUE(library.open(dir + "/prosperotv-library.sqlite3"));
        for (unsigned i = 0; i < 50; ++i)
        {
            iptv::Channel item;
            item.id = "resume-" + std::to_string(i);
            item.name = "Saved episode";
            item.url = "https://provider.invalid/episode.mp4";
            item.source_id = iptv::XtreamSourceId(account);
            item.media_kind = iptv::MediaKind::episode;
            ASSERT_TRUE(
                library.remember_vod(3, item.source_id, item, std::to_string(count - 1 - i), 1));
            ASSERT_TRUE(library.save_playback_progress(item.id, 10000000, 100000000));
        }
    }
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::series);
    for (unsigned i = 0; i < 5000 && (model.vod().requested() || model.vod().busy()); ++i)
    {
        model.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(model.vod().catalog().size(), count);
    EXPECT_EQ(model.vod().find_series("99999", iptv::XtreamSourceId(account) + 1),
              iptv::Catalog::npos);
    EXPECT_EQ(model.vod().find_series("missing", iptv::XtreamSourceId(account)),
              iptv::Catalog::npos);
    std::vector<long long> elapsed;
    for (unsigned trial = 0; trial < 5; ++trial)
    {
        const auto began = std::chrono::steady_clock::now();
        const auto home = model.home_items();
        elapsed.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - began)
                              .count());
        EXPECT_EQ(home[0].size(), 50u);
    }
    std::sort(elapsed.begin(), elapsed.end());
    std::printf("Home 100000 shows / 50 resumes median=%lld us min=%lld us max=%lld us\n",
                elapsed[2], elapsed.front(), elapsed.back());
    EXPECT_TRUE(model.hide_category("Comedy", true));
    EXPECT_TRUE(model.home_items()[0].empty());
    model.close();
}

TEST(VodLibrary, PlaylistParentLookupUsesExactGroupingKeyAndSource)
{
    auto catalog = iptv::ParseExtendedM3u(
        "#EXTM3U\n#EXTINF:-1 group-title=\"Kids\",Show S01E01\nhttps://host/series/1.mp4\n", 123);
    ASSERT_EQ(catalog.size(), 1u);
    ptv::VodLibrary vod;
    vod.configure_playlist(&catalog);
    vod.select(ptv::VodKind::series);
    ASSERT_EQ(vod.catalog().size(), 1u);
    EXPECT_EQ(vod.find_series("Kids\nShow", 123), 0u);
    EXPECT_EQ(vod.find_series("Kids\nShow", 124), iptv::Catalog::npos);
    EXPECT_EQ(vod.find_series("Show", 123), iptv::Catalog::npos);
}

TEST_F(VodTest, ViewingLimitStopsFontlessControlsQualityReopenHomeAndNextEpisode)
{
    const auto wall = ptv::platform::unix_time();
    host::set_unix_time(wall);
    host::set_monotonic_us(1000000);
    ptv::Parental pin;
    ASSERT_TRUE(pin.load(dir + "/prosperotv-parental-v1.txt"));
    ASSERT_TRUE(pin.set_pin("1234"));
    ASSERT_TRUE(pin.unlock("1234", wall));
    ASSERT_TRUE(pin.set_limits(1, -1, 0));
    ASSERT_TRUE(pin.set_kids(true));
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    model.vod().select(ptv::VodKind::movies);
    ASSERT_TRUE(settle(model));
    ASSERT_TRUE(model.play_vod(0));
    ptv::PlayRequest movie;
    ASSERT_TRUE(model.take_play_request(&movie));
    EXPECT_FALSE(model.finish_vod(movie, 12000000, 100000000, false));
    model.view.vod_series_group = "Kids";
    model.vod().select(ptv::VodKind::episodes, "20");
    ASSERT_TRUE(settle(model));
    ASSERT_TRUE(model.play_vod(2));
    ptv::PlayRequest episode;
    ASSERT_TRUE(model.take_play_request(&episode));
    // The native callback invokes this before touching fonts or quality controls.
    EXPECT_EQ(model.viewing_control(), 0);
    host::set_unix_time(wall + 60);
    host::set_monotonic_us(61000000);
    EXPECT_EQ(model.viewing_control(), 2);
    EXPECT_FALSE(model.finish_vod(episode, 100000000, 100000000, true));
    EXPECT_FALSE(model.take_play_request(&episode));
    EXPECT_FALSE(model.play_home(movie.channel_id, true));
    // A quality reopen reuses the current request but must acquire the same meter again.
    EXPECT_FALSE(model.viewing_tick(true));
    EXPECT_EQ(model.viewing_control(), 2);
    model.close();
}
