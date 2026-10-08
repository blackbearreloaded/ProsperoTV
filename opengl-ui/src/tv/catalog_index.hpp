// ProsperoTV - What is worked out from a catalog once, when it arrives.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A provider can list a quarter of a million channels. Reading every name to
// put the list in order, to find the sports or to count the countries takes a
// few tenths of a second at that size: too long for a frame, so it is done
// here, once per catalog and on the thread that downloaded it. What the frame
// loop then does with a list of any size is compare bytes.

#pragma once

#include "iptv_catalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptv
{
void note_scripts(std::string_view text, bool *east_asian, bool *korean);

struct Facet
{
    std::string value;
    unsigned count = 0;
};

struct CatalogIndex
{
    static constexpr unsigned kFacetMax = 24;

    // A channel in one byte: its picture size (a Quality) and the lists it is in.
    static constexpr std::uint8_t kQualityMask = 0x07;
    static constexpr std::uint8_t kNews = 0x08;
    static constexpr std::uint8_t kSports = 0x10;
    static constexpr std::uint8_t kKids = 0x20;

    void build(const iptv::Catalog &catalog);
    void clear();
    std::size_t size() const
    {
        return order.size();
    }

    std::vector<std::uint32_t> order;  // catalog indices in the order of the alphabet
    std::vector<std::uint32_t> ranks;  // by catalog index: where it is in `order`
    std::vector<std::uint8_t> letters; // by catalog index: 0 for '#', 1 to 26
    std::vector<std::uint8_t> traits;  // by catalog index
    // The most common values of each kind, at most kFacetMax of them.
    std::array<Facet, kFacetMax> countries;
    std::array<Facet, kFacetMax> categories;
    // Provider names are exact, untruncated and include every category.
    std::vector<Facet> provider_categories;
    std::array<Facet, kFacetMax> languages;
    unsigned country_count = 0;
    unsigned category_count = 0;
    unsigned language_count = 0;
    // The scripts the names and groups are written in beyond the European
    // ones: the faces for them are large, and loaded only when asked for.
    bool east_asian = false;
    bool korean = false;
};

} // namespace ptv
