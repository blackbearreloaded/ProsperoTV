// ProsperoTV - What a channel record says, in the words the screens show.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/channel_text.hpp"

#include <iterator>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>

namespace ptv
{

namespace
{

char lower(char value)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool is_space(char value)
{
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

bool is_separator(char value)
{
    return value == ',' || value == ';' || value == '|';
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    return text;
}

// ASCII capitals as small letters; every other byte as it is.
constexpr std::array<unsigned char, 256> kLower = []
{
    std::array<unsigned char, 256> table{};
    for (unsigned value = 0; value < 256; ++value)
        table[value] = static_cast<unsigned char>(value >= 'A' && value <= 'Z' ? value + 32 : value);
    return table;
}();

// The name, the playlist's own name and the address: where a size or a codec
// is written when it is written anywhere. They are put in small letters once,
// and then searched as many times as there are words to look for: a catalog
// of a quarter of a million channels asks this of every one of them.
class Mentions
{
  public:
    explicit Mentions(const iptv::ChannelView &channel)
    {
        add(channel.name);
        if (channel.tvg_name != channel.name)
            add(channel.tvg_name);
        add(channel.url);
    }

    // `needle` in small letters.
    bool operator()(std::string_view needle) const
    {
        return std::string_view(text_.data(), size_).find(needle) != std::string_view::npos;
    }

  private:
    void add(std::string_view text)
    {
        // A line break keeps a word from matching across two fields.
        if (size_ != 0 && size_ < text_.size())
            text_[size_++] = '\n';
        const std::size_t count = std::min(text.size(), text_.size() - size_);
        for (std::size_t index = 0; index < count; ++index)
            text_[size_ + index] = static_cast<char>(kLower[static_cast<unsigned char>(text[index])]);
        size_ += count;
    }

    // Longer than any name and address a playlist is allowed to give.
    std::array<char, 2 * 4096 + 2 * 2048 + 2> text_;
    std::size_t size_ = 0;
};

// "1080p", "576i", "2160p": digits and one letter, as a playlist note.
bool is_size_note(std::string_view note)
{
    if (note.size() < 4 || note.size() > 6)
        return false;
    const char last = lower(note.back());
    if (last != 'p' && last != 'i')
        return false;
    for (std::size_t i = 0; i + 1 < note.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(note[i])))
            return false;
    return true;
}

} // namespace

namespace
{

// Code points that stand for one plain letter or digit, in code order.
struct Decorative
{
    std::uint32_t code;
    char plain;
};
constexpr Decorative kDecorative[] = {
    {0x00AA, 'a'}, {0x00B2, '2'}, {0x00B3, '3'}, {0x00B9, '1'}, {0x00BA, 'o'}, {0x0262, 'G'},
    {0x026A, 'I'}, {0x0274, 'N'}, {0x0280, 'R'}, {0x028F, 'Y'}, {0x0299, 'B'}, {0x029C, 'H'},
    {0x029F, 'L'}, {0x02B0, 'H'}, {0x02B2, 'J'}, {0x02B3, 'R'}, {0x02B7, 'W'}, {0x02B8, 'Y'},
    {0x02E1, 'L'}, {0x02E2, 'S'}, {0x02E3, 'X'}, {0x1D00, 'A'}, {0x1D04, 'C'}, {0x1D05, 'D'},
    {0x1D07, 'E'}, {0x1D0A, 'J'}, {0x1D0B, 'K'}, {0x1D0D, 'M'}, {0x1D0F, 'O'}, {0x1D18, 'P'},
    {0x1D1B, 'T'}, {0x1D1C, 'U'}, {0x1D20, 'V'}, {0x1D21, 'W'}, {0x1D22, 'Z'}, {0x1D2C, 'A'},
    {0x1D2E, 'B'}, {0x1D30, 'D'}, {0x1D31, 'E'}, {0x1D33, 'G'}, {0x1D34, 'H'}, {0x1D35, 'I'},
    {0x1D36, 'J'}, {0x1D37, 'K'}, {0x1D38, 'L'}, {0x1D39, 'M'}, {0x1D3A, 'N'}, {0x1D3C, 'O'},
    {0x1D3E, 'P'}, {0x1D3F, 'R'}, {0x1D40, 'T'}, {0x1D41, 'U'}, {0x1D42, 'W'}, {0x1D43, 'A'},
    {0x1D47, 'B'}, {0x1D48, 'D'}, {0x1D49, 'E'}, {0x1D4D, 'G'}, {0x1D4F, 'K'}, {0x1D50, 'M'},
    {0x1D52, 'O'}, {0x1D56, 'P'}, {0x1D57, 'T'}, {0x1D58, 'U'}, {0x1D5B, 'V'}, {0x1D62, 'I'},
    {0x1D63, 'R'}, {0x1D64, 'U'}, {0x1D65, 'V'}, {0x1D9C, 'C'}, {0x1DA0, 'F'}, {0x1DA6, 'I'},
    {0x1DBB, 'Z'}, {0x2070, '0'}, {0x2071, 'I'}, {0x2074, '4'}, {0x2075, '5'}, {0x2076, '6'},
    {0x2077, '7'}, {0x2078, '8'}, {0x2079, '9'}, {0x207A, '+'}, {0x207B, '-'}, {0x207F, 'N'},
    {0x2080, '0'}, {0x2081, '1'}, {0x2082, '2'}, {0x2083, '3'}, {0x2084, '4'}, {0x2085, '5'},
    {0x2086, '6'}, {0x2087, '7'}, {0x2088, '8'}, {0x2089, '9'}, {0x2C7D, 'V'}, {0xA730, 'F'},
    {0xA731, 'S'},
};

} // namespace

std::string plain_text(std::string_view text)
{
    std::string plain;
    plain.reserve(text.size());
    for (std::size_t index = 0; index < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80)
        {
            plain.push_back(text[index++]);
            continue;
        }
        const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (length == 1 || index + length > text.size())
        {
            plain.push_back(text[index++]); // not UTF-8: left as it came
            continue;
        }
        std::uint32_t code = lead & (0xFFu >> (length + 1));
        bool whole = true;
        for (std::size_t at = 1; at < length; ++at)
        {
            const unsigned char next = static_cast<unsigned char>(text[index + at]);
            whole = whole && (next & 0xC0) == 0x80;
            code = (code << 6) | (next & 0x3Fu);
        }
        char found = 0;
        if (whole && code >= 0xFF01 && code <= 0xFF5E)
            found = static_cast<char>(code - 0xFEE0); // full-width forms
        else if (whole)
        {
            const auto *end = std::end(kDecorative);
            const auto *at = std::lower_bound(
                std::begin(kDecorative), end, code,
                [](const Decorative &entry, std::uint32_t value) { return entry.code < value; });
            if (at != end && at->code == code)
                found = at->plain;
        }
        if (found != 0)
            plain.push_back(found);
        else
            plain.append(text.substr(index, length));
        index += length;
    }
    return plain;
}

std::string label_text(std::string_view text)
{
    const std::string plain = plain_text(text);
    std::string label;
    label.reserve(plain.size());
    bool space = false;
    for (std::size_t index = 0; index < plain.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(plain[index]);
        const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (index + length > plain.size())
            break;
        std::uint32_t code = lead;
        if (length > 1)
        {
            code = lead & (0xFFu >> (length + 1));
            for (std::size_t at = 1; at < length; ++at)
                code = (code << 6) | (static_cast<unsigned char>(plain[index + at]) & 0x3Fu);
        }
        // Arrows, shapes, signs, dingbats, variation selectors and pictures:
        // decoration around the words, and nothing the faces can write.
        const bool symbol = (code >= 0x2190 && code <= 0x2BFF) || (code >= 0xFE00 && code <= 0xFE0F) ||
                            (code >= 0x1F000 && code <= 0x1FAFF);
        if (symbol || code == ' ')
            space = !label.empty();
        else
        {
            if (space)
                label.push_back(' ');
            space = false;
            label.append(plain, index, length);
        }
        index += length;
    }
    return label.empty() ? plain : label;
}

bool contains_nocase(std::string_view text, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > text.size())
        return false;
    const auto fold = [](char value) { return kLower[static_cast<unsigned char>(value)]; };
    const unsigned char first = fold(needle.front());
    const std::size_t last = text.size() - needle.size();
    for (std::size_t start = 0; start <= last; ++start)
    {
        if (fold(text[start]) != first)
            continue;
        std::size_t at = 1;
        while (at < needle.size() && fold(text[start + at]) == fold(needle[at]))
            ++at;
        if (at == needle.size())
            return true;
    }
    return false;
}

bool equals_nocase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (lower(left[index]) != lower(right[index]))
            return false;
    return true;
}

bool field_has_value(std::string_view field, std::string_view value)
{
    if (value.empty())
        return true;
    std::size_t start = 0;
    while (start < field.size())
    {
        while (start < field.size() && (is_separator(field[start]) || is_space(field[start])))
            ++start;
        std::size_t end = start;
        while (end < field.size() && !is_separator(field[end]))
            ++end;
        if (equals_nocase(trimmed(field.substr(start, end - start)), value))
            return true;
        start = end + 1u;
    }
    return false;
}

std::string first_value(std::string_view field)
{
    std::size_t end = 0;
    while (end < field.size() && !is_separator(field[end]))
        ++end;
    return std::string(trimmed(field.substr(0, end)));
}

unsigned quality_of(const iptv::ChannelView &channel)
{
    const Mentions mentions(channel);
    if (mentions("2160p") || mentions("3840x2160") || mentions(" 4k") || mentions("uhd"))
        return kQualityUhd;
    if (mentions("1080p") || mentions("1920x1080") || mentions("fhd"))
        return kQualityFullHd;
    if (mentions("720p") || mentions("1280x720") || mentions(" hd"))
        return kQualityHd;
    if (mentions("576p") || mentions("480p") || mentions("360p") || mentions("240p") ||
        mentions(" sd"))
        return kQualitySd;
    return kQualityAny;
}

const char *quality_filter_name(unsigned quality)
{
    static constexpr const char *names[kQualityCount] = {"Any quality", "SD", "720p", "1080p",
                                                         "4K"};
    return quality < kQualityCount ? names[quality] : names[0];
}

std::string resolution_label(const iptv::ChannelView &channel)
{
    static constexpr struct
    {
        const char *needle;
        const char *label;
    } sizes[] = {{"3840x2160", "4K"},  {"2160p", "4K"},        {"2560x1440", "1440p"},
                 {"1440p", "1440p"},   {"1920x1080", "1080p"}, {"1080p", "1080p"},
                 {"1280x720", "720p"}, {"720p", "720p"},       {"720x576", "576p"},
                 {"576p", "576p"},     {"720x480", "480p"},    {"480p", "480p"},
                 {"360p", "360p"},     {"270p", "270p"},       {"240p", "240p"}};
    const Mentions mentions(channel);
    for (const auto &size : sizes)
        if (mentions(size.needle))
            return size.label;
    return {};
}

const char *codec_label(const iptv::ChannelView &channel)
{
    const Mentions mentions(channel);
    if (mentions(".webm") || mentions("vp9"))
        return "VP9";
    if (mentions("hevc") || mentions("h265") || mentions("h.265"))
        return "HEVC";
    if (mentions("h264") || mentions("h.264") || mentions("avc"))
        return "H.264";
    return "";
}

std::string display_name(const iptv::ChannelView &channel, std::vector<std::string> *notes)
{
    std::string_view rest = trimmed(channel.name);
    // Notes sit at the end, each in its own brackets: peel them off from the right.
    for (;;)
    {
        if (rest.empty())
            break;
        const char close = rest.back();
        if (close != ')' && close != ']')
            break;
        const char open = close == ')' ? '(' : '[';
        const std::size_t at = rest.rfind(open);
        // A name that is nothing but a bracket keeps it.
        if (at == std::string_view::npos || at == 0)
            break;
        const std::string_view note = trimmed(rest.substr(at + 1, rest.size() - at - 2));
        // Only the playlist's notes go: a size in round brackets, any remark
        // in square ones. "(HD)" and "(Pluto TV)" are part of the name.
        if (close == ')' && !is_size_note(note))
            break;
        if (notes != nullptr && close == ']' && !note.empty())
            notes->insert(notes->begin(), std::string(note));
        rest = trimmed(rest.substr(0, at));
    }
    if (rest.empty())
        return "Unnamed channel";
    return std::string(rest);
}

std::string monogram(const iptv::ChannelView &channel)
{
    const std::string_view source = !channel.tvg_name.empty() ? channel.tvg_name
                                    : !channel.tvg_id.empty() ? channel.tvg_id
                                                              : channel.name;
    std::string letters;
    bool word_start = true;
    for (const char raw : source)
    {
        const unsigned char value = static_cast<unsigned char>(raw);
        const bool letter = value < 0x80 && std::isalnum(value) != 0;
        if (letter && word_start && letters.size() < 2)
            letters.push_back(static_cast<char>(std::toupper(value)));
        word_start = !letter;
    }
    if (letters.size() < 2)
    {
        // One word: its first two letters.
        bool skipped_first = letters.empty();
        for (const char raw : source)
        {
            const unsigned char value = static_cast<unsigned char>(raw);
            if (value >= 0x80 || std::isalnum(value) == 0)
                continue;
            if (!skipped_first)
            {
                skipped_first = true;
                continue;
            }
            letters.push_back(static_cast<char>(std::toupper(value)));
            if (letters.size() == 2)
                break;
        }
    }
    if (!letters.empty())
        return letters;
    // No Latin letter at all (a Chinese, Japanese or Korean name): its first
    // character says more than "TV".
    for (std::size_t index = 0; index < source.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(source[index]);
        const std::size_t length = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        if (lead >= 0xe0 && index + length <= source.size())
            return std::string(source.substr(index, length));
        index += length;
    }
    return "TV";
}

std::string category_of(const iptv::ChannelView &channel)
{
    const std::string first = first_value(channel.group_title);
    return first.empty() ? "Uncategorized" : first;
}

std::string place_line(const iptv::ChannelView &channel)
{
    std::string line = first_value(channel.tvg_country);
    if (line.empty())
        line = "World";
    const std::string language = first_value(channel.tvg_language);
    if (!language.empty())
        line += "  \xC2\xB7  " + language;
    return line;
}

namespace
{

// One code point of UTF-8; a byte that is not the start of one counts as itself.
std::uint32_t next_code(std::string_view text, std::size_t *index)
{
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const std::size_t at = *index;
    const unsigned char lead = byte(at);
    const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (length == 1 || at + length > text.size())
    {
        ++*index;
        return lead;
    }
    std::uint32_t code = lead & (0xFFu >> (length + 1));
    for (std::size_t i = 1; i < length; ++i)
        code = (code << 6) | (byte(at + i) & 0x3Fu);
    *index += length;
    return code;
}

// The capital A to Z a Latin letter is filed under (accents put aside), or 0.
char latin_base(std::uint32_t code)
{
    if (code < 0x80)
        return std::isalpha(static_cast<int>(code)) != 0
                   ? static_cast<char>(std::toupper(static_cast<int>(code)))
                   : '\0';
    if (code >= 0xC0 && code <= 0xFF)
    {
        // U+00C0..U+00DF; the small letters of U+00E0..U+00FF repeat them.
        static constexpr char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOO\0OUUUUYTS";
        return code == 0xFF ? 'Y' : kLatin1[(code - 0xC0) % 32];
    }
    if (code >= 0x100 && code <= 0x17F)
    {
        // Latin Extended-A: the last code of each run of one letter.
        static constexpr struct
        {
            std::uint16_t last;
            char base;
        } kRuns[] = {{0x105, 'A'}, {0x10D, 'C'}, {0x111, 'D'}, {0x11B, 'E'}, {0x123, 'G'},
                     {0x127, 'H'}, {0x133, 'I'}, {0x135, 'J'}, {0x138, 'K'}, {0x142, 'L'},
                     {0x14B, 'N'}, {0x153, 'O'}, {0x159, 'R'}, {0x161, 'S'}, {0x167, 'T'},
                     {0x173, 'U'}, {0x175, 'W'}, {0x178, 'Y'}, {0x17E, 'Z'}, {0x17F, 'S'}};
        for (const auto &run : kRuns)
            if (code <= run.last)
                return run.base;
    }
    return '\0';
}

} // namespace

std::string sort_key(const iptv::ChannelView &channel)
{
    const std::string name = display_name(channel);
    std::string folded;
    folded.reserve(name.size());
    for (std::size_t index = 0; index < name.size();)
    {
        const std::size_t start = index;
        const std::uint32_t code = next_code(name, &index);
        if (const char base = latin_base(code); base != '\0')
            folded.push_back(base);
        else if (code >= 0x80 || std::isdigit(static_cast<int>(code)) != 0)
            folded.append(name, start, index - start);
        else if (!folded.empty() && code == ' ' && folded.back() != ' ')
            folded.push_back(' '); // punctuation is passed over; one space keeps words apart
    }
    if (!folded.empty() && folded.back() == ' ')
        folded.pop_back();
    const char first = folded.empty() ? '\0' : folded.front();
    // '#' holds what starts with a digit or in another script, and sorts first.
    return std::string(1, first >= 'A' && first <= 'Z' ? first : '#') + folded;
}

int letter_of_key(std::string_view key)
{
    return key.empty() || key.front() < 'A' || key.front() > 'Z' ? 0 : key.front() - 'A' + 1;
}

char letter_char(int letter)
{
    return letter >= 1 && letter < kLetterCount ? static_cast<char>('A' + letter - 1) : '#';
}

std::string group_digits(unsigned value)
{
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%u", value);
    std::string text(digits);
    for (int at = static_cast<int>(text.size()) - 3; at > 0; at -= 3)
        text.insert(static_cast<std::size_t>(at), ",");
    return text;
}

} // namespace ptv
