// ProsperoTV - MAC portal authentication, catalog and playback contracts.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/portal.hpp"
#include "tv/model.hpp"
#include "host_platform.hpp"
#include "iptv_store.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

namespace
{
class PortalTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        host::reset();
        char pattern[] = "/tmp/prosperotv-portal-XXXXXX";
        dir = mkdtemp(pattern);
        host::set_network(true, "");
        host::set_network_piece(1);
        response("type=stb&action=handshake&token=", R"({"js":{"token":"session-token"}})");
        response(
            "type=stb&action=get_profile&stb_type=MAG250&hd=1&auth_second_step=0&not_valid_token=0",
            R"({"js":{"id":42,"status":0}})");
        response(
            "type=itv&action=get_genres",
            R"({"js":[{"id":"1","title":"US"},{"id":"2","title":"Sports","parent_id":"1"},{"id":"3","title":"Football","parent_id":"2"}]})");
        response(
            "type=itv&action=get_all_channels",
            R"({"js":{"total_items":2,"data":[{"id":"12","name":"Sports One","cmd":"ffmpeg http://localhost/ch/12_","tv_genre_id":"3","logo":"/logos/sports.png","xmltv_id":"sports.one"},{"id":13,"name":"News","cmd":"ffmpeg https://live.invalid/news.ts","logo":null,"tv_genre_id":1}]}})");
        ptv::platform::network_init();
    }
    void TearDown() override
    {
        ptv::platform::network_shutdown();
        host::reset();
        std::filesystem::remove_all(dir);
    }
    void response(const std::string &query, std::string_view body)
    {
        const auto path = dir + "/reply-" + std::to_string(number++) + ".json";
        std::ofstream(path) << body;
        host::set_network_response(credentials.url + "?" + query + "&JsHttpRequest=1-xml", path);
    }
    const ptv::PortalCredentials credentials{
        "https://portal.invalid/stalker_portal/server/load.php", "00:1A:79:12:34:56"};
    std::string dir;
    int number = 0;
};

TEST(PortalAddress, AcceptsPortalPagesAndExplicitEndpointsAndNormalizesMac)
{
    std::string result;
    for (const auto input : {"https://portal.invalid/stalker_portal/c/",
                             "https://portal.invalid/stalker_portal/c/index.html",
                             "https://portal.invalid/stalker_portal"})
    {
        ASSERT_TRUE(ptv::portal_endpoint(input, &result));
        EXPECT_EQ(result, "https://portal.invalid/stalker_portal/server/load.php");
    }
    ASSERT_TRUE(ptv::portal_endpoint("https://portal.invalid/portal.php", &result));
    EXPECT_EQ(result, "https://portal.invalid/portal.php");
    EXPECT_FALSE(ptv::portal_endpoint("file:///portal", &result));
    EXPECT_FALSE(ptv::portal_endpoint("https://user:password@portal.invalid", &result));
    EXPECT_FALSE(ptv::portal_endpoint("https://portal.invalid/?token=x", &result));
    ASSERT_TRUE(ptv::portal_mac("00-1a-79-12-ab-cd", &result));
    EXPECT_EQ(result, "00:1A:79:12:AB:CD");
    EXPECT_FALSE(ptv::portal_mac("00:1A:79:12:AB:ZZ", &result));
    EXPECT_FALSE(ptv::portal_mac("00:1A:79:12:AB", &result));
}

TEST_F(PortalTest, SignsInStreamsNestedCategoriesAndPersistsOpaqueCommands)
{
    ptv::PortalClient client(credentials, nullptr);
    iptv::Catalog catalog;
    iptv::ParseReport report;
    ASSERT_TRUE(client.load(&catalog, &report)) << client.error();
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(report.accepted, 2u);
    EXPECT_EQ(catalog[0].group_title, "US / Sports / Football");
    EXPECT_EQ(catalog[0].tvg_logo, "https://portal.invalid/logos/sports.png");
    EXPECT_EQ(catalog[0].tvg_id, "sports.one");
    EXPECT_EQ(catalog[1].tvg_id, "13");
    EXPECT_TRUE(catalog[1].tvg_logo.empty());
    const auto requests = host::requests();
    ASSERT_EQ(requests.size(), 4u);
    EXPECT_EQ(requests[0].cookie, "mac=00%3A1A%3A79%3A12%3A34%3A56; stb_lang=en; timezone=UTC");
    EXPECT_TRUE(requests[0].authorization.empty());
    EXPECT_EQ(requests[1].authorization, "Bearer session-token");
    EXPECT_EQ(requests[3].authorization, "Bearer session-token");
    ASSERT_EQ(iptv::SaveCatalog(dir + "/catalog.sqlite3", catalog), iptv::StoreStatus::ok);
    iptv::Catalog loaded;
    ASSERT_EQ(iptv::LoadCatalog(dir + "/catalog.sqlite3", &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded[0].portal_command, "ffmpeg http://localhost/ch/12_");
    EXPECT_EQ(loaded[0].id, catalog[0].id);
}

TEST_F(PortalTest, ResolvesACommandOnlyThroughTheProvidersCreateLink)
{
    const std::string query = "type=itv&action=create_link&cmd=ffmpeg%20http%3A%2F%2Flocalhost%"
                              "2Fch%2F12_&forced_storage=undefined&disable_ad=0";
    response(query, R"({"js":{"cmd":"ffmpeg https://cdn.invalid/stream.ts?token=temporary"}})");
    ptv::PortalClient client(credentials, nullptr);
    std::string url;
    ASSERT_TRUE(client.resolve("ffmpeg http://localhost/ch/12_", &url)) << client.error();
    EXPECT_EQ(url, "https://cdn.invalid/stream.ts?token=temporary");
    response(query, R"({"js":{"cmd":"ffmpeg file:///not-a-stream"}})");
    EXPECT_FALSE(client.resolve("ffmpeg http://localhost/ch/12_", &url));
    response(
        "type=stb&action=get_profile&stb_type=MAG250&hd=1&auth_second_step=0&not_valid_token=0",
        R"({"js":{"status":1,"msg":"blocked"}})");
    EXPECT_FALSE(client.sign_in());
    EXPECT_NE(client.error().find("active subscription"), std::string::npos);
}

TEST_F(PortalTest, SavesTheTwoFieldFormAndReopensItsCachedChannels)
{
    const auto settle = [](ptv::Model &model)
    {
        for (int i = 0; i < 400; ++i)
        {
            model.poll();
            if (!model.refreshing())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    std::string first;
    {
        ptv::Model model(dir);
        ASSERT_TRUE(model.open());
        ASSERT_TRUE(settle(model));
        model.edit_saved_source(4);
        model.poll();
        EXPECT_EQ(host::keyboard_title(), "Portal address (1 of 2)");
        host::set_keyboard_text("https://portal.invalid/stalker_portal/c/");
        model.poll();
        model.poll();
        EXPECT_EQ(host::keyboard_title(), "Portal MAC code (2 of 2)");
        host::set_keyboard_text("00-1a-79-12-34-56");
        model.poll();
        ASSERT_TRUE(settle(model));
        ASSERT_EQ(model.active_source(), iptv::SourceKind::Portal);
        ASSERT_EQ(model.channel_count(), 2u);
        EXPECT_EQ(model.selected_source_id(), 4);
        ASSERT_TRUE(model.set_schedule(4, ptv::RefreshSchedule::manual));
        ASSERT_TRUE(model.play(0));
        ptv::PlayRequest request;
        ASSERT_TRUE(model.take_play_request(&request));
        EXPECT_EQ(request.portal.url, credentials.url);
        EXPECT_EQ(request.portal.mac, credentials.mac);
        EXPECT_EQ(request.portal_command, "ffmpeg http://localhost/ch/12_");
        first = request.channel_id;
        model.close();
    }
    const auto requests = host::requests().size();
    ptv::Model again(dir);
    ASSERT_TRUE(again.open());
    ASSERT_EQ(again.channel_count(), 2u);
    EXPECT_EQ(again.active_source(), iptv::SourceKind::Portal);
    EXPECT_FALSE(again.refreshing());
    EXPECT_EQ(host::requests().size(), requests);
    ASSERT_TRUE(again.play(0));
    ptv::PlayRequest request;
    ASSERT_TRUE(again.take_play_request(&request));
    EXPECT_EQ(request.channel_id, first);
    EXPECT_EQ(request.portal_command, "ffmpeg http://localhost/ch/12_");
    again.close();
}

TEST_F(PortalTest, ResolvingOnAWorkerCompletesAndCanBeCancelled)
{
    response("type=itv&action=create_link&cmd=live&forced_storage=undefined&disable_ad=0",
             R"({"js":{"cmd":"ffmpeg https://cdn.invalid/live.ts"}})");
    ptv::PortalResolveJob job;
    ASSERT_TRUE(job.start(credentials, "live"));
    for (int i = 0; i < 400 && !job.done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(job.done());
    job.cancel();
    ASSERT_TRUE(job.succeeded()) << job.error();
    EXPECT_EQ(job.url(), "https://cdn.invalid/live.ts");
    host::set_network(true, "", 5000);
    ASSERT_TRUE(job.start(credentials, "live"));
    const auto start = std::chrono::steady_clock::now();
    job.cancel();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    EXPECT_TRUE(job.done());
    EXPECT_TRUE(job.url().empty());
}
} // namespace
