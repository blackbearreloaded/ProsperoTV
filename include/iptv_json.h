/* ProsperoTV - Shared provider JSON reader, derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "iptv_catalog.h"
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace iptv::json
{
inline constexpr std::size_t kMaxJsonDepth = 24u;

inline int HexDigit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

inline bool AppendUtf8(std::uint32_t codepoint, std::string *output, std::size_t maximum)
{
    if (!output)
        return true;
    if (codepoint < 0x20u || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu))
        return false;
    const std::size_t bytes = codepoint < 0x80u      ? 1u
                              : codepoint < 0x800u   ? 2u
                              : codepoint < 0x10000u ? 3u
                                                     : 4u;
    if (output->size() + bytes > maximum)
        return false;
    char encoded[4]{};
    if (bytes == 1u)
        encoded[0] = static_cast<char>(codepoint);
    else if (bytes == 2u)
    {
        encoded[0] = static_cast<char>(0xc0u | (codepoint >> 6u));
        encoded[1] = static_cast<char>(0x80u | (codepoint & 0x3fu));
    }
    else if (bytes == 3u)
    {
        encoded[0] = static_cast<char>(0xe0u | (codepoint >> 12u));
        encoded[1] = static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu));
        encoded[2] = static_cast<char>(0x80u | (codepoint & 0x3fu));
    }
    else
    {
        encoded[0] = static_cast<char>(0xf0u | (codepoint >> 18u));
        encoded[1] = static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu));
        encoded[2] = static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu));
        encoded[3] = static_cast<char>(0x80u | (codepoint & 0x3fu));
    }
    output->append(encoded, bytes);
    return true;
}

class JsonReader
{
  public:
    explicit JsonReader(std::string_view input) : input_(input)
    {
    }

    void Whitespace()
    {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])))
            ++position_;
    }

    char Peek()
    {
        Whitespace();
        return position_ < input_.size() ? input_[position_] : '\0';
    }

    bool Consume(char expected)
    {
        Whitespace();
        if (position_ >= input_.size() || input_[position_] != expected)
            return false;
        ++position_;
        return true;
    }

    bool Finished()
    {
        Whitespace();
        return position_ == input_.size();
    }

    bool String(std::string *output, std::size_t maximum = kDefaultMaxFieldBytes)
    {
        Whitespace();
        if (position_ >= input_.size() || input_[position_++] != '"')
            return false;
        if (output)
            output->clear();
        while (position_ < input_.size())
        {
            const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"')
                return true;
            if (byte < 0x20u)
                return false;
            if (byte != '\\')
            {
                if (output)
                {
                    if (output->size() >= maximum)
                        return false;
                    output->push_back(static_cast<char>(byte));
                }
                continue;
            }
            if (position_ >= input_.size())
                return false;
            const char escaped = input_[position_++];
            char simple = '\0';
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                simple = escaped;
                break;
            case 'b':
                simple = ' ';
                break;
            case 'f':
                simple = ' ';
                break;
            case 'n':
                simple = ' ';
                break;
            case 'r':
                simple = ' ';
                break;
            case 't':
                simple = ' ';
                break;
            case 'u':
            {
                std::uint32_t codepoint = 0;
                if (!UnicodeEscape(&codepoint))
                    return false;
                if (codepoint >= 0xd800u && codepoint <= 0xdbffu)
                {
                    if (position_ + 2u > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1u] != 'u')
                        return false;
                    position_ += 2u;
                    std::uint32_t low = 0;
                    if (!UnicodeEscape(&low) || low < 0xdc00u || low > 0xdfffu)
                        return false;
                    codepoint = 0x10000u + ((codepoint - 0xd800u) << 10u) + (low - 0xdc00u);
                }
                else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu)
                {
                    return false;
                }
                if (!AppendUtf8(codepoint, output, maximum))
                    return false;
                continue;
            }
            default:
                return false;
            }
            if (output)
            {
                if (output->size() >= maximum)
                    return false;
                output->push_back(simple);
            }
        }
        return false;
    }

    bool Scalar(std::string *output)
    {
        Whitespace();
        const std::size_t start = position_;
        while (position_ < input_.size())
        {
            const char value = input_[position_];
            if (value == ',' || value == ']' || value == '}' ||
                std::isspace(static_cast<unsigned char>(value)))
                break;
            ++position_;
        }
        if (position_ == start)
            return false;
        const auto token = input_.substr(start, position_ - start);
        if (token != "true" && token != "false" && token != "null")
        {
            std::size_t at = token.front() == '-' ? 1u : 0u;
            const auto digit = [](char c) { return c >= '0' && c <= '9'; };
            if (at == token.size() || !digit(token[at]))
                return false;
            if (token[at] == '0')
                ++at;
            else
                while (at < token.size() && digit(token[at]))
                    ++at;
            if (at < token.size() && token[at] == '.')
            {
                const auto first = ++at;
                while (at < token.size() && digit(token[at]))
                    ++at;
                if (at == first)
                    return false;
            }
            if (at < token.size() && (token[at] == 'e' || token[at] == 'E'))
            {
                ++at;
                if (at < token.size() && (token[at] == '-' || token[at] == '+'))
                    ++at;
                const auto first = at;
                while (at < token.size() && digit(token[at]))
                    ++at;
                if (at == first)
                    return false;
            }
            if (at != token.size())
                return false;
        }
        if (output)
            *output = std::string(input_.substr(start, position_ - start));
        return true;
    }

    bool StringOrScalar(std::string *output, std::size_t maximum = kDefaultMaxFieldBytes)
    {
        if (Peek() == '"')
            return String(output, maximum);
        if (!Scalar(output))
            return false;
        return !output || output->size() <= maximum;
    }

    bool SkipValue(std::size_t depth = 0)
    {
        if (depth >= kMaxJsonDepth)
            return false;
        const char value = Peek();
        if (value == '"')
            return String(nullptr);
        if (value == '{')
        {
            if (!Consume('{'))
                return false;
            if (Consume('}'))
                return true;
            for (;;)
            {
                if (!String(nullptr) || !Consume(':') || !SkipValue(depth + 1u))
                    return false;
                if (Consume('}'))
                    return true;
                if (!Consume(','))
                    return false;
            }
        }
        if (value == '[')
        {
            if (!Consume('['))
                return false;
            if (Consume(']'))
                return true;
            for (;;)
            {
                if (!SkipValue(depth + 1u))
                    return false;
                if (Consume(']'))
                    return true;
                if (!Consume(','))
                    return false;
            }
        }
        return Scalar(nullptr);
    }

  private:
    bool UnicodeEscape(std::uint32_t *codepoint)
    {
        if (!codepoint || position_ + 4u > input_.size())
            return false;
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4u; ++index)
        {
            const int digit = HexDigit(input_[position_++]);
            if (digit < 0)
                return false;
            value = (value << 4u) | static_cast<std::uint32_t>(digit);
        }
        *codepoint = value;
        return true;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

template <typename Handler> bool ReadObject(JsonReader *reader, Handler handler)
{
    if (!reader || !reader->Consume('{'))
        return false;
    if (reader->Consume('}'))
        return true;
    for (;;)
    {
        std::string key;
        if (!reader->String(&key, 128u) || !reader->Consume(':') || !handler(key, reader))
            return false;
        if (reader->Consume('}'))
            return true;
        if (!reader->Consume(','))
            return false;
    }
}

template <typename Handler> bool ReadArray(JsonReader *reader, Handler handler)
{
    if (!reader || !reader->Consume('['))
        return false;
    if (reader->Consume(']'))
        return true;
    for (;;)
    {
        if (!handler(reader))
            return false;
        if (reader->Consume(']'))
            return true;
        if (!reader->Consume(','))
            return false;
    }
}

inline bool IsJsonSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// Cuts an answer into the elements of its list as the bytes arrive: the list
// is the document itself, or the "data" member of the document. Only the
// element that is passing through is kept, and only when it straddles two
// pieces; everything around the list is checked for balance and let go.
class ListSplitter
{
  public:
    // Stalker puts the data array inside a "js" object; Xtream puts it at
    // the root. The same bounded element reader serves both providers.
    explicit ListSplitter(std::string_view wrapper = {}) : wrapper_(wrapper)
    {
    }
    // False when the bytes cannot be such a document, or the handler refuses
    // an element. The handler is given each element as text.
    template <typename Handler> bool Feed(std::string_view bytes, Handler &&handler)
    {
        if (failed_)
            return false;
        std::size_t start = 0; // of the element passing through, in this piece
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            const char value = bytes[index];
            switch (phase_)
            {
            case Phase::start:
                if (IsJsonSpace(value))
                    break;
                if (value == '[')
                {
                    found_ = true;
                    root_is_list_ = true;
                    phase_ = Phase::list;
                }
                else if (value == '{')
                {
                    depth_ = 1;
                    phase_ = Phase::object;
                }
                else
                {
                    return Fail();
                }
                break;
            case Phase::object:
                if (in_string_)
                {
                    if (escape_)
                        escape_ = false;
                    else if (value == '\\')
                        escape_ = true;
                    else if (value == '"')
                        in_string_ = false;
                    else if (depth_ == list_depth_ && key_.size() < 32u)
                        key_.push_back(value);
                    break;
                }
                if (value == '"')
                {
                    in_string_ = true;
                    if (depth_ == list_depth_)
                        key_.clear();
                }
                else if (value == '[' && depth_ == list_depth_ && after_colon_ && !found_ &&
                         key_ == "data")
                {
                    found_ = true;
                    phase_ = Phase::list;
                }
                else if (value == '{' || value == '[')
                {
                    if (value == '{' && depth_ == 1 && after_colon_ && !wrapper_.empty() &&
                        key_ == wrapper_)
                    {
                        list_depth_ = 2;
                        after_colon_ = false;
                    }
                    if (++depth_ > kMaxJsonDepth)
                        return Fail();
                }
                else if (value == '}' || value == ']')
                {
                    if (--depth_ == 0)
                        phase_ = Phase::done;
                }
                else if (depth_ == list_depth_ && value == ':')
                {
                    after_colon_ = true;
                }
                else if (depth_ == list_depth_ && value == ',')
                {
                    after_colon_ = false;
                }
                break;
            case Phase::list:
                if (IsJsonSpace(value))
                    break;
                if (value == ']')
                {
                    if (after_separator_)
                        return Fail();
                    phase_ = root_is_list_ ? Phase::done : Phase::object;
                    after_colon_ = false;
                    break;
                }
                if (after_element_)
                {
                    if (value != ',')
                        return Fail();
                    after_element_ = false;
                    after_separator_ = true;
                    break;
                }
                if (value == ',')
                    return Fail();
                after_separator_ = false;
                start = index;
                in_string_ = value == '"';
                escape_ = false;
                depth_element_ = value == '{' || value == '[' ? 1u : 0u;
                phase_ = Phase::element;
                break;
            case Phase::element:
            {
                bool ended = false;
                std::size_t end = index + 1u;
                if (in_string_)
                {
                    if (escape_)
                        escape_ = false;
                    else if (value == '\\')
                        escape_ = true;
                    else if (value == '"')
                    {
                        in_string_ = false;
                        ended = depth_element_ == 0;
                    }
                }
                else if (depth_element_ == 0)
                {
                    // A bare value ends where the list goes on.
                    if (value == ',' || value == ']' || IsJsonSpace(value))
                    {
                        ended = true;
                        end = index;
                        --index; // the list reads this byte itself
                    }
                }
                else if (value == '"')
                {
                    in_string_ = true;
                }
                else if (value == '{' || value == '[')
                {
                    if (++depth_element_ > kMaxJsonDepth)
                        return Fail();
                }
                else if (value == '}' || value == ']')
                {
                    ended = --depth_element_ == 0;
                }
                if (!ended)
                    break;
                std::string_view element = bytes.substr(start, end - start);
                if (element.size() > kMaxElementBytes)
                    return Fail();
                if (!element_.empty())
                {
                    if (element_.size() + element.size() > kMaxElementBytes)
                        return Fail();
                    element_.append(element);
                    element = element_;
                }
                if (!handler(element))
                    return Fail();
                element_.clear();
                after_element_ = true;
                phase_ = Phase::list;
                break;
            }
            case Phase::done:
                if (!IsJsonSpace(value))
                    return Fail();
                break;
            }
        }
        if (phase_ == Phase::element)
        {
            if (element_.size() + bytes.size() - start > kMaxElementBytes)
                return Fail();
            element_.append(bytes.substr(start));
        }
        return true;
    }

    // The document ended where a document ends.
    bool complete() const
    {
        return !failed_ && phase_ == Phase::done;
    }
    // It had a list.
    bool found() const
    {
        return found_;
    }

  private:
    enum class Phase : std::uint8_t
    {
        start,
        object,
        list,
        element,
        done,
    };
    static constexpr std::size_t kMaxElementBytes = 1024u * 1024u;

    bool Fail()
    {
        failed_ = true;
        return false;
    }

    Phase phase_ = Phase::start;
    bool failed_ = false;
    bool found_ = false;
    bool root_is_list_ = false;
    bool in_string_ = false;
    bool escape_ = false;
    bool after_colon_ = false;
    bool after_element_ = false;
    bool after_separator_ = false;
    std::size_t depth_ = 0;
    std::size_t depth_element_ = 0;
    std::size_t list_depth_ = 1;
    std::string wrapper_;
    std::string key_;
    std::string element_;
};

} // namespace iptv::json
