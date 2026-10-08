// ProsperoTV - Console-user isolation and legacy migration.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/profiles.hpp"
#include "tv/library.hpp"
#include "tv/settings.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace
{
class ProfileTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        char pattern[] = "/tmp/prosperotv-profile-XXXXXX";
        root = mkdtemp(pattern);
        legacy = root + "/config";
        std::filesystem::create_directory(legacy);
    }
    void TearDown() override
    {
        std::filesystem::remove_all(root);
    }
    std::string root, legacy, error;
};

TEST_F(ProfileTest, OnlyFirstUserInheritsSharedAccountsAndOtherUsersStartEmpty)
{
    ptv::Settings settings;
    settings.volume = 35;
    ASSERT_TRUE(ptv::save_settings(legacy, settings));
    {
        ptv::Library library;
        ASSERT_TRUE(library.open(legacy + "/prosperotv-library.sqlite3"));
        ptv::SavedSource source{
            0,  1,  "First user's source",       "https://example.invalid/list", {},
            {}, {}, ptv::RefreshSchedule::manual};
        ASSERT_TRUE(library.save_source(&source));
    }
    std::ofstream(legacy + "/phone-pairing-v1.txt") << "legacy-phone-token";
    ptv::ProfilePaths first, second, reopened;
    ASSERT_TRUE(ptv::prepare_profile(root, legacy, 0x1234, first, error)) << error;
    ASSERT_TRUE(ptv::prepare_profile(root, legacy, 0x5678, second, error)) << error;
    EXPECT_NE(first.config, second.config);
    EXPECT_NE(first.cache, second.cache);
    EXPECT_NE(first.logs, second.logs);
    EXPECT_EQ(ptv::load_settings(first.config).volume, 35);
    EXPECT_EQ(ptv::load_settings(second.config).volume, 100);
    EXPECT_TRUE(std::filesystem::exists(first.config + "/prosperotv-library.sqlite3"));
    EXPECT_FALSE(std::filesystem::exists(second.config + "/prosperotv-library.sqlite3"));
    EXPECT_FALSE(std::filesystem::exists(first.config + "/phone-pairing-v1.txt"));
    settings.volume = 75;
    ASSERT_TRUE(ptv::save_settings(second.config, settings));
    std::ofstream(second.config + "/phone-pairing-v1.txt") << "second user's phone";
    ASSERT_TRUE(ptv::prepare_profile(root, legacy, 0x1234, reopened, error));
    EXPECT_EQ(reopened.config, first.config);
    EXPECT_EQ(ptv::load_settings(reopened.config).volume, 35);
    EXPECT_FALSE(std::filesystem::exists(reopened.config + "/phone-pairing-v1.txt"));
    EXPECT_EQ(ptv::load_settings(second.config).volume, 75);
}

TEST_F(ProfileTest, MigrationResumesWithoutReplacingSettingsAndNeverReimportsAfterCompletion)
{
    std::ofstream(legacy + "/iptv-custom-source-v1.txt") << "old source";
    const auto profile = root + "/profiles/00000012";
    std::filesystem::create_directories(profile + "/config");
    std::ofstream(root + "/profiles/legacy-owner-v1.txt") << "00000012\n";
    const auto source = profile + "/config/iptv-custom-source-v1.txt";
    std::ofstream(source) << "already copied and edited";
    ptv::ProfilePaths paths;
    ASSERT_TRUE(ptv::prepare_profile(root, legacy, 0x12, paths, error));
    std::ifstream input(source);
    std::string text;
    std::getline(input, text);
    EXPECT_EQ(text, "already copied and edited");
    input.close();
    std::filesystem::remove(source);
    ASSERT_TRUE(ptv::prepare_profile(root, legacy, 0x12, paths, error));
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::exists(legacy + "/iptv-custom-source-v1.txt"));
}

TEST_F(ProfileTest, UnknownUsersAndDamagedOwnershipFailWithoutUsingSharedSettings)
{
    ptv::ProfilePaths paths;
    EXPECT_FALSE(ptv::prepare_profile(root, legacy, -1, paths, error));
    EXPECT_TRUE(paths.config.empty());
    EXPECT_FALSE(std::filesystem::exists(root + "/profiles"));
    std::filesystem::create_directory(root + "/profiles");
    std::ofstream(root + "/profiles/legacy-owner-v1.txt") << "incomplete";
    EXPECT_FALSE(ptv::prepare_profile(root, legacy, 55, paths, error));
    EXPECT_TRUE(paths.config.empty());
    EXPECT_FALSE(
        std::filesystem::exists(root + "/profiles/00000037/config/iptv-custom-source-v1.txt"));
}
} // namespace
