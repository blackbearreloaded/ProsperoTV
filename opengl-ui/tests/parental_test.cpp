// ProsperoTV - Parental policy, persistent retry delay and playback entry points.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/model.hpp"
#include "tv/parental.hpp"
#include "tv/backup.hpp"
#include "host_platform.hpp"
#include "iptv_store.h"
#include "iptv_ime.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
class ParentalTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        host::reset();
        char pattern[] = "/tmp/prosperotv-parental-XXXXXX";
        dir = mkdtemp(pattern);
        file = dir + "/prosperotv-parental-v1.txt";
    }
    void TearDown() override
    {
        host::reset();
        std::filesystem::remove_all(dir);
    }
    static void answer(ptv::Model &model, const char *pin)
    {
        model.poll();
        host::set_keyboard_text(pin);
        model.poll();
    }
    static void unlock(ptv::Model &model)
    {
        model.parental_action(ptv::Model::ParentalAction::unlock);
        answer(model, "836295");
        ASSERT_TRUE(model.parental().unlocked());
    }
    static void settle(ptv::Model &model)
    {
        for (unsigned i = 0; i < 400 && model.refreshing(); ++i)
        {
            model.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_FALSE(model.refreshing());
    }
    std::string dir, file;
};

TEST_F(ParentalTest, PinHashRetryDelayAndKidsModeSurviveRestartAndBackup)
{
    ptv::Parental pin;
    ASSERT_TRUE(pin.load(file));
    EXPECT_TRUE(pin.unlocked());
    EXPECT_FALSE(pin.set_pin("abc"));
    ASSERT_TRUE(pin.set_pin("836295"));
    EXPECT_FALSE(pin.unlocked());
    {
        std::ifstream in(file);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        EXPECT_EQ(text.find("836295"), std::string::npos);
    }
    for (int i = 0; i < 5; ++i)
        EXPECT_FALSE(pin.unlock("0000", 1000));
    EXPECT_EQ(pin.wait_until(), 1030u);
    ASSERT_TRUE(pin.load(file));
    EXPECT_FALSE(pin.unlock("836295", 1029));
    ASSERT_TRUE(pin.unlock("836295", 1030));
    ASSERT_TRUE(pin.set_kids(true));
    EXPECT_FALSE(pin.unlocked());
    ASSERT_TRUE(pin.load(file));
    EXPECT_TRUE(pin.kids_only());
    EXPECT_FALSE(pin.set_kids(false));
    EXPECT_FALSE(pin.remove());
    EXPECT_FALSE(pin.set_pin("1234"));
    std::string error;
    ASSERT_TRUE(ptv::backup_settings(dir, dir + "/backup.sqlite3", error)) << error;
    ASSERT_TRUE(pin.unlock("836295", 1100));
    ASSERT_TRUE(pin.remove());
    EXPECT_FALSE(pin.enabled());
    ASSERT_TRUE(ptv::restore_settings(dir, dir + "/backup.sqlite3", error)) << error;
    ASSERT_TRUE(pin.load(file));
    EXPECT_TRUE(pin.kids_only());
    EXPECT_FALSE(pin.unlocked());
    ASSERT_TRUE(pin.unlock("836295", 1200));
    ASSERT_TRUE(pin.set_kids(false));
}

TEST_F(ParentalTest, CorruptLinkedAndUnwritableSettingsFailClosed)
{
    std::ofstream(file) << "PTV_PARENTAL_1\ncorrupt";
    ptv::Parental pin;
    EXPECT_FALSE(pin.load(file));
    EXPECT_FALSE(pin.unlocked());
    EXPECT_FALSE(pin.set_pin("1234"));
    std::filesystem::remove(file);
    std::filesystem::create_symlink(dir + "/absent", file);
    EXPECT_FALSE(pin.load(file));
    EXPECT_FALSE(pin.unlocked());
    std::filesystem::remove(file);
    ASSERT_TRUE(pin.load(file));
    ASSERT_TRUE(pin.set_pin("836295"));
    std::filesystem::create_directory(file + ".tmp");
    EXPECT_FALSE(pin.unlock("836295", 1000));
    EXPECT_FALSE(pin.valid());
    EXPECT_FALSE(pin.unlocked());
}

TEST_F(ParentalTest, PinAndKidsPolicyCoverListsFavoritesDirectPlayPreviewGuideResumeAndPhone)
{
    const auto playlist = dir + "/list.m3u";
    std::ofstream(playlist)
        << "#EXTM3U\n"
           "#EXTINF:-1 group-title=\"US / Adult\",Restricted\nhttps://test.invalid/a\n"
           "#EXTINF:-1 group-title=\"US / Kids / Cartoons\",Cartoon\nhttps://test.invalid/b\n"
           "#EXTINF:-1 group-title=\"Movies\" is-adult=\"1\",Flagged\nhttps://test.invalid/c\n"
           "#EXTINF:-1 group-title=\"General\",General\nhttps://test.invalid/d\n";
    host::set_network(true, playlist);
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    settle(model);
    ASSERT_EQ(model.channel_count(), 4u);
    EXPECT_EQ(model.toggle_favorite(0), ptv::Model::Starred::added);
    ASSERT_TRUE(model.play(0));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    model.parental_action(ptv::Model::ParentalAction::set_pin);
    answer(model, "836295");
    EXPECT_EQ(host::keyboard_title(), "Confirm parent PIN");
    answer(model, "836295");
    ASSERT_TRUE(model.parental().enabled());
    EXPECT_FALSE(model.parental().unlocked());
    EXPECT_EQ(model.visible_count(), 2u);
    EXPECT_FALSE(model.play(0));
    EXPECT_FALSE(model.play(2));
    EXPECT_FALSE(model.preview_request(model.channel(0).id));
    ptv::Programme programme;
    programme.start = 1;
    programme.end = INT64_MAX;
    EXPECT_FALSE(model.play_programme(0, programme));
    model.resume_last(true);
    EXPECT_FALSE(model.take_play_request(&request));
    model.set_group(ptv::Group::favorites);
    EXPECT_EQ(model.visible_count(), 0u);
    model.set_query("Restricted");
    EXPECT_EQ(model.visible_count(), 0u);
    std::string output;
    EXPECT_EQ(model.remote_sources({}, output), 403);
    EXPECT_EQ(model.remote_sources(
                  R"({"op":"save","kind":1,"name":"X","url":"https://other.invalid"})", output),
              403);
    model.add_source(iptv::SourceKind::Custom);
    EXPECT_FALSE(iptv_ime_busy());
    unlock(model);
    ASSERT_TRUE(model.play(0));
    EXPECT_TRUE(model.take_play_request(&request));
    ASSERT_TRUE(model.set_category_rule("General", 2));
    model.parental_action(ptv::Model::ParentalAction::kids);
    EXPECT_TRUE(model.parental().kids_only());
    EXPECT_FALSE(model.parental().unlocked());
    EXPECT_FALSE(model.play(0));
    EXPECT_FALSE(model.play(2));
    EXPECT_TRUE(model.play(1));
    EXPECT_TRUE(model.play(3));
    EXPECT_FALSE(model.set_category_rule("US / Adult", 2));
    unlock(model);
    ASSERT_TRUE(model.set_category_rule("US / Adult", 2));
    EXPECT_FALSE(model.play(0)); // Adult names/flags take precedence over kids approval.
    model.parental_action(ptv::Model::ParentalAction::kids);
    EXPECT_FALSE(model.parental().kids_only());
    EXPECT_TRUE(model.play(0));
    model.close();
    ptv::Model reopened(dir);
    ASSERT_TRUE(reopened.open());
    settle(reopened);
    EXPECT_FALSE(reopened.parental().unlocked());
    EXPECT_FALSE(reopened.play(0));
    EXPECT_EQ(reopened.category_rule("General"), 2);
    reopened.close();
}

TEST_F(ParentalTest, AdultMetadataMergesAndSurvivesCatalogCopiesAndCache)
{
    const auto catalog = iptv::ParseExtendedM3u(
        "#EXTM3U\n#EXTINF:-1 tvg-id=\"one\",One\nhttps://test.invalid/1\n"
        "#EXTINF:-1 tvg-id=\"one\" is-adult=\"1\",One\nhttps://test.invalid/2\n",
        123);
    ASSERT_EQ(catalog.size(), 1u);
    EXPECT_TRUE(catalog[0].adult);
    EXPECT_TRUE(catalog[0].Copy().adult);
    iptv::Catalog copy(catalog);
    EXPECT_TRUE(copy[0].adult);
    ASSERT_EQ(iptv::SaveCatalog(dir + "/cache.sqlite3", copy), iptv::StoreStatus::ok);
    iptv::Catalog loaded;
    iptv::StoreReport report;
    ASSERT_EQ(iptv::LoadCatalog(dir + "/cache.sqlite3", &loaded, {}, &report),
              iptv::StoreStatus::ok);
    EXPECT_TRUE(loaded[0].adult);
    EXPECT_EQ(report.catalog_version, 5u);
    std::vector<iptv::XtreamCategory> categories;
    ASSERT_EQ(
        iptv::ParseXtreamCategories(
            R"([{"category_id":1,"category_name":"Cinema","is_adult":1},{"category_id":2,"parent_id":1,"category_name":"HD"}])",
            &categories),
        iptv::XtreamStatus::ok);
    EXPECT_TRUE(categories[1].adult);
    ASSERT_EQ(
        iptv::ParseXtreamLiveStreams(
            R"([{"stream_id":1,"name":"One","category_id":2},{"stream_id":2,"name":"Two","is_adult":"1"}])",
            {"https://test.invalid", "u", "p"}, categories, 123, &loaded),
        iptv::XtreamStatus::ok);
    EXPECT_TRUE(loaded[0].adult);
    EXPECT_TRUE(loaded[1].adult);
    EXPECT_TRUE(ptv::adult_category("US / 18+ / Movies"));
    EXPECT_FALSE(ptv::adult_category("Adult Swim"));
    EXPECT_FALSE(ptv::kids_category("Kidnap movies"));
}
} // namespace
