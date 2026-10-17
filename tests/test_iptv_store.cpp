/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
namespace fs = std::filesystem;

constexpr std::uint64_t kSourceId = 0x123456789abcdef0ULL;

iptv::Channel MakeChannel(std::size_t index, std::uint64_t source_id = kSourceId)
{
    iptv::Channel channel;
    channel.id = "channel-" + std::to_string(index);
    channel.source_id = source_id;
    channel.name = "Channel " + std::to_string(index);
    channel.url = "https://primary.example/live/" + std::to_string(index) + ".m3u8";
    channel.alternate_urls = {
        "https://backup-one.example/live/" + std::to_string(index),
        "https://backup-two.example/live/" + std::to_string(index),
    };
    channel.tvg_id = "station." + std::to_string(index);
    channel.channel_number = std::to_string(index + 101) + ".2";
    channel.tvg_name = "Guide name " + std::to_string(index);
    channel.tvg_logo = "https://logos.example/" + std::to_string(index) + ".png";
    channel.group_title = "News";
    channel.alternate_group_titles = {"Local", "International"};
    channel.tvg_country = "US";
    channel.tvg_language = "English";
    channel.http_user_agent = "ProsperoTV-test/1.0 (channel " + std::to_string(index) + ")";
    channel.http_referrer = "https://portal.example/watch/" + std::to_string(index);
    channel.source_line = static_cast<std::uint32_t>(index * 2u + 2u);
    return channel;
}

iptv::Catalog MakeCatalog(std::size_t count, std::uint64_t source_id = kSourceId)
{
    iptv::Catalog catalog;
    catalog.source_id = source_id;
    for (std::size_t index = 0; index < count; ++index)
        EXPECT_TRUE(catalog.Add(MakeChannel(index, source_id)));
    return catalog;
}

void ExpectChannelEquals(const iptv::ChannelView &expected, const iptv::ChannelView &actual)
{
    EXPECT_EQ(actual.media_kind, expected.media_kind);
    EXPECT_EQ(actual.channel_number, expected.channel_number);
    EXPECT_EQ(actual.id, expected.id);
    EXPECT_EQ(actual.source_id, expected.source_id);
    EXPECT_EQ(actual.name, expected.name);
    EXPECT_EQ(actual.url, expected.url);
    EXPECT_EQ(actual.alternate_urls, expected.alternate_urls);
    EXPECT_EQ(actual.tvg_id, expected.tvg_id);
    EXPECT_EQ(actual.tvg_name, expected.tvg_name);
    EXPECT_EQ(actual.tvg_logo, expected.tvg_logo);
    EXPECT_EQ(actual.group_title, expected.group_title);
    EXPECT_EQ(actual.alternate_group_titles, expected.alternate_group_titles);
    EXPECT_EQ(actual.tvg_country, expected.tvg_country);
    EXPECT_EQ(actual.tvg_language, expected.tvg_language);
    EXPECT_EQ(actual.http_user_agent, expected.http_user_agent);
    EXPECT_EQ(actual.http_referrer, expected.http_referrer);
    EXPECT_EQ(actual.source_line, expected.source_line);
    EXPECT_EQ(actual.playback_status, expected.playback_status);
    EXPECT_EQ(actual.playback_result, expected.playback_result);
    EXPECT_EQ(actual.playback_checked_unix, expected.playback_checked_unix);
}

class IptvStoreTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / ("prosperotv-store-test-" + std::to_string(tick) + "-" +
                                             std::to_string(sequence.fetch_add(1)));
        ASSERT_TRUE(fs::create_directories(root_));
        path_ = root_ / "catalog.sqlite3";
        history_path_ = root_ / "playback-history.sqlite3";
    }

    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    fs::path StagingPath() const
    {
        return fs::path(path_.string() + ".new");
    }
    fs::path BackupPath() const
    {
        return fs::path(path_.string() + ".bak");
    }

    static void WriteBytes(const fs::path &path, const std::string &bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }

    fs::path root_;
    fs::path path_;
    fs::path history_path_;
};

TEST_F(IptvStoreTest, EmptyVodCacheRequiresExplicitOptIn)
{
    const auto empty = MakeCatalog(0);
    EXPECT_EQ(iptv::SaveCatalog(path_.string(), empty), iptv::StoreStatus::invalid_argument);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), empty, {.allow_empty = true}),
              iptv::StoreStatus::ok);
    auto loaded = MakeCatalog(1);
    EXPECT_NE(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    ASSERT_EQ(loaded.size(), 1u);
    iptv::StoreReport report;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded, {.allow_empty = true}, &report),
              iptv::StoreStatus::ok);
    EXPECT_TRUE(loaded.empty());
    EXPECT_EQ(loaded.source_id, kSourceId);
    EXPECT_GT(report.saved_unix, 0u);
}

TEST_F(IptvStoreTest, RoundTripsMediaKindsAndReadsVersionFiveAsLive)
{
    iptv::Catalog catalog;
    catalog.source_id = kSourceId;
    for (int kind = 0; kind < 3; ++kind)
    {
        auto channel = MakeChannel(kind);
        channel.media_kind = static_cast<iptv::MediaKind>(kind);
        ASSERT_TRUE(catalog.Add(channel));
    }
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), catalog), iptv::StoreStatus::ok);
    iptv::Catalog loaded;
    iptv::StoreReport report;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded, {}, &report), iptv::StoreStatus::ok);
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(report.catalog_version, 7u);
    for (std::size_t i = 0; i < loaded.size(); ++i)
        ExpectChannelEquals(catalog[i], loaded[i]);

    sqlite3 *database = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &database), SQLITE_OK);
    const int result = sqlite3_exec(
        database, "ALTER TABLE channels DROP COLUMN media_kind; PRAGMA user_version=5;", nullptr,
        nullptr, nullptr);
    sqlite3_close(database);
    ASSERT_EQ(result, SQLITE_OK);
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded, {}, &report), iptv::StoreStatus::ok);
    EXPECT_EQ(report.catalog_version, 5u);
    ASSERT_EQ(loaded.size(), 3u);
    for (const auto channel : loaded)
        EXPECT_EQ(channel.media_kind, iptv::MediaKind::live);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), loaded), iptv::StoreStatus::ok);
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded, {}, &report), iptv::StoreStatus::ok);
    EXPECT_EQ(report.catalog_version, 7u);
}

TEST_F(IptvStoreTest, InspectsASavedCatalogWithoutReadingItsRows)
{
    iptv::StoreReport report;
    std::uint64_t source = 1;
    EXPECT_EQ(iptv::InspectCatalog(path_.string(), &report, &source), iptv::StoreStatus::not_found);
    EXPECT_EQ(source, 0u);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), MakeCatalog(5)), iptv::StoreStatus::ok);
    ASSERT_EQ(iptv::InspectCatalog(path_.string(), &report, &source), iptv::StoreStatus::ok);
    EXPECT_EQ(report.records, 5u);
    EXPECT_EQ(report.catalog_version, 7u);
    EXPECT_GT(report.saved_unix, 0u);
    EXPECT_EQ(source, kSourceId);
    EXPECT_EQ(iptv::InspectCatalog("", &report), iptv::StoreStatus::invalid_argument);
}

TEST_F(IptvStoreTest, ReadsVersionSixWithoutInventingProviderNumbers)
{
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), MakeCatalog(1)), iptv::StoreStatus::ok);
    sqlite3 *database = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &database), SQLITE_OK);
    const int result = sqlite3_exec(
        database, "ALTER TABLE channels DROP COLUMN channel_number; PRAGMA user_version=6;",
        nullptr, nullptr, nullptr);
    sqlite3_close(database);
    ASSERT_EQ(result, SQLITE_OK);
    iptv::Catalog loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_TRUE(loaded[0].channel_number.empty());
}

TEST_F(IptvStoreTest, RejectsInvalidStoredMediaKind)
{
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), MakeCatalog(1)), iptv::StoreStatus::ok);
    sqlite3 *database = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &database), SQLITE_OK);
    const int result = sqlite3_exec(
        database, "PRAGMA ignore_check_constraints=ON; UPDATE channels SET media_kind=4294967296;",
        nullptr, nullptr, nullptr);
    sqlite3_close(database);
    ASSERT_EQ(result, SQLITE_OK);
    iptv::Catalog loaded;
    EXPECT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::corrupt);
}

TEST_F(IptvStoreTest, RoundTripsEveryChannelFieldAlternatesAndHttpHeaders)
{
    const iptv::Catalog expected = MakeCatalog(2);
    iptv::StoreReport save_report;

    ASSERT_EQ(iptv::SaveCatalog(path_.string(), expected, {}, &save_report), iptv::StoreStatus::ok);
    EXPECT_EQ(save_report.status, iptv::StoreStatus::ok);
    EXPECT_EQ(save_report.records, expected.size());
    EXPECT_EQ(save_report.bytes, fs::file_size(path_));

    iptv::Catalog actual;
    iptv::StoreReport load_report;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &actual, {}, &load_report), iptv::StoreStatus::ok);
    EXPECT_EQ(actual.source_id, expected.source_id);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
        ExpectChannelEquals(expected[index], actual[index]);
    EXPECT_EQ(load_report.status, iptv::StoreStatus::ok);
    EXPECT_EQ(load_report.records, actual.size());
    EXPECT_EQ(load_report.bytes, save_report.bytes);
    EXPECT_GT(load_report.saved_unix, 0u);
}

TEST_F(IptvStoreTest, RestoresAValidBackupWhenPrimaryIsCorrupt)
{
    const iptv::Catalog expected = MakeCatalog(2, 55);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), expected), iptv::StoreStatus::ok);
    ASSERT_TRUE(fs::copy_file(path_, BackupPath()));
    WriteBytes(path_, "not a sqlite database");

    iptv::Catalog recovered;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &recovered), iptv::StoreStatus::ok);
    EXPECT_EQ(recovered.source_id, expected.source_id);
    EXPECT_EQ(recovered.size(), expected.size());
    EXPECT_TRUE(fs::is_regular_file(path_));
    EXPECT_FALSE(fs::exists(BackupPath()));
}

TEST_F(IptvStoreTest, SuccessfulReplacementCleansStagingAndBackupFiles)
{
    const iptv::Catalog first = MakeCatalog(1, 11);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), first), iptv::StoreStatus::ok);

    WriteBytes(StagingPath(), "stale staging");
    WriteBytes(BackupPath(), "stale backup");
    const iptv::Catalog replacement = MakeCatalog(3, 22);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), replacement), iptv::StoreStatus::ok);

    EXPECT_TRUE(fs::is_regular_file(path_));
    EXPECT_FALSE(fs::exists(StagingPath()));
    EXPECT_FALSE(fs::exists(BackupPath()));

    iptv::Catalog loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.source_id, replacement.source_id);
    ASSERT_EQ(loaded.size(), replacement.size());
    ExpectChannelEquals(replacement.back(), loaded.back());
}

TEST_F(IptvStoreTest, FailedSaveRemovesStagingAndPreservesLastGoodPrimary)
{
    const iptv::Catalog good = MakeCatalog(1);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), good), iptv::StoreStatus::ok);
    WriteBytes(StagingPath(), "stale staging");

    iptv::StoreLimits limits;
    limits.max_file_bytes = 1u;
    iptv::StoreReport report;
    EXPECT_EQ(iptv::SaveCatalog(path_.string(), MakeCatalog(2, 22), limits, &report),
              iptv::StoreStatus::too_large);
    EXPECT_EQ(report.status, iptv::StoreStatus::too_large);
    EXPECT_FALSE(fs::exists(StagingPath()));

    iptv::Catalog loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    ASSERT_EQ(loaded.size(), 1u);
    ExpectChannelEquals(good.front(), loaded.front());
}

TEST_F(IptvStoreTest, RejectsCorruptDatabaseWithoutMutatingOutputAndSaveRecovers)
{
    WriteBytes(path_, "this is not a sqlite database");
    iptv::Catalog output = MakeCatalog(1, 77);
    const iptv::Catalog sentinel = output;
    iptv::StoreReport corrupt_report;

    EXPECT_EQ(iptv::LoadCatalog(path_.string(), &output, {}, &corrupt_report),
              iptv::StoreStatus::corrupt);
    EXPECT_EQ(corrupt_report.status, iptv::StoreStatus::corrupt);
    EXPECT_EQ(corrupt_report.records, 0u);
    ASSERT_EQ(output.size(), sentinel.size());
    EXPECT_EQ(output.source_id, sentinel.source_id);
    ExpectChannelEquals(sentinel.front(), output.front());

    const iptv::Catalog recovered = MakeCatalog(2, 88);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), recovered), iptv::StoreStatus::ok);
    EXPECT_FALSE(fs::exists(StagingPath()));
    EXPECT_FALSE(fs::exists(BackupPath()));

    iptv::Catalog loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.source_id, recovered.source_id);
    ASSERT_EQ(loaded.size(), recovered.size());
    ExpectChannelEquals(recovered.front(), loaded.front());
}

TEST_F(IptvStoreTest, EnforcesSaveAndLoadFileSizeLimits)
{
    const iptv::Catalog original = MakeCatalog(2);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), original), iptv::StoreStatus::ok);
    const std::uintmax_t original_size = fs::file_size(path_);

    iptv::StoreLimits load_limits;
    load_limits.max_file_bytes = static_cast<std::size_t>(original_size - 1u);
    iptv::Catalog output = MakeCatalog(1, 999);
    iptv::StoreReport load_report;
    EXPECT_EQ(iptv::LoadCatalog(path_.string(), &output, load_limits, &load_report),
              iptv::StoreStatus::too_large);
    EXPECT_EQ(load_report.bytes, original_size);
    EXPECT_EQ(output.source_id, 999u);

    iptv::StoreLimits save_limits;
    save_limits.max_file_bytes = 1u;
    iptv::StoreReport save_report;
    EXPECT_EQ(iptv::SaveCatalog(path_.string(), MakeCatalog(3, 333), save_limits, &save_report),
              iptv::StoreStatus::too_large);
    EXPECT_GT(save_report.bytes, save_limits.max_file_bytes);
    EXPECT_FALSE(fs::exists(StagingPath()));

    iptv::Catalog loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.source_id, original.source_id);
    EXPECT_EQ(loaded.size(), original.size());
}

TEST_F(IptvStoreTest, EnforcesChannelLimitsOnSaveAndLoad)
{
    const iptv::Catalog two_channels = MakeCatalog(2);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), two_channels), iptv::StoreStatus::ok);

    iptv::StoreLimits limits;
    limits.max_channels = 1u;
    iptv::StoreReport save_report;
    EXPECT_EQ(iptv::SaveCatalog(path_.string(), two_channels, limits, &save_report),
              iptv::StoreStatus::invalid_argument);
    EXPECT_EQ(save_report.status, iptv::StoreStatus::invalid_argument);

    iptv::Catalog output = MakeCatalog(1, 444);
    iptv::StoreReport load_report;
    EXPECT_EQ(iptv::LoadCatalog(path_.string(), &output, limits, &load_report),
              iptv::StoreStatus::corrupt);
    EXPECT_EQ(load_report.status, iptv::StoreStatus::corrupt);
    EXPECT_EQ(output.source_id, 444u);
}

TEST_F(IptvStoreTest, RoundTripsRepresentativeMultiThousandChannelCache)
{
    constexpr std::size_t kChannelCount = 4096u;
    const iptv::Catalog expected = MakeCatalog(kChannelCount);
    iptv::StoreReport save_report;
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), expected, {}, &save_report), iptv::StoreStatus::ok);
    EXPECT_EQ(save_report.records, kChannelCount);

    iptv::Catalog actual;
    iptv::StoreReport load_report;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &actual, {}, &load_report), iptv::StoreStatus::ok);
    EXPECT_EQ(load_report.records, kChannelCount);
    ASSERT_EQ(actual.size(), kChannelCount);
    ExpectChannelEquals(expected.front(), actual.front());
    ExpectChannelEquals(expected[kChannelCount / 2u], actual[kChannelCount / 2u]);
    ExpectChannelEquals(expected.back(), actual.back());
}

TEST_F(IptvStoreTest, PersistsPlaybackResultsAcrossCatalogRefreshesAndSources)
{
    ASSERT_EQ(iptv::RecordPlaybackResult(history_path_.string(), kSourceId, "channel-0", true, 0),
              iptv::StoreStatus::ok);
    ASSERT_EQ(iptv::RecordPlaybackResult(history_path_.string(), kSourceId, "channel-1", false, -6),
              iptv::StoreStatus::ok);

    iptv::Catalog refreshed = MakeCatalog(3);
    ASSERT_EQ(iptv::LoadPlaybackResults(history_path_.string(), kSourceId, &refreshed),
              iptv::StoreStatus::ok);
    EXPECT_EQ(refreshed[0].playback_status, iptv::PlaybackStatus::playable);
    EXPECT_EQ(refreshed[0].playback_result, 0);
    EXPECT_GT(refreshed[0].playback_checked_unix, 0u);
    EXPECT_EQ(refreshed[1].playback_status, iptv::PlaybackStatus::failed);
    EXPECT_EQ(refreshed[1].playback_result, -6);
    EXPECT_GT(refreshed[1].playback_checked_unix, 0u);
    EXPECT_EQ(refreshed[2].playback_status, iptv::PlaybackStatus::unknown);

    iptv::Catalog other_source = MakeCatalog(1, 99);
    ASSERT_EQ(iptv::LoadPlaybackResults(history_path_.string(), 99, &other_source),
              iptv::StoreStatus::ok);
    EXPECT_EQ(other_source[0].playback_status, iptv::PlaybackStatus::unknown);
}
TEST_F(IptvStoreTest, RoundTripsMoreThanAHundredThousandChannels)
{
    constexpr std::size_t kChannelCount = 120000u;
    iptv::Catalog expected;
    expected.source_id = kSourceId;
    for (std::size_t index = 0; index < kChannelCount; ++index)
    {
        iptv::Channel channel = MakeChannel(index);
        // One channel in ten has other addresses, as in a real list.
        if (index % 10u != 0)
        {
            channel.alternate_urls.clear();
            channel.alternate_group_titles.clear();
        }
        ASSERT_TRUE(expected.Add(channel));
    }
    iptv::StoreReport save_report;
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), expected, {}, &save_report), iptv::StoreStatus::ok);
    EXPECT_EQ(save_report.records, kChannelCount);
    // The copy on the console is about the size of what it holds.
    EXPECT_LT(save_report.bytes / kChannelCount, 420u);

    iptv::Catalog actual;
    iptv::StoreReport load_report;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &actual, {}, &load_report), iptv::StoreStatus::ok);
    ASSERT_EQ(actual.size(), kChannelCount);
    for (const std::size_t index :
         {std::size_t{0}, std::size_t{9}, std::size_t{10}, kChannelCount / 2u, kChannelCount - 1u})
        ExpectChannelEquals(expected[index], actual[index]);
    EXPECT_EQ(actual.Find("channel-119999"), kChannelCount - 1u);
}

TEST_F(IptvStoreTest, AChannelThatCannotBeStoredFailsTheSaveAndKeepsTheOldCopy)
{
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), MakeCatalog(2)), iptv::StoreStatus::ok);
    iptv::Catalog broken = MakeCatalog(1, 31);
    iptv::Channel nameless = MakeChannel(1, 31);
    nameless.name.clear();
    ASSERT_TRUE(broken.Add(nameless));
    iptv::StoreReport report;
    EXPECT_EQ(iptv::SaveCatalog(path_.string(), broken, {}, &report),
              iptv::StoreStatus::invalid_argument);
    EXPECT_EQ(report.records, 0u);
    EXPECT_FALSE(fs::exists(StagingPath()));

    iptv::Catalog loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.source_id, kSourceId);
    EXPECT_EQ(loaded.size(), 2u);
}

// The copy an earlier release left on the console: every table keyed by the
// channel's id, a source id on every row, and indexes nothing read.
void WriteVersionOneFile(const fs::path &path)
{
    sqlite3 *database = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &database), SQLITE_OK);
    const char *sql =
        "CREATE TABLE metadata(key TEXT PRIMARY KEY NOT NULL,value INTEGER NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE channels("
        "id TEXT PRIMARY KEY NOT NULL,source_id INTEGER NOT NULL,position INTEGER NOT NULL,"
        "source_line INTEGER NOT NULL,name TEXT NOT NULL,url TEXT NOT NULL,tvg_id TEXT NOT NULL,"
        "tvg_name TEXT NOT NULL,tvg_logo TEXT NOT NULL,group_title TEXT NOT NULL,"
        "tvg_country TEXT NOT NULL,tvg_language TEXT NOT NULL,user_agent TEXT NOT NULL,"
        "referrer TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE UNIQUE INDEX channels_position ON channels(position);"
        "CREATE INDEX channels_name ON channels(name COLLATE NOCASE);"
        "CREATE TABLE alternate_urls(channel_id TEXT NOT NULL,position INTEGER NOT NULL,"
        "url TEXT NOT NULL,PRIMARY KEY(channel_id,position)) WITHOUT ROWID;"
        "CREATE TABLE alternate_groups(channel_id TEXT NOT NULL,position INTEGER NOT NULL,"
        "value TEXT NOT NULL,PRIMARY KEY(channel_id,position)) WITHOUT ROWID;"
        "PRAGMA user_version=1;"
        "INSERT INTO metadata VALUES('source_id',77),('saved_unix',1700000000);"
        // Written out of order: the position says where each one goes.
        "INSERT INTO channels VALUES('zeta',77,1,4,'Zeta','https://z.example/live','z.id','Zeta',"
        "'','Sports','US','English','','');"
        "INSERT INTO channels VALUES('alpha',77,0,2,'Alpha','https://a.example/live','a.id',"
        "'Alpha "
        "TV','https://logos.example/a.png','News','GB','English','Agent/1','https://r.example');"
        "INSERT INTO alternate_urls VALUES('alpha',1,'https://a2.example/live'),"
        "('alpha',0,'https://a1.example/live');"
        "INSERT INTO alternate_groups VALUES('zeta',0,'Local');";
    char *message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    EXPECT_EQ(result, SQLITE_OK) << (message != nullptr ? message : "");
    sqlite3_free(message);
    sqlite3_close(database);
}

TEST_F(IptvStoreTest, ReadsTheCopyAnEarlierReleaseSaved)
{
    WriteVersionOneFile(path_);
    iptv::Catalog loaded;
    iptv::StoreReport report;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded, {}, &report), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.source_id, 77u);
    EXPECT_EQ(report.saved_unix, 1700000000u);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].id, "alpha");
    EXPECT_EQ(loaded[0].tvg_name, "Alpha TV");
    EXPECT_EQ(loaded[0].tvg_logo, "https://logos.example/a.png");
    EXPECT_EQ(loaded[0].http_user_agent, "Agent/1");
    EXPECT_EQ(loaded[0].http_referrer, "https://r.example");
    EXPECT_EQ(loaded[0].source_line, 2u);
    ASSERT_EQ(loaded[0].alternate_urls.size(), 2u);
    EXPECT_EQ(loaded[0].alternate_urls[0], "https://a1.example/live");
    EXPECT_EQ(loaded[0].alternate_urls[1], "https://a2.example/live");
    EXPECT_EQ(loaded[1].id, "zeta");
    ASSERT_EQ(loaded[1].alternate_group_titles.size(), 1u);
    EXPECT_EQ(loaded[1].alternate_group_titles[0], "Local");

    // Saved again, it is in today's form and reads the same.
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), loaded), iptv::StoreStatus::ok);
    iptv::Catalog again;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &again), iptv::StoreStatus::ok);
    ASSERT_EQ(again.size(), 2u);
    ExpectChannelEquals(loaded[0], again[0]);
    ExpectChannelEquals(loaded[1], again[1]);
}

} // namespace
