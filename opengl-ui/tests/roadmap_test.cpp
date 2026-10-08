// ProsperoTV - Persistent source and browsing regressions.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_platform.hpp"
#include "tv/library.hpp"
#include "tv/model.hpp"
#include "tv/settings.hpp"
#include "iptv_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
class RoadmapTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        host::reset();
        char pattern[] = "/tmp/prosperotv-roadmap-XXXXXX";
        dir = mkdtemp(pattern);
        playlist = dir + "/input.m3u";
        std::ofstream file(playlist);
        file << "#EXTM3U\n";
        for (int i = 0; i < 320; ++i)
            file << "#EXTINF:-1 tvg-id=\"c" << i << "\" group-title=\"US / Category " << i % 40
                 << " with a name longer than forty-seven characters, intact\",Channel " << i
                 << "\nhttps://example.invalid/" << i << ".m3u8\n";
        file.close();
        host::set_network(true, playlist);
    }
    void TearDown() override
    {
        std::filesystem::remove_all(dir);
        host::reset();
    }
    static bool settle(ptv::Model &model)
    {
        for (int i = 0; i < 500; ++i)
        {
            model.poll();
            if (!model.refreshing())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void load(ptv::Model &model)
    {
        ASSERT_TRUE(model.open());
        ASSERT_TRUE(settle(model));
        ASSERT_EQ(model.channel_count(), 320u);
    }
    std::string dir, playlist;
};

TEST(RefreshSchedule, BoundariesManualAndClockChanges)
{
    using ptv::RefreshSchedule;
    EXPECT_TRUE(ptv::refresh_due(RefreshSchedule::manual, 0, 500));
    EXPECT_FALSE(ptv::refresh_due(RefreshSchedule::manual, 100, UINT64_MAX));
    EXPECT_FALSE(ptv::refresh_due(RefreshSchedule::daily, 100, 100 + 86400 - 1));
    EXPECT_TRUE(ptv::refresh_due(RefreshSchedule::daily, 100, 100 + 86400));
    EXPECT_FALSE(ptv::refresh_due(RefreshSchedule::weekly, 100, 100 + 6 * 86400));
    EXPECT_TRUE(ptv::refresh_due(RefreshSchedule::weekly, 100, 100 + 7 * 86400));
    EXPECT_FALSE(ptv::refresh_due(RefreshSchedule::daily, 100, 0));
    EXPECT_FALSE(ptv::refresh_due(RefreshSchedule::daily, 100, 99));
}

TEST_F(RoadmapTest, AllProviderNamesRemainExactAndHiddenCategoriesSurviveRestart)
{
    std::string category;
    {
        ptv::Model model(dir);
        load(model);
        ASSERT_EQ(model.provider_categories().size(), 40u);
        category = model.provider_categories().front().value;
        EXPECT_GT(category.size(), 47u);
        model.set_provider_category(category);
        EXPECT_EQ(model.visible_count(), 8u);
        model.clear_filters();
        ASSERT_TRUE(model.hide_category(category, true));
        EXPECT_EQ(model.visible_count(), 312u);
        EXPECT_EQ(model.group_size(ptv::Group::all), 312u);
        model.close();
    }
    ptv::Model model(dir);
    load(model);
    EXPECT_TRUE(model.category_hidden(category));
    EXPECT_EQ(model.visible_count(), 312u);
    ASSERT_TRUE(model.hide_category(category, false));
    EXPECT_EQ(model.visible_count(), 320u);
    model.close();
}

TEST_F(RoadmapTest, ParentCategoriesFilterAndHideEveryDescendant)
{
    ptv::Model model(dir);
    load(model);
    model.set_provider_category("US");
    EXPECT_EQ(model.visible_count(), 320u);
    model.set_provider_category("U");
    EXPECT_EQ(model.visible_count(), 0u);
    model.clear_filters();
    const auto child = model.provider_categories()[0].value;
    ASSERT_TRUE(model.hide_category(child, true));
    ASSERT_TRUE(model.hide_category("US", true));
    EXPECT_EQ(model.visible_count(), 0u);
    EXPECT_TRUE(model.category_hidden("US / A newly added group"));
    EXPECT_FALSE(model.hide_category(child, false));
    ASSERT_TRUE(model.hide_category("US", false));
    EXPECT_EQ(model.visible_count(), 312u); // Individually hidden child stays hidden.
    ASSERT_TRUE(model.hide_category(child, false));
    EXPECT_EQ(model.visible_count(), 320u);
    model.close();
}

TEST_F(RoadmapTest, SeveralPlaylistsHaveIndependentCachesSelectionsAndSchedules)
{
    std::int64_t first = 0, second = 0;
    std::string channel;
    {
        ptv::Model model(dir);
        load(model);
        model.add_source(iptv::SourceKind::Custom);
        host::set_keyboard_text("https://one.example.invalid/list.m3u");
        ASSERT_TRUE(settle(model));
        first = model.selected_source_id();
        EXPECT_GT(first, 4);
        channel = model.channel(0).id;
        ASSERT_TRUE(model.set_schedule(first, ptv::RefreshSchedule::manual));
        model.add_source(iptv::SourceKind::Custom);
        host::set_keyboard_text("https://two.example.invalid/list.m3u");
        ASSERT_TRUE(settle(model));
        second = model.selected_source_id();
        EXPECT_GT(second, first);
        EXPECT_NE(model.channel(0).id, channel);
        const auto fetches = host::fetch_count();
        model.use_saved_source(first);
        EXPECT_FALSE(model.refreshing());
        EXPECT_EQ(host::fetch_count(), fetches);
        EXPECT_EQ(model.channel(0).id, channel);
        model.close();
    }
    host::set_network(false, "");
    ptv::Model model(dir);
    load(model);
    EXPECT_EQ(model.selected_source_id(), first);
    EXPECT_EQ(model.saved_source(first)->schedule, ptv::RefreshSchedule::manual);
    EXPECT_EQ(model.saved_source(second)->url, "https://two.example.invalid/list.m3u");
    model.use_saved_source(second);
    EXPECT_FALSE(model.refreshing());
    EXPECT_NE(model.channel(0).id, channel);
    model.close();
}

TEST_F(RoadmapTest, FavoritesBeyond256AndFolderMembershipPersist)
{
    {
        ptv::Model model(dir);
        load(model);
        ASSERT_TRUE(model.create_folder("Football"));
        ASSERT_TRUE(model.create_folder("Kids"));
        for (unsigned i = 0; i < 300; ++i)
            ASSERT_EQ(model.toggle_favorite(i), ptv::Model::Starred::added);
        ASSERT_TRUE(model.put_in_folder("Football", 2, true));
        ASSERT_TRUE(model.put_in_folder("Football", 7, true));
        model.set_group(ptv::Group::favorites);
        EXPECT_EQ(model.visible_count(), 300u);
        model.set_folder("Football");
        EXPECT_EQ(model.visible_count(), 2u);
        ASSERT_TRUE(model.rename_folder("Football", "Sports"));
        EXPECT_EQ(model.folder(), "Sports");
        EXPECT_EQ(model.visible_count(), 2u);
        model.close();
    }
    ptv::Model model(dir);
    load(model);
    model.set_group(ptv::Group::favorites);
    EXPECT_EQ(model.visible_count(), 300u);
    model.set_folder("Sports");
    EXPECT_EQ(model.visible_count(), 2u);
    ASSERT_TRUE(model.remove_folder("Sports"));
    EXPECT_EQ(model.visible_count(), 300u);
    model.close();
}

TEST_F(RoadmapTest, RefusedLibraryChangesKeepSourcesAndFavorites)
{
    ptv::Model model(dir);
    load(model);
    EXPECT_FALSE(model.put_in_folder("A folder that does not exist", 0, true));
    EXPECT_FALSE(model.is_favorite(model.channel(0)));

    model.edit_saved_source(2);
    host::set_keyboard_text("https://before.invalid/list.m3u");
    ASSERT_TRUE(settle(model));
    ASSERT_EQ(model.saved_source(2)->url, "https://before.invalid/list.m3u");
    sqlite3 *blocker = nullptr;
    ASSERT_EQ(sqlite3_open((dir + "/prosperotv-library.sqlite3").c_str(), &blocker), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    model.edit_saved_source(2);
    host::set_keyboard_text("https://after.invalid/list.m3u");
    model.poll();
    EXPECT_EQ(model.saved_source(2)->url, "https://before.invalid/list.m3u");
    std::string legacy;
    EXPECT_EQ(iptv::LoadCustomSourceUrl(dir + "/iptv-custom-source-v1.txt", &legacy),
              iptv::SourceStateStatus::ok);
    EXPECT_EQ(legacy, "https://before.invalid/list.m3u");
    sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(blocker);
    model.close();
}

TEST_F(RoadmapTest, LargeFavoriteFilesRoundTripAndRejectDuplicates)
{
    std::vector<std::string> favorites;
    for (unsigned i = 0; i < iptv::kDefaultMaxFavoriteIds; ++i)
        favorites.push_back("channel-" + std::to_string(i));
    const auto path = dir + "/favorites.bin";
    ASSERT_EQ(iptv::SaveFavorites(path, favorites), iptv::UserStateStatus::ok);
    std::vector<std::string> loaded;
    ASSERT_EQ(iptv::LoadFavorites(path, &loaded), iptv::UserStateStatus::ok);
    EXPECT_EQ(loaded, favorites);
    favorites.back() = favorites.front();
    EXPECT_NE(iptv::SaveFavorites(path, favorites), iptv::UserStateStatus::ok);
    ASSERT_EQ(iptv::LoadFavorites(path, &loaded), iptv::UserStateStatus::ok);
    EXPECT_EQ(loaded.back(), "channel-65535");
}

TEST_F(RoadmapTest, HideFailedAndResumeOnlyOnceAfterReopeningTheMenu)
{
    ptv::Model model(dir);
    load(model);
    ASSERT_TRUE(model.play(2));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    model.close();
    ASSERT_TRUE(model.open());
    model.resume_last(true);
    ASSERT_TRUE(model.take_play_request(&request));
    model.close();
    ASSERT_EQ(iptv::RecordPlaybackResult(dir + "/prosperotv-playback-history.sqlite3",
                                         request.source_id, request.channel_id, false, -1),
              iptv::StoreStatus::ok);
    ASSERT_TRUE(model.open());
    model.resume_last(true);
    EXPECT_FALSE(model.take_play_request(&request));
    model.set_hide_failed(true);
    EXPECT_EQ(model.visible_count(), 319u);
    model.set_hide_failed(false);
    EXPECT_EQ(model.visible_count(), 320u);
    model.close();
    ptv::Settings settings;
    settings.hide_failed = settings.resume_last = true;
    ASSERT_TRUE(ptv::save_settings(dir, settings));
    EXPECT_EQ(ptv::load_settings(dir), settings);
    ptv::Model restarted(dir);
    load(restarted);
    restarted.resume_last(true);
    EXPECT_FALSE(restarted.take_play_request(&request));
    EXPECT_EQ(restarted.visible_count(), 319u);
    restarted.close();
}

TEST_F(RoadmapTest, DatabaseConstraintsAndWriteFailurePreserveState)
{
    ptv::Library library;
    ASSERT_TRUE(library.open(dir + "/library.sqlite3"));
    ASSERT_TRUE(library.add_folder("Kids"));
    EXPECT_FALSE(library.add_folder("Kids"));
    EXPECT_FALSE(library.add_folder("bad\nname"));
    ASSERT_TRUE(library.put_in_folder("Kids", "channel", true));
    EXPECT_FALSE(library.put_in_folder("Missing", "channel", true));
    sqlite3 *blocker = nullptr;
    ASSERT_EQ(sqlite3_open((dir + "/library.sqlite3").c_str(), &blocker), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_FALSE(library.rename_folder("Kids", "Family"));
    sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(blocker);
    EXPECT_EQ(library.folders(), std::vector<std::string>{"Kids"});
    EXPECT_TRUE(library.folder_channels("Kids").contains("channel"));
    EXPECT_FALSE(library.select_source(999));
}

TEST_F(RoadmapTest, StartupReturnsToTheSourceOfTheLastWatchedChannel)
{
    std::string channel;
    std::int64_t source = 0;
    {
        ptv::Model model(dir);
        load(model);
        model.add_source(iptv::SourceKind::Custom);
        host::set_keyboard_text("https://one.example.invalid/list.m3u");
        ASSERT_TRUE(settle(model));
        source = model.selected_source_id();
        channel = model.channel(3).id;
        ASSERT_TRUE(model.play(3));
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        model.use_saved_source(1);
        ASSERT_EQ(model.selected_source_id(), 1);
        model.close();
    }
    ptv::Settings settings;
    settings.resume_last = true;
    ASSERT_TRUE(ptv::save_settings(dir, settings));
    ptv::Model model(dir);
    load(model);
    EXPECT_EQ(model.selected_source_id(), source);
    model.resume_last(true);
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.channel_id, channel);
    model.close();
}
} // namespace
