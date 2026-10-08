// ProsperoTV - The diagnostic log: off unless asked for, and what it says when on.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/diag.hpp"
#include "tv/settings.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

std::vector<std::string> g_lines;

void keep(const char *line)
{
    g_lines.emplace_back(line);
}

class DiagTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        g_lines.clear();
        ptv::diag::set_sink(keep);
        ptv::diag::set_enabled(false);
        ptv::diag::set_forced(false);
    }
    void TearDown() override
    {
        ptv::diag::set_enabled(false);
        ptv::diag::set_forced(false);
        ptv::diag::set_sink(nullptr);
    }
};

TEST_F(DiagTest, NothingIsWrittenUntilItIsTurnedOn)
{
    EXPECT_FALSE(ptv::diag::enabled());
    ptv::diag::event("tab %s", "Live TV");
    tv_diag_line("decoder call");
    EXPECT_TRUE(g_lines.empty());
    EXPECT_EQ(tv_diag_enabled(), 0);

    ptv::diag::set_enabled(true);
    EXPECT_EQ(tv_diag_enabled(), 1);
    ptv::diag::event("tab %s", "Live TV");
    tv_diag_line("decoder call 100% done"); // a line from C is text, not a format
    ASSERT_EQ(g_lines.size(), 2u);
    EXPECT_EQ(g_lines[0], "tab Live TV");
    EXPECT_EQ(g_lines[1], "decoder call 100% done");

    ptv::diag::set_enabled(false);
    ptv::diag::event("after");
    EXPECT_EQ(g_lines.size(), 2u);
}

TEST_F(DiagTest, ABuildCanKeepItOnWhateverTheSettingSays)
{
    ptv::diag::set_forced(true);
    ptv::diag::set_enabled(false);
    EXPECT_TRUE(ptv::diag::enabled());
    ptv::diag::event("kept");
    EXPECT_EQ(g_lines.size(), 1u);
}

TEST_F(DiagTest, ALongLineIsCutNotSplit)
{
    ptv::diag::set_enabled(true);
    ptv::diag::event("%s", std::string(2000, 'x').c_str());
    ASSERT_EQ(g_lines.size(), 1u);
    EXPECT_EQ(g_lines[0].size(), 511u);
}

TEST_F(DiagTest, TheSettingIsOffByDefaultAndRemembered)
{
    char pattern[] = "/tmp/prosperotv-diag-XXXXXX";
    const std::string dir = mkdtemp(pattern);
    EXPECT_FALSE(ptv::Settings{}.diagnostics);
    EXPECT_FALSE(ptv::load_settings(dir).diagnostics); // no file yet
    ptv::Settings settings;
    settings.volume = 40;
    ASSERT_TRUE(ptv::save_settings(dir, settings));
    EXPECT_FALSE(ptv::load_settings(dir).diagnostics);
    settings.diagnostics = true;
    ASSERT_TRUE(ptv::save_settings(dir, settings));
    EXPECT_TRUE(ptv::load_settings(dir).diagnostics);
    EXPECT_EQ(ptv::load_settings(dir).volume, 40);
    // A file written before the switch existed leaves it off.
    std::ofstream(dir + "/prosperotv-interface-v1.txt") << "reduced_motion=1\nsounds=1\n";
    EXPECT_FALSE(ptv::load_settings(dir).diagnostics);
    EXPECT_TRUE(ptv::load_settings(dir).reduced_motion);
    std::filesystem::remove_all(dir);
}

} // namespace
