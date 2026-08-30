/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_catalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{

constexpr std::uint64_t kSourceId = 0x1234u;

bool HasIssue(const iptv::ParseReport &report, iptv::ParseIssueCode code)
{
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                       [code](const iptv::ParseDiagnostic &diagnostic)
                       { return diagnostic.code == code; });
}

TEST(IptvCatalogTest, ParsesCrLfHttpOptionsAndSearchMetadata)
{
    const std::string playlist = "\xef\xbb\xbf#EXTM3U\r\n"
                                 "#EXTINF:-1 TVG-ID=\"news.br\" TVG-COUNTRY=\"BR\" "
                                 "TVG-LANGUAGE=\"Portuguese\",Brazil News\r\n"
                                 "#EXTVLCOPT:HTTP-USER-AGENT=Mozilla/5.0 (PlayStation 5)\r\n"
                                 "#EXTVLCOPT:http-referrer=https://portal.example/watch?id=7\r\n"
                                 "HTTPS://CDN.Example/live.m3u8#ignored-fragment\r\n";

    iptv::ParseReport report;
    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, {}, &report);

    ASSERT_EQ(catalog.channels.size(), 1u);
    const iptv::Channel &channel = catalog.channels.front();
    EXPECT_EQ(channel.tvg_country, "BR");
    EXPECT_EQ(channel.tvg_language, "Portuguese");
    EXPECT_EQ(channel.http_user_agent, "Mozilla/5.0 (PlayStation 5)");
    EXPECT_EQ(channel.http_referrer, "https://portal.example/watch?id=7");
    EXPECT_EQ(channel.url, "https://cdn.example/live.m3u8");
    EXPECT_EQ(channel.source_line, 2u);
    EXPECT_EQ(report.accepted, 1u);
    EXPECT_EQ(report.skipped, 0u);
}

TEST(IptvCatalogTest, AcceptsRefererAliasAndTrimsOptionWhitespace)
{
    constexpr std::string_view playlist =
        "#EXTM3U\n"
        "#EXTINF:-1,Alias\n"
        "#extvlcopt: http-user-agent =  Agent with spaces  \n"
        "#EXTVLCOPT:http-referer=  https://origin.example/page  \n"
        "http://stream.example/live\n";

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId);

    ASSERT_EQ(catalog.channels.size(), 1u);
    EXPECT_EQ(catalog.channels[0].http_user_agent, "Agent with spaces");
    EXPECT_EQ(catalog.channels[0].http_referrer, "https://origin.example/page");
}

TEST(IptvCatalogTest, RejectsControlCharactersInHttpOptionValues)
{
    std::string playlist = "#EXTM3U\n"
                           "#EXTINF:-1,Unsafe headers\n"
                           "#EXTVLCOPT:http-user-agent=SafeAgent";
    playlist.push_back('\r');
    playlist += "Injected: true\n"
                "#EXTVLCOPT:http-referrer=https://origin.example/";
    playlist.push_back(static_cast<char>(0x7f));
    playlist += "Injected\nhttp://stream.example/live\n";

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId);

    ASSERT_EQ(catalog.channels.size(), 1u);
    EXPECT_TRUE(catalog.channels[0].http_user_agent.empty());
    EXPECT_TRUE(catalog.channels[0].http_referrer.empty());
}

TEST(IptvCatalogTest, DoesNotLeakOptionsAcrossRecords)
{
    constexpr std::string_view playlist = "#EXTM3U\n"
                                          "#EXTVLCOPT:http-user-agent=BeforeRecord\n"
                                          "#EXTINF:-1,Broken\n"
                                          "#EXTVLCOPT:http-user-agent=BrokenAgent\n"
                                          "ftp://stream.example/broken\n"
                                          "#EXTINF:-1,Clean\n"
                                          "http://stream.example/clean\n";

    iptv::ParseReport report;
    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, {}, &report);

    ASSERT_EQ(catalog.channels.size(), 1u);
    EXPECT_EQ(catalog.channels[0].name, "Clean");
    EXPECT_TRUE(catalog.channels[0].http_user_agent.empty());
    EXPECT_TRUE(catalog.channels[0].http_referrer.empty());
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::unsupported_url_scheme));
}

TEST(IptvCatalogTest, IgnoresUnknownMalformedAndOversizedHttpOptions)
{
    constexpr std::string_view playlist = "#EXTM3U\n"
                                          "#EXTINF:-1,Channel\n"
                                          "#EXTVLCOPT:http-user-agent\n"
                                          "#EXTVLCOPT:network-caching=1000\n"
                                          "#EXTVLCOPT:http-user-agent=123456789\n"
                                          "http://stream.example/live\n";
    iptv::ParseLimits limits;
    limits.max_field_bytes = 8;

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, limits);

    ASSERT_EQ(catalog.channels.size(), 1u);
    EXPECT_TRUE(catalog.channels[0].http_user_agent.empty());
}

TEST(IptvCatalogTest, KeepsCountryAndLanguageWhenMergingDuplicates)
{
    constexpr std::string_view playlist = "#EXTM3U\n"
                                          "#EXTINF:-1 tvg-id=\"station\",First\n"
                                          "http://one.example/live\n"
                                          "#EXTINF:-1 tvg-id=\"STATION\" tvg-country=\"CA\" "
                                          "tvg-language=\"French\",Second\n"
                                          "http://two.example/live\n";

    iptv::ParseReport report;
    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, {}, &report);

    ASSERT_EQ(catalog.channels.size(), 1u);
    EXPECT_EQ(catalog.channels[0].tvg_country, "CA");
    EXPECT_EQ(catalog.channels[0].tvg_language, "French");
    ASSERT_EQ(catalog.channels[0].alternate_urls.size(), 1u);
    EXPECT_EQ(catalog.channels[0].alternate_urls[0], "http://two.example/live");
    EXPECT_EQ(report.duplicates, 1u);
}

TEST(IptvCatalogTest, RejectsPlaylistBeyondConfiguredLimit)
{
    constexpr std::string_view playlist = "#EXTINF:-1,One\nhttp://one.example/live\n";
    iptv::ParseLimits limits;
    limits.max_playlist_bytes = playlist.size() - 1u;
    iptv::ParseReport report;

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, limits, &report);

    EXPECT_TRUE(catalog.channels.empty());
    EXPECT_TRUE(report.input_too_large);
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::input_too_large));
}

TEST(IptvCatalogTest, EnforcesFieldUrlAndChannelLimits)
{
    constexpr std::string_view playlist = "#EXTM3U\n"
                                          "#EXTINF:-1,TitleTooLong\n"
                                          "http://field.example/live\n"
                                          "#EXTINF:-1,One\n"
                                          "http://one.example/path-is-too-long\n"
                                          "#EXTINF:-1,Two\n"
                                          "http://two.example/x\n"
                                          "#EXTINF:-1,Three\n"
                                          "http://three.example/x\n";
    iptv::ParseLimits limits;
    limits.max_field_bytes = 5;
    limits.max_url_bytes = 26;
    limits.max_channels = 1;
    iptv::ParseReport report;

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, limits, &report);

    ASSERT_EQ(catalog.channels.size(), 1u);
    EXPECT_EQ(catalog.channels[0].name, "Two");
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::attribute_too_long));
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::url_too_long));
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::catalog_full));
}

TEST(IptvCatalogTest, OverlongRecordClearsPendingEntry)
{
    constexpr std::string_view playlist = "#EXTINF:-1,Pending\n"
                                          "# this comment is deliberately too long\n"
                                          "http://stream.example/live\n";
    iptv::ParseLimits limits;
    limits.max_record_bytes = 30;
    iptv::ParseReport report;

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, limits, &report);

    EXPECT_TRUE(catalog.channels.empty());
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::overlong_record));
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::url_without_extinf));
}

TEST(IptvCatalogTest, ReportsMalformedAndIncompleteEntries)
{
    constexpr std::string_view playlist = "#EXTM3U\n"
                                          "http://orphan.example/live\n"
                                          "#EXTINF:not-a-duration,Bad duration\n"
                                          "#EXTINF:-1 tvg-id=\"unterminated,Bad quote\n"
                                          "#EXTINF:-1,Missing URL\n";
    iptv::ParseReport report;

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, {}, &report);

    EXPECT_TRUE(catalog.channels.empty());
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::url_without_extinf));
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::malformed_extinf));
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::missing_url));
}

TEST(IptvCatalogTest, RejectsUnsafeOrMalformedStreamUrls)
{
    constexpr std::string_view playlist = "#EXTINF:-1,Credentials\n"
                                          "https://user:password@example.com/live\n"
                                          "#EXTINF:-1,Whitespace\n"
                                          "https://example.com/live stream\n"
                                          "#EXTINF:-1,Bad port\n"
                                          "https://example.com:not-a-port/live\n";
    iptv::ParseReport report;

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, {}, &report);

    EXPECT_TRUE(catalog.channels.empty());
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::unsafe_url));
    EXPECT_TRUE(HasIssue(report, iptv::ParseIssueCode::malformed_url));
}

TEST(IptvCatalogTest, CapsStoredDiagnosticsWithoutHidingSkippedCount)
{
    constexpr std::string_view playlist = "http://one.example/live\n"
                                          "http://two.example/live\n"
                                          "http://three.example/live\n";
    iptv::ParseLimits limits;
    limits.max_diagnostics = 1;
    iptv::ParseReport report;

    const iptv::CatalogState catalog = iptv::ParseExtendedM3u(playlist, kSourceId, limits, &report);

    EXPECT_TRUE(catalog.channels.empty());
    EXPECT_EQ(report.skipped, 3u);
    ASSERT_EQ(report.diagnostics.size(), 1u);
    EXPECT_EQ(report.diagnostics[0].line, 1u);
}

} // namespace
