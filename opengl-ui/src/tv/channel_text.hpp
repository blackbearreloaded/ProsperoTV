// ProsperoTV - What a channel record says, in the words the screens show.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "iptv_catalog.h"

#include <string>
#include <string_view>
#include <vector>

namespace ptv
{

// Picture sizes a playlist can name: the index is what the quality filter stores.
enum Quality : unsigned
{
    kQualityAny = 0,
    kQualitySd = 1,
    kQualityHd = 2,
    kQualityFullHd = 3,
    kQualityUhd = 4,
    kQualityCount = 5,
};

// Provider text with its decorative letters as plain ones: raised and lowered
// letters and digits, small capitals and full-width forms become the letters
// they stand for, as capitals ("ᵁᴴᴰ ⁶⁰ᶠᵖˢ" reads "UHD 60FPS"). Everything else is kept.
std::string plain_text(std::string_view text);
// plain_text() for a label a list writes: signs and pictures set around the
// words ("RELAX ☼") are left out too, and the spaces closed up.
std::string label_text(std::string_view text);
bool contains_nocase(std::string_view text, std::string_view needle);
bool equals_nocase(std::string_view left, std::string_view right);
// True when a list field ("US, CA; MX") holds the value as one of its entries.
bool field_has_value(std::string_view field, std::string_view value);
// The first entry of a list field, trimmed.
std::string first_value(std::string_view field);

// The size a channel's name or address announces, or kQualityAny.
unsigned quality_of(const iptv::ChannelView &channel);
// "Any quality", "SD", "720p", "1080p", "4K": the filter's own words.
const char *quality_filter_name(unsigned quality);
// The exact size the record names ("1080p", "576p"), or "" when it names none.
std::string resolution_label(const iptv::ChannelView &channel);
// The codec the record names ("HEVC", "H.264", "VP9"), or "".
const char *codec_label(const iptv::ChannelView &channel);

// A channel's name as it reads on screen: the playlist's own notes are taken
// out of it ("Name (1080p) [Not 24/7]" is "Name") and returned in `notes`
// ("Not 24/7") when the caller wants them.
std::string display_name(const iptv::ChannelView &channel, std::vector<std::string> *notes = nullptr);
// Two capitals for a channel that has no picture.
std::string monogram(const iptv::ChannelView &channel);
// "Movies", or "Uncategorized".
std::string category_of(const iptv::ChannelView &channel);
// "World", or the country, then the language when the record has one.
std::string place_line(const iptv::ChannelView &channel);

// The lists are in the order of the alphabet, under 27 letters: '#' (names
// that start with a digit or in another script), then A to Z.
inline constexpr int kLetterCount = 27;
// What a channel is sorted by: its letter, then its display name in capitals
// with accents put aside and punctuation passed over.
std::string sort_key(const iptv::ChannelView &channel);
// 0 for '#', 1 to 26 for A to Z.
int letter_of_key(std::string_view key);
char letter_char(int letter);

// 12886 as "12,886".
std::string group_digits(unsigned value);

} // namespace ptv
