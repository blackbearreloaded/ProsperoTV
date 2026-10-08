// ProsperoTV - Tests of the app's logic against the PC's stand-in console.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_platform.hpp"
#include "large_list.hpp"
#include "tv/model.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{

namespace fs = std::filesystem;

// Twelve channels, written the way public playlists write them.
constexpr const char *kPlaylist =
    "#EXTM3U\n"
    "#EXTINF:-1 tvg-id=\"a.us\" tvg-country=\"US\" tvg-language=\"English\" "
    "group-title=\"News\",Alder News (1080p)\n"
    "https://streams.example.invalid/alder/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"b.us\" tvg-country=\"US\" tvg-language=\"English\" "
    "group-title=\"Sports\",Bramble Sport (720p) [Not 24/7]\n"
    "https://streams.example.invalid/bramble/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"c.de\" tvg-country=\"DE\" tvg-language=\"German\" "
    "group-title=\"Kids\",Clover Kids (576p)\n"
    "https://streams.example.invalid/clover/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"d.de\" tvg-country=\"DE\" tvg-language=\"German\" "
    "group-title=\"General\",Dune One\n"
    "https://streams.example.invalid/dune/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"e.fr\" tvg-country=\"FR\" tvg-language=\"French\" "
    "group-title=\"Movies\",Ember Max (2160p)\n"
    "https://streams.example.invalid/ember/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"f.fr\" tvg-country=\"FR\" tvg-language=\"French\" "
    "group-title=\"News\",Fern 24 (1080p) [Geo-blocked]\n"
    "https://streams.example.invalid/fern/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"g.us\" tvg-country=\"US\" tvg-language=\"English\" "
    "group-title=\"Music\",Granite Live (720p)\n"
    "https://streams.example.invalid/granite/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"h.us\" tvg-country=\"US\" tvg-language=\"Spanish\" "
    "group-title=\"General\",Harbor Two (480p)\n"
    "https://streams.example.invalid/harbor/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"i.es\" tvg-country=\"ES\" tvg-language=\"Spanish\" "
    "group-title=\"Sports\",Iris Sport (1080p)\n"
    "https://streams.example.invalid/iris/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"j.es\" tvg-country=\"ES\" tvg-language=\"Spanish\" "
    "group-title=\"Kids\",Juniper Kids\n"
    "https://streams.example.invalid/juniper/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"k.gb\" tvg-country=\"GB\" tvg-language=\"English\" "
    "group-title=\"Documentary\",Kestrel World (1080p)\n"
    "https://streams.example.invalid/kestrel/index.m3u8\n"
    "#EXTINF:-1 tvg-id=\"l.gb\" tvg-country=\"GB\" tvg-language=\"English\" "
    "group-title=\"General\",Linden Plus (720p)\n"
    "https://streams.example.invalid/linden/index.m3u8\n";

class ModelTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        host::reset();
        now_ = static_cast<std::uint64_t>(std::time(nullptr));
        char pattern[] = "/tmp/prosperotv-model-XXXXXX";
        dir_ = mkdtemp(pattern);
        playlist_ = dir_ + "/playlist.m3u";
        std::ofstream(playlist_) << kPlaylist;
        host::set_network(true, playlist_);
    }

    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(dir_, error);
    }

    // Polls until the download is over (two seconds at most).
    static bool settle(ptv::Model &model)
    {
        for (int i = 0; i < 400; ++i)
        {
            model.poll();
            if (!model.refreshing())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // The same for a list that takes its time: three minutes at most, and
    // the most channels the download said it had read on the way.
    static bool settle_large(ptv::Model &model, unsigned *most = nullptr)
    {
        for (int i = 0; i < 36000; ++i)
        {
            model.poll();
            if (!model.refreshing())
                return true;
            if (most != nullptr && model.refresh_progress() > *most)
                *most = model.refresh_progress();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // A model with the twelve channels loaded and saved.
    void load(ptv::Model &model)
    {
        ASSERT_TRUE(model.open());
        ASSERT_TRUE(settle(model));
        ASSERT_TRUE(model.has_catalog());
        model.take_notices();
    }

    static unsigned index_of(const ptv::Model &model, const char *id)
    {
        for (unsigned index = 0; index < model.channel_count(); ++index)
            if (model.channel(index).tvg_id == id)
                return index;
        ADD_FAILURE() << "no channel " << id;
        return 0;
    }

    std::string dir_;
    std::string playlist_;
    std::uint64_t now_ = 0;
};

TEST_F(ModelTest, SaysWhichScriptsAListIsWrittenIn)
{
    {
        ptv::Model model(dir_);
        load(model);
        EXPECT_FALSE(model.uses_east_asian());
        EXPECT_FALSE(model.uses_korean());
    }
    // The same list with one Chinese name and one Korean group.
    std::ofstream(playlist_, std::ios::app)
        << "#EXTINF:-1 tvg-id=\"z.cn\" group-title=\"News\",CCTV-5 \xE9\xAB\x98\xE6\xB8\x85\n"
        << "https://streams.example.invalid/cctv5/index.m3u8\n";
    {
        fs::remove_all(dir_ + "/cache");
        ptv::Model model(dir_ + "/second");
        fs::create_directories(dir_ + "/second");
        load(model);
        EXPECT_TRUE(model.uses_east_asian());
        EXPECT_FALSE(model.uses_korean());
    }
    std::ofstream(playlist_, std::ios::app)
        << "#EXTINF:-1 tvg-id=\"y.kr\" group-title=\"\xEB\x89\xB4\xEC\x8A\xA4\",YTN\n"
        << "https://streams.example.invalid/ytn/index.m3u8\n";
    {
        ptv::Model model(dir_ + "/third");
        fs::create_directories(dir_ + "/third");
        load(model);
        EXPECT_TRUE(model.uses_east_asian());
        EXPECT_TRUE(model.uses_korean());
    }
}

TEST_F(ModelTest, FirstOpenDownloadsSavesAndSaysSo)
{
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    EXPECT_FALSE(model.has_catalog());
    EXPECT_TRUE(model.refreshing());
    EXPECT_EQ(model.level(), ptv::Level::busy);
    EXPECT_EQ(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::refreshing);

    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.has_catalog());
    EXPECT_EQ(model.channel_count(), 12u);
    EXPECT_EQ(model.visible_count(), 12u);
    EXPECT_EQ(model.level(), ptv::Level::ready);
    EXPECT_EQ(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::ready);
    EXPECT_FALSE(model.catalog_failed());
    EXPECT_TRUE(fs::exists(dir_ + "/prosperotv-catalog.sqlite3"));
    const std::vector<ptv::Notice> notices = model.take_notices();
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].title, "Channel list updated");
    EXPECT_EQ(notices[0].body, "12 channels");
    EXPECT_TRUE(model.take_notices().empty());
    model.close();
}

TEST_F(ModelTest, AFreshSavedCopyIsNotDownloadedAgain)
{
    {
        ptv::Model first(dir_);
        load(first);
        first.close();
    }
    const int before = host::fetch_count();
    host::set_unix_time(now_ + 3600);
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    EXPECT_TRUE(model.has_catalog());
    EXPECT_EQ(model.channel_count(), 12u);
    EXPECT_FALSE(model.refreshing());
    EXPECT_EQ(host::fetch_count(), before);
    EXPECT_EQ(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::cached);
    model.close();
}

TEST_F(ModelTest, AnOldSavedCopyShowsAtOnceAndIsDownloadedAgain)
{
    {
        ptv::Model first(dir_);
        load(first);
        first.close();
    }
    host::set_unix_time(now_ + 25 * 3600);
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    EXPECT_TRUE(model.has_catalog());
    EXPECT_TRUE(model.refreshing());
    ASSERT_TRUE(settle(model));
    EXPECT_EQ(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::ready);
    model.close();
}

TEST_F(ModelTest, OfflineWithASavedCopyKeepsBrowsing)
{
    {
        ptv::Model first(dir_);
        load(first);
        first.close();
    }
    host::set_unix_time(now_ + 25 * 3600);
    host::set_network(false, "");
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.has_catalog());
    EXPECT_FALSE(model.catalog_failed());
    EXPECT_EQ(model.level(), ptv::Level::warning);
    EXPECT_EQ(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::stale);
    const std::vector<ptv::Notice> notices = model.take_notices();
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].level, ptv::Level::warning);
    model.close();
}

TEST_F(ModelTest, OfflineWithNothingSavedSaysWhyAndCanRetry)
{
    host::set_network(false, "");
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(settle(model));
    EXPECT_FALSE(model.has_catalog());
    EXPECT_TRUE(model.catalog_failed());
    EXPECT_FALSE(model.catalog_error().empty());
    EXPECT_EQ(model.level(), ptv::Level::error);
    EXPECT_EQ(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::error);

    host::set_network(true, playlist_);
    model.refresh();
    EXPECT_FALSE(model.catalog_failed());
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.has_catalog());
    model.close();
}

TEST_F(ModelTest, GroupsCountAndNarrowTheList)
{
    ptv::Model model(dir_);
    load(model);
    EXPECT_EQ(model.group_size(ptv::Group::all), 12u);
    EXPECT_EQ(model.group_size(ptv::Group::news), 2u);
    EXPECT_EQ(model.group_size(ptv::Group::sports), 2u);
    EXPECT_EQ(model.group_size(ptv::Group::kids), 2u);
    EXPECT_EQ(model.group_size(ptv::Group::favorites), 0u);
    EXPECT_EQ(model.group_size(ptv::Group::recent), 0u);

    const unsigned before = model.revision();
    model.set_group(ptv::Group::sports);
    EXPECT_NE(model.revision(), before);
    ASSERT_EQ(model.visible_count(), 2u);
    EXPECT_EQ(model.channel(model.visible(0)).tvg_id, "b.us");
    EXPECT_EQ(model.channel(model.visible(1)).tvg_id, "i.es");
    EXPECT_EQ(model.position_of(model.channel(model.visible(1)).id), 1);
    EXPECT_EQ(model.position_of("nothing"), -1);
    model.close();
}

TEST_F(ModelTest, FavoritesAreSavedAndShapeTheirOwnList)
{
    ptv::Model model(dir_);
    load(model);
    const unsigned dune = index_of(model, "d.de");
    EXPECT_EQ(model.toggle_favorite(dune), ptv::Model::Starred::added);
    EXPECT_TRUE(model.is_favorite(model.channel(dune)));
    EXPECT_EQ(model.group_size(ptv::Group::favorites), 1u);
    EXPECT_EQ(model.visible_count(), 12u);

    model.set_group(ptv::Group::favorites);
    ASSERT_EQ(model.visible_count(), 1u);
    EXPECT_EQ(model.visible(0), dune);
    EXPECT_EQ(model.toggle_favorite(dune), ptv::Model::Starred::removed);
    EXPECT_EQ(model.visible_count(), 0u);
    EXPECT_EQ(model.toggle_favorite(dune), ptv::Model::Starred::added);
    model.close();

    // Another launch reads the list back.
    ptv::Model again(dir_);
    ASSERT_TRUE(again.open());
    EXPECT_EQ(again.group_size(ptv::Group::favorites), 1u);
    again.close();
}

TEST_F(ModelTest, SearchAndFiltersCombine)
{
    ptv::Model model(dir_);
    load(model);
    ASSERT_EQ(model.countries().size(), 5u);
    EXPECT_EQ(model.countries()[0].value, "US");
    EXPECT_EQ(model.countries()[0].count, 4u);
    EXPECT_FALSE(model.filtering());

    model.set_query("sport");
    EXPECT_TRUE(model.filtering());
    EXPECT_EQ(model.visible_count(), 2u);
    model.set_country("ES");
    ASSERT_EQ(model.visible_count(), 1u);
    EXPECT_EQ(model.channel(model.visible(0)).tvg_id, "i.es");
    model.set_quality(ptv::kQualityHd);
    EXPECT_EQ(model.visible_count(), 0u);

    model.clear_filters();
    EXPECT_FALSE(model.filtering());
    EXPECT_EQ(model.visible_count(), 12u);

    model.set_quality(ptv::kQualityFullHd);
    EXPECT_EQ(model.visible_count(), 4u);
    model.set_quality(ptv::kQualityAny);
    model.set_language("German");
    EXPECT_EQ(model.visible_count(), 2u);
    model.set_category("Kids");
    EXPECT_EQ(model.visible_count(), 1u);
    model.close();
}

TEST_F(ModelTest, TheKeyboardAnswersTheSearch)
{
    ptv::Model model(dir_);
    load(model);
    ASSERT_TRUE(model.ask_query());
    EXPECT_EQ(host::keyboard_requests(), 1);
    model.poll();
    EXPECT_EQ(model.visible_count(), 12u);
    host::set_keyboard_text("kids");
    model.poll();
    EXPECT_EQ(model.query(), "kids");
    EXPECT_EQ(model.visible_count(), 2u);
    model.close();
}

TEST_F(ModelTest, PlayingQueuesTheAddressesAndRemembersTheChannel)
{
    ptv::Model model(dir_);
    load(model);
    const unsigned ember = index_of(model, "e.fr");
    ptv::PlayRequest request;
    EXPECT_FALSE(model.take_play_request(&request));
    ASSERT_TRUE(model.play(ember));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_FALSE(model.take_play_request(&request));
    EXPECT_EQ(request.channel_name, "Ember Max (2160p)");
    ASSERT_EQ(request.urls.size(), 1u);
    EXPECT_EQ(request.urls[0], "https://streams.example.invalid/ember/index.m3u8");
    EXPECT_FALSE(request.reconnect_live);
    EXPECT_TRUE(model.is_recent(model.channel(ember)));
    model.close();

    // The menu reopens after the channel: the catalog is still there.
    const int before = host::fetch_count();
    ASSERT_TRUE(model.open());
    EXPECT_TRUE(model.has_catalog());
    EXPECT_EQ(model.group_size(ptv::Group::recent), 1u);
    EXPECT_EQ(host::fetch_count(), before);
    model.close();
}

TEST_F(ModelTest, AChannelThatFailedCanBeTriedAgain)
{
    ptv::Model model(dir_);
    load(model);
    const unsigned fern = index_of(model, "f.fr");
    const std::string id(model.channel(fern).id);
    EXPECT_EQ(model.failure(), nullptr);
    model.report_playback_failure(id.c_str(), "Fern 24", -3, 2, "The server refused the stream.");
    ASSERT_NE(model.failure(), nullptr);
    EXPECT_TRUE(model.failure()->can_retry);
    EXPECT_EQ(model.failure()->attempts, 2u);
    EXPECT_EQ(model.failure()->reason, "The server refused the stream.");

    ASSERT_TRUE(model.retry_failure());
    EXPECT_EQ(model.failure(), nullptr);
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_EQ(request.channel_id, id);

    model.report_playback_failure("gone", "", -1, 1, "");
    ASSERT_NE(model.failure(), nullptr);
    EXPECT_FALSE(model.failure()->can_retry);
    EXPECT_EQ(model.failure()->channel_name, "this channel");
    EXPECT_FALSE(model.failure()->reason.empty());
    model.dismiss_failure();
    EXPECT_EQ(model.failure(), nullptr);
    model.close();
}

TEST_F(ModelTest, APlaylistAddressIsTypedSavedAndUsed)
{
    ptv::Model model(dir_);
    load(model);
    EXPECT_FALSE(model.is_set_up(iptv::SourceKind::Custom));
    EXPECT_EQ(model.health(iptv::SourceKind::Custom), ptv::SourceHealth::empty);

    // Choosing a source that is not set up opens its form.
    model.use_source(iptv::SourceKind::Custom);
    EXPECT_EQ(host::keyboard_requests(), 1);
    EXPECT_EQ(host::keyboard_title(), "Playlist address");
    host::set_keyboard_text("not an address");
    model.poll();
    EXPECT_FALSE(model.is_set_up(iptv::SourceKind::Custom));
    std::vector<ptv::Notice> notices = model.take_notices();
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].level, ptv::Level::error);

    model.edit_source(iptv::SourceKind::Custom);
    host::set_keyboard_text("https://lists.example.invalid/mine.m3u");
    model.poll();
    EXPECT_EQ(model.custom_url(), "https://lists.example.invalid/mine.m3u");
    EXPECT_EQ(model.active_source(), iptv::SourceKind::Custom);
    EXPECT_TRUE(model.refreshing());
    EXPECT_FALSE(model.has_catalog());
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.has_catalog());
    EXPECT_EQ(model.health(iptv::SourceKind::Custom), ptv::SourceHealth::ready);
    EXPECT_TRUE(fs::exists(dir_ + "/prosperotv-custom-catalog.sqlite3"));
    model.close();

    // The next launch starts on that source.
    ptv::Model again(dir_);
    ASSERT_TRUE(again.open());
    EXPECT_EQ(again.active_source(), iptv::SourceKind::Custom);
    EXPECT_TRUE(again.has_catalog());
    again.close();
}

TEST_F(ModelTest, TheAccountFormAsksThreeThings)
{
    ptv::Model model(dir_);
    load(model);
    model.edit_source(iptv::SourceKind::Xtream);
    model.poll();
    EXPECT_EQ(host::keyboard_title(), "Xtream server (1 of 3)");
    host::set_keyboard_text("http://provider.example.invalid:8080");
    model.poll();
    model.poll();
    EXPECT_EQ(host::keyboard_title(), "Xtream user name (2 of 3)");
    host::set_keyboard_text("viewer");
    model.poll();
    model.poll();
    EXPECT_EQ(host::keyboard_title(), "Xtream password (3 of 3)");
    host::set_keyboard_text("secret");
    model.poll();
    EXPECT_TRUE(model.xtream_ready());
    EXPECT_EQ(model.active_source(), iptv::SourceKind::Xtream);
    // The stand-in network answers with a playlist, which is no account reply.
    ASSERT_TRUE(settle(model));
    EXPECT_FALSE(model.has_catalog());
    EXPECT_TRUE(model.catalog_failed());
    model.close();
}

TEST_F(ModelTest, SourcesCannotChangeWhileADownloadRuns)
{
    host::set_network(true, playlist_, 300);
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(model.refreshing());
    model.use_source(iptv::SourceKind::Custom);
    EXPECT_EQ(host::keyboard_requests(), 0);
    const std::vector<ptv::Notice> notices = model.take_notices();
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].level, ptv::Level::warning);
    model.close();
}

TEST_F(ModelTest, ClosingStopsADownloadInProgress)
{
    host::set_network(true, playlist_, 5000);
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(model.refreshing());
    const auto start = std::chrono::steady_clock::now();
    model.close();
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(model.refreshing());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(took).count(), 1500);
    EXPECT_FALSE(model.has_catalog());
    EXPECT_NE(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::refreshing);

    // And the next session starts it again.
    host::set_network(true, playlist_, 0);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(settle(model));
    EXPECT_TRUE(model.has_catalog());
    model.close();
}

TEST_F(ModelTest, WithoutAKeyboardTheFormsSaySo)
{
    host::set_keyboard_available(false);
    ptv::Model model(dir_);
    load(model);
    EXPECT_FALSE(model.keyboard_ready());
    EXPECT_FALSE(model.ask_query());
    model.edit_source(iptv::SourceKind::Custom);
    EXPECT_EQ(host::keyboard_requests(), 0);
    EXPECT_EQ(model.take_notices().size(), 2u);
    model.close();
}

// ---- lists of the size large providers send ------------------------------------

TEST_F(ModelTest, AListOfMoreThanAHundredThousandChannelsIsBrowsedLikeAnyOther)
{
    constexpr unsigned kChannels = 120000;
    std::ofstream(playlist_) << large_list::playlist(kChannels);
    const std::uintmax_t file_bytes = fs::file_size(playlist_);
    ASSERT_GT(file_bytes, 24u * 1024u * 1024u);
    // The download arrives in pieces that end anywhere.
    host::set_network_piece(8191);
    {
        ptv::Model model(dir_);
        ASSERT_TRUE(model.open());
        unsigned most = 0;
        ASSERT_TRUE(settle_large(model, &most));
        ASSERT_TRUE(model.has_catalog());
        EXPECT_EQ(model.channel_count(), kChannels);
        EXPECT_EQ(model.visible_count(), kChannels);
        // While it ran it said how far it was; and it read the whole list.
        EXPECT_GT(most, 0u);
        EXPECT_LE(most, kChannels);
        EXPECT_EQ(model.refresh_progress(), 0u);
        EXPECT_EQ(host::delivered_bytes(), file_bytes);
        EXPECT_EQ(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::ready);
        const std::vector<ptv::Notice> notices = model.take_notices();
        ASSERT_EQ(notices.size(), 1u);
        EXPECT_EQ(notices[0].body, "120,000 channels");

        // The lists and their sizes.
        EXPECT_EQ(model.group_size(ptv::Group::all), kChannels);
        EXPECT_EQ(model.group_size(ptv::Group::sports), kChannels / 8);
        EXPECT_EQ(model.group_size(ptv::Group::news), kChannels / 8);
        EXPECT_EQ(model.group_size(ptv::Group::kids), kChannels / 8);
        ASSERT_EQ(model.countries().size(), 4u);
        EXPECT_EQ(model.countries()[0].count, kChannels / 4);
        EXPECT_EQ(model.categories().size(), ptv::Model::kFacetMax);
        EXPECT_EQ(model.categories()[0].count, kChannels / 8);

        // The order of the alphabet, the letters, and where a channel is.
        EXPECT_EQ(model.channel(model.visible(0)).name, "A-Net 000000 FHD");
        EXPECT_EQ(model.channel(model.visible(1)).name, "A-Net 000026 HD");
        EXPECT_EQ(model.channel(model.visible(kChannels - 1)).name, "Z-Net 119989");
        EXPECT_EQ(model.letter_start(0), -1);
        EXPECT_EQ(model.letter_start(1), 0);
        EXPECT_EQ(model.letter_start(2), static_cast<int>(large_list::share(kChannels, 26, 0)));
        EXPECT_EQ(model.letter_start(26),
                  static_cast<int>(kChannels - large_list::share(kChannels, 26, 25)));
        EXPECT_EQ(model.letter_at(kChannels - 1), 26);
        for (const unsigned position : {0u, 1u, 4616u, 60000u, kChannels - 1})
        {
            const unsigned index = model.visible(position);
            EXPECT_EQ(model.position_of(model.channel(index).id), static_cast<int>(position));
            EXPECT_EQ(model.number_of(index), position + 1);
        }
        EXPECT_EQ(model.position_of("nothing"), -1);
        ASSERT_TRUE(model.find(model.channel(77777).id).has_value());
        EXPECT_EQ(model.find(model.channel(77777).id)->tvg_id, "c77777.example");
        EXPECT_FALSE(model.find("nothing").has_value());

        // A search and the filters narrow it, alone and together.
        model.set_query("net 077777");
        ASSERT_EQ(model.visible_count(), 1u);
        EXPECT_EQ(model.channel(model.visible(0)).tvg_id, "c77777.example");
        model.set_query("fhd");
        EXPECT_EQ(model.visible_count(), kChannels / 5);
        model.set_query("");
        model.set_quality(ptv::kQualityHd);
        EXPECT_EQ(model.visible_count(), kChannels / 5);
        model.set_country("DE");
        EXPECT_EQ(model.visible_count(), kChannels / 20);
        model.set_category("News");
        EXPECT_EQ(model.visible_count(), 0u); // news is index 1 of 8, DE is 2 of 4
        model.clear_filters();
        model.set_group(ptv::Group::sports);
        ASSERT_EQ(model.visible_count(), kChannels / 8);
        EXPECT_EQ(model.channel(model.visible(0)).name, "A-Net 000000 FHD");
        const unsigned in_sports = model.visible(9000);
        EXPECT_EQ(model.position_of(model.channel(in_sports).id), 9000);
        model.set_group(ptv::Group::all);
        EXPECT_EQ(model.visible_count(), kChannels);

        // A favorite and a watched channel far down the list.
        const unsigned starred = model.visible(100000);
        const std::string starred_id(model.channel(starred).id);
        EXPECT_EQ(model.toggle_favorite(starred), ptv::Model::Starred::added);
        EXPECT_EQ(model.group_size(ptv::Group::favorites), 1u);
        model.set_group(ptv::Group::favorites);
        ASSERT_EQ(model.visible_count(), 1u);
        EXPECT_EQ(model.visible(0), starred);
        EXPECT_EQ(model.position_of(starred_id), 0);
        model.set_group(ptv::Group::all);
        ASSERT_TRUE(model.play(model.visible(110000)));
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        EXPECT_EQ(request.urls.size(), 1u);
        model.close();

        // The menu reopens after the channel: nothing is read again.
        const int fetches = host::fetch_count();
        ASSERT_TRUE(model.open());
        EXPECT_EQ(model.channel_count(), kChannels);
        EXPECT_EQ(model.group_size(ptv::Group::recent), 1u);
        EXPECT_EQ(host::fetch_count(), fetches);
        model.close();
    }

    // The next launch opens from the copy saved on the console.
    const int fetches = host::fetch_count();
    host::set_unix_time(now_ + 3600);
    ptv::Model again(dir_);
    ASSERT_TRUE(again.open());
    EXPECT_TRUE(again.has_catalog());
    EXPECT_FALSE(again.refreshing());
    EXPECT_EQ(host::fetch_count(), fetches);
    EXPECT_EQ(again.channel_count(), kChannels);
    EXPECT_EQ(again.group_size(ptv::Group::favorites), 1u);
    EXPECT_EQ(again.group_size(ptv::Group::recent), 1u);
    EXPECT_EQ(again.channel(again.visible(kChannels - 1)).name, "Z-Net 119989");
    again.close();
}

TEST_F(ModelTest, ASourceWithMoreChannelsThanTheAppHoldsKeepsItsFirstOnesAndSaysSo)
{
    const unsigned held = static_cast<unsigned>(iptv::kDefaultMaxChannels);
    std::ofstream(playlist_) << large_list::playlist(held + 20000);
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(settle_large(model));
    ASSERT_TRUE(model.has_catalog());
    EXPECT_EQ(model.channel_count(), held);
    EXPECT_EQ(model.health(iptv::SourceKind::BuiltIn), ptv::SourceHealth::ready);
    // The download stopped where the catalog was full.
    EXPECT_LT(host::delivered_bytes(), fs::file_size(playlist_));
    const std::vector<ptv::Notice> notices = model.take_notices();
    ASSERT_EQ(notices.size(), 2u);
    EXPECT_EQ(notices[0].body, "250,000 channels");
    EXPECT_EQ(notices[1].level, ptv::Level::warning);
    EXPECT_EQ(notices[1].title, "This source has more channels than ProsperoTV holds");
    EXPECT_EQ(notices[1].body, "Showing its first 250,000.");
    model.close();
}

TEST_F(ModelTest, AnAddressThatIsNoPlaylistIsGivenUpEarly)
{
    // Four megabytes of something else: a web page, a video.
    std::string other;
    while (other.size() < 4u * 1024u * 1024u)
        other += "<p>This is not a channel list, however long it goes on.</p>\n";
    std::ofstream(playlist_) << other;
    ptv::Model model(dir_);
    ASSERT_TRUE(model.open());
    ASSERT_TRUE(settle_large(model));
    EXPECT_FALSE(model.has_catalog());
    EXPECT_TRUE(model.catalog_failed());
    EXPECT_NE(model.catalog_error().find("no channels that can be played"), std::string::npos);
    EXPECT_LT(host::delivered_bytes(), 2u * 1024u * 1024u);
    model.close();
}

// ---- the words ------------------------------------------------------------------

iptv::Channel named(const char *name, const char *url = "https://x.example.invalid/a.m3u8")
{
    iptv::Channel channel;
    channel.name = name;
    channel.url = url;
    return channel;
}

TEST(ChannelText, PlaylistNotesLeaveTheName)
{
    std::vector<std::string> notes;
    EXPECT_EQ(ptv::display_name(named("Alder News (1080p) [Not 24/7]"), &notes), "Alder News");
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0], "Not 24/7");

    notes.clear();
    EXPECT_EQ(ptv::display_name(named("Fern 24 (720p) [Geo-blocked] [Not 24/7]"), &notes),
              "Fern 24");
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0], "Geo-blocked");
    EXPECT_EQ(notes[1], "Not 24/7");

    EXPECT_EQ(ptv::display_name(named("Harbor (Kids)")), "Harbor (Kids)");
    EXPECT_EQ(ptv::display_name(named("(1080p)")), "(1080p)");
    EXPECT_EQ(ptv::display_name(named("  ")), "Unnamed channel");
    EXPECT_EQ(ptv::display_name(named("Dune One")), "Dune One");
}

TEST(ChannelText, NamesAreFiledUnderTheirLetter)
{
    EXPECT_EQ(ptv::sort_key(named("Alder News (1080p)")), "AALDER NEWS");
    // Accents are put aside, and so is punctuation.
    EXPECT_EQ(ptv::sort_key(named("\xC3\x89lan TV")), "EELAN TV");
    EXPECT_EQ(ptv::sort_key(named("\xC5\x81\xC3\xB3"
                                  "d\xC5\xBA")),
              "LLODZ");
    EXPECT_EQ(ptv::sort_key(named("(( Zed! ))")), "ZZED");
    EXPECT_EQ(ptv::sort_key(named("+Plus TV")), "PPLUS TV");
    // Digits and other scripts go under '#'.
    EXPECT_EQ(ptv::sort_key(named("24 Kitchen")), "#24 KITCHEN");
    const char *cyrillic = "\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB2\xD1\x8B\xD0\xB9";
    EXPECT_EQ(ptv::sort_key(named(cyrillic)), std::string("#") + cyrillic);
    EXPECT_EQ(ptv::letter_of_key("#24"), 0);
    EXPECT_EQ(ptv::letter_of_key("AALDER"), 1);
    EXPECT_EQ(ptv::letter_of_key("ZZED"), 26);
    EXPECT_EQ(ptv::letter_char(0), '#');
    EXPECT_EQ(ptv::letter_char(1), 'A');
    EXPECT_EQ(ptv::letter_char(26), 'Z');
}

TEST_F(ModelTest, TheListsAreInTheOrderOfTheAlphabet)
{
    // The playlist gives them in another order, and writes some with accents.
    std::ofstream(playlist_)
        << "#EXTM3U\n"
           "#EXTINF:-1 tvg-id=\"z.xx\",Zenith\nhttps://s.example.invalid/z\n"
           "#EXTINF:-1 tvg-id=\"e2.xx\",Echo Two\nhttps://s.example.invalid/e2\n"
           "#EXTINF:-1 tvg-id=\"n9.xx\",9 Live\nhttps://s.example.invalid/n9\n"
           "#EXTINF:-1 tvg-id=\"el.xx\",\xC3\x89lan\nhttps://s.example.invalid/el\n"
           "#EXTINF:-1 tvg-id=\"a.xx\",alder\nhttps://s.example.invalid/a\n"
           "#EXTINF:-1 tvg-id=\"e1.xx\",Echo One\nhttps://s.example.invalid/e1\n";
    ptv::Model model(dir_);
    load(model);
    ASSERT_EQ(model.visible_count(), 6u);
    const char *expected[] = {"n9.xx", "a.xx", "e1.xx", "e2.xx", "el.xx", "z.xx"};
    for (unsigned position = 0; position < 6; ++position)
        EXPECT_EQ(model.channel(model.visible(position)).tvg_id, expected[position]) << position;
    EXPECT_EQ(model.letter_at(0), 0);
    EXPECT_EQ(model.letter_at(1), 1);
    EXPECT_EQ(model.letter_at(4), 5);
    EXPECT_EQ(model.letter_start(0), 0);
    EXPECT_EQ(model.letter_start(1), 1);
    EXPECT_EQ(model.letter_start(5), 2);
    EXPECT_EQ(model.letter_start(26), 5);
    EXPECT_EQ(model.letter_start(2), -1); // nothing under B
    // A narrower list keeps the order and has its own letters.
    model.set_query("echo");
    ASSERT_EQ(model.visible_count(), 2u);
    EXPECT_EQ(model.letter_start(5), 0);
    EXPECT_EQ(model.letter_start(1), -1);
    model.close();
}

TEST(ChannelText, SizesAndCodecsAreReadFromTheRecord)
{
    EXPECT_EQ(ptv::quality_of(named("Ember Max (2160p)")), ptv::kQualityUhd);
    EXPECT_EQ(ptv::quality_of(named("Alder News (1080p)")), ptv::kQualityFullHd);
    EXPECT_EQ(ptv::quality_of(named("Bramble HD")), ptv::kQualityHd);
    EXPECT_EQ(ptv::quality_of(named("Clover (576p)")), ptv::kQualitySd);
    EXPECT_EQ(ptv::quality_of(named("Dune One")), ptv::kQualityAny);
    EXPECT_EQ(ptv::resolution_label(named("Clover (576p)")), "576p");
    EXPECT_EQ(ptv::resolution_label(named("Ember Max (2160p)")), "4K");
    EXPECT_EQ(ptv::resolution_label(named("Dune One")), "");
    EXPECT_STREQ(ptv::codec_label(named("Dune", "https://x.example.invalid/hevc/a.m3u8")), "HEVC");
    EXPECT_STREQ(ptv::codec_label(named("Dune")), "");
}

TEST(ChannelText, MonogramsPlacesAndNumbers)
{
    EXPECT_EQ(ptv::monogram(named("Alder News")), "AN");
    EXPECT_EQ(ptv::monogram(named("Kestrel")), "KE");
    EXPECT_EQ(ptv::monogram(named("\xE4\xB8\xAD\xE6\x96\x87")), "\xE4\xB8\xAD");
    iptv::Channel channel = named("Alder");
    EXPECT_EQ(ptv::place_line(channel), "World");
    EXPECT_EQ(ptv::category_of(channel), "Uncategorized");
    channel.tvg_country = "US;CA";
    channel.tvg_language = "English";
    channel.group_title = "News;Business";
    EXPECT_EQ(ptv::place_line(channel), "US  \xC2\xB7  English");
    EXPECT_EQ(ptv::category_of(channel), "News");
    EXPECT_EQ(ptv::group_digits(0), "0");
    EXPECT_EQ(ptv::group_digits(999), "999");
    EXPECT_EQ(ptv::group_digits(12886), "12,886");
    EXPECT_EQ(ptv::group_digits(1234567), "1,234,567");
}

} // namespace
