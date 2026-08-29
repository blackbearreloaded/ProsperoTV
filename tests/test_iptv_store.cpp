/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_store.h"

#include <gtest/gtest.h>

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
    channel.tvg_name = "Guide name " + std::to_string(index);
    channel.tvg_logo = "https://logos.example/" + std::to_string(index) + ".png";
    channel.group_title = "News";
    channel.alternate_group_titles = {"Local", "International"};
    channel.tvg_country = "US";
    channel.tvg_language = "English";
    channel.http_user_agent = "psiptv-test/1.0 (channel " + std::to_string(index) + ")";
    channel.http_referrer = "https://portal.example/watch/" + std::to_string(index);
    channel.source_line = static_cast<std::uint32_t>(index * 2u + 2u);
    return channel;
}

iptv::CatalogState MakeCatalog(std::size_t count, std::uint64_t source_id = kSourceId)
{
    iptv::CatalogState catalog;
    catalog.source_id = source_id;
    catalog.channels.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        catalog.channels.push_back(MakeChannel(index, source_id));
    return catalog;
}

void ExpectChannelEquals(const iptv::Channel &expected, const iptv::Channel &actual)
{
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
}

class IptvStoreTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static std::atomic<unsigned long long> sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / ("psiptv-store-test-" + std::to_string(tick) + "-" +
                                             std::to_string(sequence.fetch_add(1)));
        ASSERT_TRUE(fs::create_directories(root_));
        path_ = root_ / "catalog.sqlite3";
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
};

TEST_F(IptvStoreTest, RoundTripsEveryChannelFieldAlternatesAndHttpHeaders)
{
    const iptv::CatalogState expected = MakeCatalog(2);
    iptv::StoreReport save_report;

    ASSERT_EQ(iptv::SaveCatalog(path_.string(), expected, {}, &save_report), iptv::StoreStatus::ok);
    EXPECT_EQ(save_report.status, iptv::StoreStatus::ok);
    EXPECT_EQ(save_report.records, expected.channels.size());
    EXPECT_EQ(save_report.bytes, fs::file_size(path_));

    iptv::CatalogState actual;
    iptv::StoreReport load_report;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &actual, {}, &load_report), iptv::StoreStatus::ok);
    EXPECT_EQ(actual.source_id, expected.source_id);
    ASSERT_EQ(actual.channels.size(), expected.channels.size());
    for (std::size_t index = 0; index < expected.channels.size(); ++index)
        ExpectChannelEquals(expected.channels[index], actual.channels[index]);
    EXPECT_EQ(load_report.status, iptv::StoreStatus::ok);
    EXPECT_EQ(load_report.records, actual.channels.size());
    EXPECT_EQ(load_report.bytes, save_report.bytes);
    EXPECT_GT(load_report.saved_unix, 0u);
}

TEST_F(IptvStoreTest, RestoresAValidBackupWhenPrimaryIsCorrupt)
{
    const iptv::CatalogState expected = MakeCatalog(2, 55);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), expected), iptv::StoreStatus::ok);
    ASSERT_TRUE(fs::copy_file(path_, BackupPath()));
    WriteBytes(path_, "not a sqlite database");

    iptv::CatalogState recovered;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &recovered), iptv::StoreStatus::ok);
    EXPECT_EQ(recovered.source_id, expected.source_id);
    EXPECT_EQ(recovered.channels.size(), expected.channels.size());
    EXPECT_TRUE(fs::is_regular_file(path_));
    EXPECT_FALSE(fs::exists(BackupPath()));
}

TEST_F(IptvStoreTest, SuccessfulReplacementCleansStagingAndBackupFiles)
{
    const iptv::CatalogState first = MakeCatalog(1, 11);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), first), iptv::StoreStatus::ok);

    WriteBytes(StagingPath(), "stale staging");
    WriteBytes(BackupPath(), "stale backup");
    const iptv::CatalogState replacement = MakeCatalog(3, 22);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), replacement), iptv::StoreStatus::ok);

    EXPECT_TRUE(fs::is_regular_file(path_));
    EXPECT_FALSE(fs::exists(StagingPath()));
    EXPECT_FALSE(fs::exists(BackupPath()));

    iptv::CatalogState loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.source_id, replacement.source_id);
    ASSERT_EQ(loaded.channels.size(), replacement.channels.size());
    ExpectChannelEquals(replacement.channels.back(), loaded.channels.back());
}

TEST_F(IptvStoreTest, FailedSaveRemovesStagingAndPreservesLastGoodPrimary)
{
    const iptv::CatalogState good = MakeCatalog(1);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), good), iptv::StoreStatus::ok);
    WriteBytes(StagingPath(), "stale staging");

    iptv::StoreLimits limits;
    limits.max_file_bytes = 1u;
    iptv::StoreReport report;
    EXPECT_EQ(iptv::SaveCatalog(path_.string(), MakeCatalog(2, 22), limits, &report),
              iptv::StoreStatus::too_large);
    EXPECT_EQ(report.status, iptv::StoreStatus::too_large);
    EXPECT_FALSE(fs::exists(StagingPath()));

    iptv::CatalogState loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    ASSERT_EQ(loaded.channels.size(), 1u);
    ExpectChannelEquals(good.channels.front(), loaded.channels.front());
}

TEST_F(IptvStoreTest, RejectsCorruptDatabaseWithoutMutatingOutputAndSaveRecovers)
{
    WriteBytes(path_, "this is not a sqlite database");
    iptv::CatalogState output = MakeCatalog(1, 77);
    const iptv::CatalogState sentinel = output;
    iptv::StoreReport corrupt_report;

    EXPECT_EQ(iptv::LoadCatalog(path_.string(), &output, {}, &corrupt_report),
              iptv::StoreStatus::corrupt);
    EXPECT_EQ(corrupt_report.status, iptv::StoreStatus::corrupt);
    EXPECT_EQ(corrupt_report.records, 0u);
    ASSERT_EQ(output.channels.size(), sentinel.channels.size());
    EXPECT_EQ(output.source_id, sentinel.source_id);
    ExpectChannelEquals(sentinel.channels.front(), output.channels.front());

    const iptv::CatalogState recovered = MakeCatalog(2, 88);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), recovered), iptv::StoreStatus::ok);
    EXPECT_FALSE(fs::exists(StagingPath()));
    EXPECT_FALSE(fs::exists(BackupPath()));

    iptv::CatalogState loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.source_id, recovered.source_id);
    ASSERT_EQ(loaded.channels.size(), recovered.channels.size());
    ExpectChannelEquals(recovered.channels.front(), loaded.channels.front());
}

TEST_F(IptvStoreTest, EnforcesSaveAndLoadFileSizeLimits)
{
    const iptv::CatalogState original = MakeCatalog(2);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), original), iptv::StoreStatus::ok);
    const std::uintmax_t original_size = fs::file_size(path_);

    iptv::StoreLimits load_limits;
    load_limits.max_file_bytes = static_cast<std::size_t>(original_size - 1u);
    iptv::CatalogState output = MakeCatalog(1, 999);
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

    iptv::CatalogState loaded;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &loaded), iptv::StoreStatus::ok);
    EXPECT_EQ(loaded.source_id, original.source_id);
    EXPECT_EQ(loaded.channels.size(), original.channels.size());
}

TEST_F(IptvStoreTest, EnforcesChannelLimitsOnSaveAndLoad)
{
    const iptv::CatalogState two_channels = MakeCatalog(2);
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), two_channels), iptv::StoreStatus::ok);

    iptv::StoreLimits limits;
    limits.max_channels = 1u;
    iptv::StoreReport save_report;
    EXPECT_EQ(iptv::SaveCatalog(path_.string(), two_channels, limits, &save_report),
              iptv::StoreStatus::invalid_argument);
    EXPECT_EQ(save_report.status, iptv::StoreStatus::invalid_argument);

    iptv::CatalogState output = MakeCatalog(1, 444);
    iptv::StoreReport load_report;
    EXPECT_EQ(iptv::LoadCatalog(path_.string(), &output, limits, &load_report),
              iptv::StoreStatus::corrupt);
    EXPECT_EQ(load_report.status, iptv::StoreStatus::corrupt);
    EXPECT_EQ(output.source_id, 444u);
}

TEST_F(IptvStoreTest, RoundTripsRepresentativeMultiThousandChannelCache)
{
    constexpr std::size_t kChannelCount = 4096u;
    const iptv::CatalogState expected = MakeCatalog(kChannelCount);
    iptv::StoreReport save_report;
    ASSERT_EQ(iptv::SaveCatalog(path_.string(), expected, {}, &save_report), iptv::StoreStatus::ok);
    EXPECT_EQ(save_report.records, kChannelCount);

    iptv::CatalogState actual;
    iptv::StoreReport load_report;
    ASSERT_EQ(iptv::LoadCatalog(path_.string(), &actual, {}, &load_report), iptv::StoreStatus::ok);
    EXPECT_EQ(load_report.records, kChannelCount);
    ASSERT_EQ(actual.channels.size(), kChannelCount);
    ExpectChannelEquals(expected.channels.front(), actual.channels.front());
    ExpectChannelEquals(expected.channels[kChannelCount / 2u], actual.channels[kChannelCount / 2u]);
    ExpectChannelEquals(expected.channels.back(), actual.channels.back());
}
} // namespace
