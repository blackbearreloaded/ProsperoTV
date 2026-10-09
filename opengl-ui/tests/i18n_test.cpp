// ProsperoTV - Console-language mapping and stable translation fallback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/i18n.hpp"
#include <gtest/gtest.h>
#include <array>
#include <set>
#include <string>
#include <string_view>

namespace
{
struct Translation
{
    const char *english;
    std::array<const char *, 6> texts;
};
const Translation translations[] = {
#include "tv/translations.inc"
};

std::string placeholders(std::string_view text)
{
    std::string result;
    for (std::size_t pos = 0; pos < text.size(); ++pos)
        if (text[pos] == '%')
        {
            result += '%';
            if (++pos < text.size())
                result += text[pos];
        }
    return result;
}

TEST(InterfaceLanguage, RegionalVariantsAndUnknownValues)
{
    for (const int id : {3, 20})
    {
        ptv::set_console_language(id);
        EXPECT_STREQ(ptv::interface_language(), "es");
        EXPECT_STREQ(ptv::tr("Settings"), "Ajustes");
    }
    for (const int id : {7, 17})
    {
        ptv::set_console_language(id);
        EXPECT_STREQ(ptv::interface_language(), "pt");
    }
    for (const int id : {2, 22})
    {
        ptv::set_console_language(id);
        EXPECT_STREQ(ptv::interface_language(), "fr");
    }
    for (const int id : {-1, 1, 18, 999})
    {
        ptv::set_console_language(id);
        EXPECT_STREQ(ptv::interface_language(), "en");
        EXPECT_STREQ(ptv::tr("Settings"), "Settings");
    }
    EXPECT_STREQ(ptv::tr(nullptr), "");
}

TEST(InterfaceLanguage, EveryEntryHasAllTranslationsAndUnknownTextSurvives)
{
    std::set<std::string_view> keys;
    for (const auto &entry : translations)
        EXPECT_TRUE(keys.emplace(entry.english).second) << entry.english;
    const int languages[] = {3, 2, 4, 5, 7, 6};
    for (unsigned language = 0; language < std::size(languages); ++language)
    {
        ptv::set_console_language(languages[language]);
        for (const auto &entry : translations)
        {
            const auto text = entry.texts[language];
            EXPECT_TRUE(text && *text);
            if (text)
            {
                EXPECT_STREQ(ptv::tr(entry.english), text);
                EXPECT_EQ(placeholders(entry.english), placeholders(text)) << entry.english;
            }
        }
        EXPECT_STREQ(ptv::tr("Provider Channel 1"), "Provider Channel 1");
    }
    ptv::set_console_language(1);
}
} // namespace
