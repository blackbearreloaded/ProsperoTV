// ProsperoTV - Shared provider parser and Stalker envelope regressions.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_json.h"
#include <gtest/gtest.h>
#include <vector>

TEST(ProviderJson, ReadsStalkerChannelEnvelopeAtEveryChunkBoundary)
{
    const std::string document =
        R"({"js":{"data":[{"id":"1","name":"A {channel}"},{"id":2,"name":"B \"quoted\""}],"total_items":2},"other":null})";
    for (std::size_t size = 1; size <= document.size(); ++size)
    {
        iptv::json::ListSplitter splitter("js");
        std::vector<std::string> ids;
        for (std::size_t i = 0; i < document.size(); i += size)
            ASSERT_TRUE(splitter.Feed(std::string_view(document).substr(i, size),
                                      [&](std::string_view item)
                                      {
                                          iptv::json::JsonReader reader(item);
                                          return iptv::json::ReadObject(
                                                     &reader,
                                                     [&](const std::string &key, auto *value)
                                                     {
                                                         if (key != "id")
                                                             return value->SkipValue();
                                                         std::string id;
                                                         if (!value->StringOrScalar(&id))
                                                             return false;
                                                         ids.push_back(id);
                                                         return true;
                                                     }) &&
                                                 reader.Finished();
                                      }));
        EXPECT_TRUE(splitter.complete());
        EXPECT_TRUE(splitter.found());
        EXPECT_EQ(ids, (std::vector<std::string>{"1", "2"}));
    }
}

TEST(ProviderJson, RejectsBrokenListsAndInvalidScalarValues)
{
    for (const auto text : {"[{},]", "[{},,{}]", "[{}{}]", "[,{}]", "[{}"})
    {
        iptv::json::ListSplitter splitter;
        EXPECT_FALSE(splitter.Feed(text, [](std::string_view) { return true; }) &&
                     splitter.complete())
            << text;
    }
    for (const auto text : {"undefined", "01", "1.", "1e", "-", "truex", "[false]"})
    {
        iptv::json::JsonReader reader(text);
        std::string result;
        EXPECT_FALSE(reader.Scalar(&result)) << text;
    }
    for (const auto text : {"null", "true", "false", "-1.5e+2", "0", "12"})
    {
        iptv::json::JsonReader reader(text);
        std::string result;
        EXPECT_TRUE(reader.Scalar(&result)) << text;
        EXPECT_TRUE(reader.Finished());
    }
}
