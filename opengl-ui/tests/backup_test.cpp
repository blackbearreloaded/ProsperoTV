// ProsperoTV - Portable backups, interrupted restore and diagnostic privacy.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/backup.hpp"
#include "tv/library.hpp"
#include "tv/settings.hpp"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
class BackupTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        char pattern[] = "/tmp/prosperotv-backup-XXXXXX";
        root = mkdtemp(pattern);
        config = root + "/config";
        drive = root + "/usb0";
        std::filesystem::create_directory(config);
        std::filesystem::create_directory(drive);
        archive = drive + "/ProsperoTV-backup.sqlite3";
    }
    void TearDown() override
    {
        std::filesystem::remove_all(root);
    }
    static std::string read(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), {}};
    }
    void edit_archive(const char *sql)
    {
        sqlite3 *db = nullptr;
        ASSERT_EQ(sqlite3_open(archive.c_str(), &db), SQLITE_OK);
        EXPECT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK);
        EXPECT_EQ(sqlite3_close(db), SQLITE_OK);
    }
    std::string root, config, drive, archive, error;
};

TEST_F(BackupTest, RestoresSourcesFoldersSettingsAndBinaryStateWithoutPairingOrCache)
{
    ptv::Settings settings;
    settings.volume = 35;
    settings.hide_failed = true;
    ASSERT_TRUE(ptv::save_settings(config, settings));
    {
        ptv::Library library;
        ASSERT_TRUE(library.open(config + "/prosperotv-library.sqlite3"));
        ptv::SavedSource source{0,      2,        "Family TV", "https://example.invalid",
                                "user", "secret", {},          ptv::RefreshSchedule::weekly};
        ASSERT_TRUE(library.save_source(&source));
        ASSERT_TRUE(library.select_source(source.id));
        ASSERT_TRUE(library.add_folder("Kids"));
        ASSERT_TRUE(library.put_in_folder("Kids", "channel-2", true));
    }
    const std::string binary("\x01\x00\x02", 3);
    std::ofstream(config + "/iptv-favorites-v1.bin", std::ios::binary)
        .write(binary.data(), binary.size());
    std::ofstream(config + "/phone-pairing-v1.txt") << "private-phone-token";
    std::ofstream(config + "/catalog.sqlite3") << "downloaded catalogue";
    ASSERT_TRUE(ptv::backup_settings(config, archive, error)) << error;
    const auto bytes = read(archive);
    EXPECT_EQ(bytes.find("private-phone-token"), std::string::npos);
    EXPECT_EQ(bytes.find("downloaded catalogue"), std::string::npos);
    settings.volume = 90;
    ASSERT_TRUE(ptv::save_settings(config, settings));
    std::filesystem::remove(config + "/prosperotv-library.sqlite3");
    std::filesystem::remove(config + "/iptv-favorites-v1.bin");
    std::ofstream(config + "/iptv-custom-source-v1.txt") << "added after backup";
    ASSERT_TRUE(ptv::restore_settings(config, archive, error)) << error;
    EXPECT_EQ(ptv::load_settings(config).volume, 35);
    EXPECT_EQ(read(config + "/iptv-favorites-v1.bin"), binary);
    EXPECT_FALSE(std::filesystem::exists(config + "/iptv-custom-source-v1.txt"));
    EXPECT_EQ(read(config + "/phone-pairing-v1.txt"), "private-phone-token");
    ptv::Library library;
    ASSERT_TRUE(library.open(config + "/prosperotv-library.sqlite3"));
    ASSERT_EQ(library.sources().size(), 1u);
    EXPECT_EQ(library.sources()[0].password, "secret");
    EXPECT_EQ(library.sources()[0].schedule, ptv::RefreshSchedule::weekly);
    EXPECT_EQ(library.folder_channels("Kids").count("channel-2"), 1u);
}

TEST_F(BackupTest, RejectsIncompleteOrChangedArchivesBeforeAlteringSettings)
{
    const auto settings = config + "/prosperotv-interface-v1.txt";
    std::ofstream(settings) << "original settings";
    for (const auto *sql :
         {"UPDATE files SET crc=crc+1 WHERE name='prosperotv-interface-v1.txt'",
          "DELETE FROM files WHERE name='iptv-history-v1.bin'",
          "UPDATE files SET name='unexpected.txt' WHERE name='iptv-history-v1.bin'",
          "UPDATE files SET present='invalid' WHERE name='iptv-history-v1.bin'"})
    {
        ASSERT_TRUE(ptv::backup_settings(config, archive, error));
        edit_archive(sql);
        std::ofstream(settings) << "current settings";
        EXPECT_FALSE(ptv::restore_settings(config, archive, error));
        EXPECT_EQ(read(settings), "current settings");
        EXPECT_FALSE(std::filesystem::exists(config + "/prosperotv-restore-journal.sqlite3"));
    }
}

TEST_F(BackupTest, RecoversInterruptedRestoreAndRefusesLinkedDestinations)
{
    const auto settings = config + "/prosperotv-interface-v1.txt";
    const auto journal = config + "/prosperotv-restore-journal.sqlite3";
    std::ofstream(settings) << "before restore";
    ASSERT_TRUE(ptv::backup_settings(config, journal, error));
    std::ofstream(settings) << "half restored";
    std::ofstream(config + "/iptv-history-v1.bin") << "partial history";
    ASSERT_TRUE(ptv::recover_settings(config, error)) << error;
    EXPECT_EQ(read(settings), "before restore");
    EXPECT_FALSE(std::filesystem::exists(config + "/iptv-history-v1.bin"));
    EXPECT_FALSE(std::filesystem::exists(journal));
    ASSERT_TRUE(ptv::backup_settings(config, archive, error));
    std::filesystem::remove(settings);
    const auto unrelated = root + "/unrelated.txt";
    std::ofstream(unrelated) << "leave alone";
    std::filesystem::create_symlink(unrelated, settings);
    EXPECT_FALSE(ptv::restore_settings(config, archive, error));
    EXPECT_EQ(read(unrelated), "leave alone");
    EXPECT_FALSE(ptv::backup_settings(config, archive, error));
}

TEST_F(BackupTest, ReportsKeepUsefulNumericEvidenceAndExcludeProviderDetails)
{
    auto report = ptv::failure_report(
        "stream_state=7\ncoded=1920x1080\nvisible=bad\nresult=999\n"
        "native_result=-125\ndecoder_output_error=0x123a\ncodec=deadbeef\n"
        "channel=PRIVATE\nurl=https://provider.invalid/user/password\nplayback_error=SECRET\n",
        "01.000.030", 123456, -4, 3);
    EXPECT_NE(report.find("result=-4\n"), std::string::npos);
    EXPECT_NE(report.find("native_result=-125\n"), std::string::npos);
    EXPECT_NE(report.find("coded=1920x1080\n"), std::string::npos);
    EXPECT_EQ(report.find("999"), std::string::npos);
    EXPECT_EQ(report.find("deadbeef"), std::string::npos);
    EXPECT_EQ(report.find("PRIVATE"), std::string::npos);
    EXPECT_EQ(report.find("SECRET"), std::string::npos);
    EXPECT_EQ(report.find("provider"), std::string::npos);
    EXPECT_FALSE(ptv::export_failure_report(config, drive + "/report.txt", error));
    std::ofstream(config + "/prosperotv-last-failure.txt") << report << "url=SECRET\n";
    ASSERT_TRUE(ptv::export_failure_report(config, drive + "/report.txt", error));
    EXPECT_EQ(read(drive + "/report.txt"), report);
}

TEST_F(BackupTest, FindsMultipleUsbMountsAndSkipsLinks)
{
    std::filesystem::create_directory(root + "/usb2");
    std::filesystem::create_directory_symlink(drive, root + "/usb1");
    EXPECT_EQ(ptv::usb_drives(root), (std::vector<std::string>{drive, root + "/usb2"}));
}
} // namespace
