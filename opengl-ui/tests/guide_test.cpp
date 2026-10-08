// ProsperoTV - Programme parsing, storage and provider playback regressions.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/guide.hpp"
#include "tv/model.hpp"
#include "host_platform.hpp"
#include "iptv_store.h"
#include "iptv_xtream.h"
#include <gtest/gtest.h>
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>

namespace
{
constexpr char kPlaylist[] =
    "#EXTM3U x-tvg-url=\"https://guide.invalid/guide.xml.gz\" catchup=\"default\" "
    "catchup-days=\"7\"\n"
    "#EXTINF:-1 tvg-id=\"station\" "
    "catchup-source=\"https://archive.invalid/stream?start={utc}&end={utcend}\" group-title=\"US / "
    "Sports / Football\",Sports One\n"
    "https://live.invalid/one.m3u8\n"
    "#EXTINF:-1 tvg-name=\"News Two\" group-title=\"US / News\",News "
    "Two\nhttps://live.invalid/two.ts\n";
constexpr char kXml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE tv SYSTEM \"xmltv.dtd\"><tv>"
    "<channel id=\"news\"><display-name>News Two</display-name></channel>"
    "<programme channel=\"station\" start=\"20261008170000 +0100\" stop=\"20261008170000 "
    "+0000\"><title>Football &amp; friends</title><desc>Live from the stadium</desc></programme>"
    "<programme channel=\"station\" start=\"20261008170000 +0000\" stop=\"20261008180000 "
    "+0000\"><title><![CDATA[Post-match <analysis>]]></title></programme>"
    "<programme channel=\"station\" start=\"20261008150000 +0000\" stop=\"20261008160000 "
    "+0000\"><title>Previous match</title></programme>"
    "<programme channel=\"news\" start=\"20261008160000 "
    "+0000\"><title>Headlines</title></programme>"
    "<programme channel=\"news\" start=\"20261008170000 +0000\"><title>Weather</title></programme>"
    "<programme channel=\"unknown\" start=\"20261008160000 +0000\" stop=\"20261008170000 "
    "+0000\"><title>Not in our list</title></programme>"
    "</tv>";

class GuideTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        host::reset();
        char pattern[] = "/tmp/prosperotv-guide-XXXXXX";
        dir = mkdtemp(pattern);
        catalog = iptv::ParseExtendedM3u(kPlaylist, 42);
        now = ptv::xmltv_time("20261008163000 +0000");
        host::set_unix_time(static_cast<std::uint64_t>(now));
    }
    void TearDown() override
    {
        host::reset();
        std::filesystem::remove_all(dir);
    }
    void parse(ptv::Guide &guide, std::string_view xml = kXml)
    {
        ptv::XmltvReader reader(catalog, now, guide);
        for (const char &c : xml)
            ASSERT_TRUE(reader.feed({&c, 1}));
        ASSERT_TRUE(reader.finish());
    }
    static void settle(ptv::Model &model)
    {
        for (int i = 0; i < 600; ++i)
        {
            model.poll();
            if (!model.refreshing() && !model.guide_refreshing())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL() << "Guide worker did not finish";
    }
    iptv::Catalog catalog;
    std::string dir;
    std::int64_t now = 0;
};

TEST_F(GuideTest, TimezonesGapsAndInvalidDates)
{
    EXPECT_EQ(ptv::xmltv_time("20261008173000 +0100"), now);
    EXPECT_EQ(ptv::xmltv_time("20261008123000 -0400"), now);
    EXPECT_EQ(ptv::xmltv_time("202610081630"), now);
    EXPECT_EQ(ptv::xmltv_time("20261008173000 BST"), now);
    EXPECT_EQ(ptv::xmltv_time("20260230123000 +0000"), 0);
    EXPECT_EQ(ptv::xmltv_time("20261008253000 +0000"), 0);
    EXPECT_EQ(ptv::xmltv_time("20261008163000 +0060"), 0);
    EXPECT_EQ(ptv::xmltv_time("invalid"), 0);
    ptv::Guide guide;
    parse(guide);
    ASSERT_EQ(guide.count(), 5u);
    ASSERT_NE(guide.now(catalog[0].id, now), nullptr);
    EXPECT_EQ(guide.now(catalog[0].id, now)->title, "Football & friends");
    EXPECT_EQ(guide.next(catalog[0].id, now)->title, "Post-match <analysis>");
    EXPECT_EQ(guide.now(catalog[0].id, now + 1800)->title, "Post-match <analysis>");
    EXPECT_EQ(guide.now(catalog[0].id, now + 5400), nullptr);
    EXPECT_EQ(guide.now(catalog[1].id, now)->title, "Headlines");
    EXPECT_EQ(guide.now(catalog[1].id, now + 3600), nullptr); // Unknown last stop is not guessed.
    EXPECT_TRUE(guide.matches_now(catalog[0].id, "football", now));
    EXPECT_FALSE(guide.matches_now(catalog[0].id, "previous", now));
}

TEST_F(GuideTest, GzipAndCacheRoundTripKeepTheLastGoodCopy)
{
    ptv::Guide guide;
    const std::string file = dir + "/guide.xml.gz";
    gzFile zip = gzopen(file.c_str(), "wb");
    ASSERT_NE(zip, nullptr);
    ASSERT_EQ(gzwrite(zip, kXml, sizeof(kXml) - 1), static_cast<int>(sizeof(kXml) - 1));
    ASSERT_EQ(gzclose(zip), Z_OK);
    std::ifstream input(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    ptv::XmltvReader reader(catalog, now, guide);
    for (std::size_t i = 0; i < bytes.size(); i += 3)
        ASSERT_TRUE(reader.feed(std::string_view(bytes).substr(i, 3)));
    ASSERT_TRUE(reader.finish());
    guide.saved_unix = static_cast<std::uint64_t>(now);
    ASSERT_TRUE(guide.save(dir + "/cache.db"));
    ptv::Guide loaded;
    ASSERT_TRUE(loaded.load(dir + "/cache.db", 42));
    EXPECT_EQ(loaded.count(), 5u);
    EXPECT_EQ(loaded.saved_unix, static_cast<std::uint64_t>(now));
    EXPECT_FALSE(loaded.load(dir + "/cache.db", 43));
    EXPECT_EQ(loaded.count(), 5u);
    ptv::Guide partial;
    ptv::XmltvReader truncated(catalog, now, partial);
    ASSERT_TRUE(truncated.feed(std::string_view(bytes).substr(0, bytes.size() - 4)));
    EXPECT_FALSE(truncated.finish());
    ptv::XmltvReader bad(catalog, now, partial);
    EXPECT_FALSE(bad.feed("<tv><programme></tv>"));
    ptv::XmltvReader wrong(catalog, now, partial);
    EXPECT_FALSE(wrong.feed("<html/>"));
    EXPECT_FALSE(wrong.finish());
}

TEST_F(GuideTest, PlaylistAndXtreamMetadataSurviveCatalogStorage)
{
    ASSERT_EQ(catalog.guide_urls.size(), 1u);
    ASSERT_EQ(catalog[0].catchup, "default");
    EXPECT_EQ(catalog[0].catchup_days, "7");
    ASSERT_EQ(iptv::SaveCatalog(dir + "/channels.db", catalog), iptv::StoreStatus::ok);
    iptv::Catalog loaded;
    ASSERT_EQ(iptv::LoadCatalog(dir + "/channels.db", &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.guide_urls, catalog.guide_urls);
    EXPECT_EQ(loaded[0].catchup_source, catalog[0].catchup_source);
    const iptv::XtreamCredentials account{"https://service.invalid/sub", "user@home", "p&ss"};
    std::string url;
    ASSERT_TRUE(iptv::BuildXtreamGuideUrl(account, &url));
    EXPECT_EQ(url, "https://service.invalid/sub/xmltv.php?username=user%40home&password=p%26ss");
    ASSERT_EQ(
        iptv::ParseXtreamLiveStreams(
            "[{\"stream_id\":123,\"name\":\"Sports\",\"tv_archive\":1,\"tv_archive_duration\":7}]",
            account, {}, 43, &loaded),
        iptv::XtreamStatus::ok);
    EXPECT_EQ(loaded[0].catchup, "xc");
    EXPECT_EQ(loaded[0].catchup_days, "7");
    const ptv::Programme old{now - 5400, now - 1800, "Previous", {}, {}};
    EXPECT_EQ(
        ptv::catchup_url(loaded[0], old, now),
        "https://service.invalid/sub/timeshift/user%40home/p%26ss/60/2026-10-08:15-00/123.ts");
}

TEST_F(GuideTest, CatchupIsLimitedToTheAdvertisedArchive)
{
    const ptv::Programme old{now - 5400, now - 1800, "Previous", {}, {}};
    EXPECT_EQ(ptv::catchup_url(catalog[0], old, now),
              "https://archive.invalid/stream?start=" + std::to_string(old.start) +
                  "&end=" + std::to_string(old.end));
    EXPECT_TRUE(ptv::catchup_url(catalog[0], old, now + 8 * 86400).empty());
    EXPECT_TRUE(
        ptv::catchup_url(catalog[0], {now - 1800, now + 1800, "Live", {}, {}}, now).empty());
    auto channel = catalog[0].Copy();
    channel.catchup = "append";
    channel.catchup_source = "?start={Y}{m}{d}{H}{M}{S}&duration={duration}";
    EXPECT_EQ(ptv::catchup_url(channel, old, now),
              "https://live.invalid/one.m3u8?start=20261008150000&duration=3600");
    channel.catchup_source = "?start={unsupported}";
    EXPECT_TRUE(ptv::catchup_url(channel, old, now).empty());
}

TEST_F(GuideTest, DownloadSearchPlaybackAndFailedRefreshWorkTogether)
{
    const auto playlist = dir + "/list.m3u", xml = dir + "/guide.xml";
    std::ofstream(playlist) << kPlaylist;
    std::ofstream(xml) << kXml;
    host::set_network(true, playlist);
    host::set_network_response("https://guide.invalid/guide.xml.gz", xml);
    ptv::Model model(dir);
    ASSERT_TRUE(model.open());
    settle(model);
    ASSERT_EQ(model.guide().count(), 5u);
    model.set_query("football");
    ASSERT_EQ(model.visible_count(), 1u);
    const auto index = model.visible(0);
    const auto entries = model.guide().programmes(model.channel(index).id);
    ASSERT_EQ(entries.size(), 3u);
    ASSERT_TRUE(model.play_programme(index, entries[0]));
    ptv::PlayRequest request;
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_FALSE(request.reconnect_live);
    EXPECT_FALSE(request.record_channel_result);
    EXPECT_NE(request.urls[0].find("archive.invalid"), request.urls[0].npos);
    EXPECT_FALSE(model.play_programme(index, entries[2]));
    ASSERT_TRUE(model.play_programme(index, entries[1]));
    ASSERT_TRUE(model.take_play_request(&request));
    EXPECT_TRUE(request.record_channel_result);
    EXPECT_EQ(request.urls[0], model.channel(index).url);
    host::set_network(false, "");
    model.refresh_guide();
    settle(model);
    EXPECT_EQ(model.guide().count(), 5u);
    EXPECT_NE(model.guide_status().find("failed"), model.guide_status().npos);
    model.close();
    ptv::Model offline(dir);
    ASSERT_TRUE(offline.open());
    settle(offline);
    EXPECT_EQ(offline.guide().count(), 5u);
    offline.close();
}
} // namespace
