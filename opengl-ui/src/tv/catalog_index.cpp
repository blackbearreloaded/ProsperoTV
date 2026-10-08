// ProsperoTV - What is worked out from a catalog once, when it arrives.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/catalog_index.hpp"

#include "tv/channel_text.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ptv
{

namespace
{

// Counts the values of one kind. A catalog stores a value many channels share
// once, so most channels are recognized by where their text is and never read.
class Tally
{
  public:
    void add(std::string_view field)
    {
        if (field.empty())
            return;
        const auto known = by_text_.find(field.data());
        if (known != by_text_.end())
        {
            if (known->second != kNothing)
                ++entries_[known->second].second;
            return;
        }
        const std::string value = first_value(field).substr(0, 47);
        std::uint32_t entry = kNothing;
        if (!value.empty())
        {
            std::string folded = value;
            for (char &byte : folded)
                if (byte >= 'A' && byte <= 'Z')
                    byte = static_cast<char>(byte + 32);
            const auto found = by_value_.find(folded);
            if (found != by_value_.end())
            {
                entry = found->second;
            }
            else
            {
                entry = static_cast<std::uint32_t>(entries_.size());
                entries_.emplace_back(value, 0u);
                by_value_.emplace(std::move(folded), entry);
            }
            ++entries_[entry].second;
        }
        if (by_text_.size() < kRemembered)
            by_text_.emplace(field.data(), entry);
    }

    // The most common values, at most kFacetMax of them.
    void store(std::array<Facet, CatalogIndex::kFacetMax> *facets, unsigned *count)
    {
        std::sort(entries_.begin(), entries_.end(),
                  [](const Entry &left, const Entry &right)
                  {
                      if (left.second != right.second)
                          return left.second > right.second;
                      return left.first < right.first;
                  });
        *count =
            static_cast<unsigned>(std::min<std::size_t>(entries_.size(), CatalogIndex::kFacetMax));
        for (unsigned index = 0; index < *count; ++index)
        {
            (*facets)[index].value = entries_[index].first;
            (*facets)[index].count = entries_[index].second;
        }
        for (unsigned index = *count; index < CatalogIndex::kFacetMax; ++index)
            (*facets)[index] = {};
    }

  private:
    using Entry = std::pair<std::string, unsigned>;
    static constexpr std::uint32_t kNothing = 0xffffffffu;
    static constexpr std::size_t kRemembered = 65536;

    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::uint32_t> by_value_;
    std::unordered_map<const char *, std::uint32_t> by_text_;
};

bool in_named_group(const iptv::ChannelView &channel, std::string_view term)
{
    if (contains_nocase(channel.group_title, term))
        return true;
    for (const std::string_view alternate : channel.alternate_group_titles)
        if (contains_nocase(alternate, term))
            return true;
    return false;
}

} // namespace

// Every character of these scripts is three bytes of UTF-8.
void note_scripts(std::string_view text, bool *east_asian, bool *korean)
{
    for (std::size_t index = 0; index + 2 < text.size(); ++index)
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead < 0xe3 || lead > 0xef)
            continue;
        const std::uint32_t codepoint =
            (static_cast<std::uint32_t>(lead & 0x0f) << 12) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1]) & 0x3f) << 6) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 2])) & 0x3f);
        if ((codepoint >= 0xac00 && codepoint <= 0xd7a3) ||
            (codepoint >= 0x3130 && codepoint <= 0x318f))
            *korean = true;
        else if ((codepoint >= 0x3040 && codepoint <= 0x30ff) ||
                 (codepoint >= 0x3400 && codepoint <= 0x9fff) ||
                 (codepoint >= 0xf900 && codepoint <= 0xfaff))
            *east_asian = true;
        index += 2;
    }
}

void CatalogIndex::clear()
{
    *this = {};
}

void CatalogIndex::build(const iptv::Catalog &catalog)
{
    clear();
    const std::uint32_t count = static_cast<std::uint32_t>(catalog.size());
    order.resize(count);
    ranks.resize(count);
    letters.resize(count);
    traits.resize(count);

    // What each channel is sorted by, back to back in one piece of memory.
    std::string keys;
    std::vector<std::uint32_t> key_at(static_cast<std::size_t>(count) + 1u);
    keys.reserve(static_cast<std::size_t>(count) * 24u);
    Tally country_tally;
    Tally category_tally;
    Tally language_tally;
    std::map<std::string_view, unsigned> provider_tally;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const iptv::ChannelView channel = catalog[index];
        const std::string key = sort_key(channel);
        key_at[index] = static_cast<std::uint32_t>(keys.size());
        keys += key;
        letters[index] = static_cast<std::uint8_t>(letter_of_key(key));
        order[index] = index;

        std::uint8_t trait = static_cast<std::uint8_t>(quality_of(channel)) & kQualityMask;
        if (in_named_group(channel, "news"))
            trait |= kNews;
        if (in_named_group(channel, "sport"))
            trait |= kSports;
        if (in_named_group(channel, "kid"))
            trait |= kKids;
        traits[index] = trait;

        country_tally.add(channel.tvg_country);
        category_tally.add(channel.group_title);
        if (!channel.group_title.empty())
            ++provider_tally[channel.group_title];
        language_tally.add(channel.tvg_language);
        for (const std::string_view category : channel.alternate_group_titles)
        {
            category_tally.add(category);
            if (!category.empty() && category != channel.group_title)
                ++provider_tally[category];
        }

        if (!east_asian || !korean)
        {
            note_scripts(channel.name, &east_asian, &korean);
            note_scripts(channel.group_title, &east_asian, &korean);
        }
    }
    key_at[count] = static_cast<std::uint32_t>(keys.size());

    // Channels of one name stay in the order the playlist gave them.
    const auto key_of = [&](std::uint32_t index)
    { return std::string_view(keys.data() + key_at[index], key_at[index + 1u] - key_at[index]); };
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t left, std::uint32_t right)
                     { return key_of(left) < key_of(right); });
    for (std::uint32_t rank = 0; rank < count; ++rank)
        ranks[order[rank]] = rank;

    country_tally.store(&countries, &country_count);
    category_tally.store(&categories, &category_count);
    language_tally.store(&languages, &language_count);
    for (const auto &[name, size] : provider_tally)
        provider_categories.push_back({std::string(name), size});
}

} // namespace ptv
