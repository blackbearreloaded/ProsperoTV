/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_xtream.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace iptv
{
namespace
{

constexpr char kCredentialsMagic[] = "PROSPEROTV-XTREAM-1";
constexpr std::size_t kMaxJsonDepth = 24u;
constexpr std::size_t kMaxCategories = 4096u;

bool EqualsCi(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index])))
            return false;
    return true;
}

bool SafeCredential(std::string_view value, std::size_t maximum)
{
    if (value.empty() || value.size() > maximum)
        return false;
    for (const unsigned char byte : value)
        if (byte < 0x20u || byte == 0x7fu)
            return false;
    return true;
}

bool EndsWithCi(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() &&
           EqualsCi(value.substr(value.size() - suffix.size()), suffix);
}

bool PercentEncode(std::string_view input, std::string *output)
{
    if (!output)
        return false;
    static constexpr char digits[] = "0123456789ABCDEF";
    output->clear();
    output->reserve(input.size() * 3u);
    for (const unsigned char byte : input)
    {
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~')
        {
            output->push_back(static_cast<char>(byte));
        }
        else
        {
            output->push_back('%');
            output->push_back(digits[byte >> 4u]);
            output->push_back(digits[byte & 0x0fu]);
        }
    }
    return true;
}

XtreamStatus ReplaceFile(const std::string &temporary, const std::string &path)
{
#ifdef _WIN32
    return MoveFileExA(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? XtreamStatus::ok
               : XtreamStatus::io_error;
#else
    return std::rename(temporary.c_str(), path.c_str()) == 0 ? XtreamStatus::ok
                                                             : XtreamStatus::io_error;
#endif
}

int HexDigit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

bool AppendUtf8(std::uint32_t codepoint, std::string *output, std::size_t maximum)
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

template <typename Handler> bool ReadArrayResponse(JsonReader *reader, Handler handler, bool *found)
{
    if (!reader || !found)
        return false;
    *found = false;
    if (reader->Peek() == '[')
    {
        *found = true;
        return ReadArray(reader, handler) && reader->Finished();
    }
    if (reader->Peek() != '{')
        return false;
    const bool valid = ReadObject(reader,
                                  [&](const std::string &key, JsonReader *value)
                                  {
                                      if (key == "data" && value->Peek() == '[')
                                      {
                                          *found = true;
                                          return ReadArray(value, handler);
                                      }
                                      return value->SkipValue();
                                  });
    return valid && reader->Finished();
}

std::string CategoryName(const std::unordered_map<std::string, std::string> &categories,
                         const std::string &id)
{
    const auto found = categories.find(id);
    return found == categories.end() || found->second.empty() ? "Live TV" : found->second;
}

std::string StableXtreamChannelId(std::uint64_t source_id, std::string_view stream_id)
{
    char prefix[32]{};
    std::snprintf(prefix, sizeof(prefix),
                  "xtream:%016llx:", static_cast<unsigned long long>(source_id));
    return std::string(prefix) + std::string(stream_id);
}

} // namespace

bool NormalizeXtreamServerUrl(std::string_view input, std::string *normalized)
{
    if (!normalized || input.empty() || input.size() > kMaxXtreamServerBytes ||
        input.find('?') != std::string_view::npos || input.find('#') != std::string_view::npos)
        return false;
    std::string canonical;
    if (!CanonicalizeStreamUrl(input, &canonical))
        return false;
    while (!canonical.empty() && canonical.back() == '/')
        canonical.pop_back();
    constexpr std::string_view endpoint = "/player_api.php";
    if (EndsWithCi(canonical, endpoint))
        canonical.resize(canonical.size() - endpoint.size());
    while (!canonical.empty() && canonical.back() == '/')
        canonical.pop_back();
    if (canonical.size() < 8u)
        return false;
    *normalized = std::move(canonical);
    return true;
}

bool ValidateXtreamCredentials(const XtreamCredentials &credentials)
{
    std::string normalized;
    return NormalizeXtreamServerUrl(credentials.server_url, &normalized) &&
           normalized == credentials.server_url &&
           SafeCredential(credentials.username, kMaxXtreamCredentialBytes) &&
           SafeCredential(credentials.password, kMaxXtreamCredentialBytes);
}

std::uint64_t XtreamSourceId(const XtreamCredentials &credentials)
{
    std::uint64_t hash = UINT64_C(1469598103934665603);
    const auto add = [&hash](std::string_view value)
    {
        for (const unsigned char byte : value)
        {
            hash ^= byte;
            hash *= UINT64_C(1099511628211);
        }
        hash ^= 0xffu;
        hash *= UINT64_C(1099511628211);
    };
    add(credentials.server_url);
    add(credentials.username);
    add(credentials.password);
    return UINT64_C(0x5854000000000000) | (hash & UINT64_C(0x0000ffffffffffff));
}

bool BuildXtreamApiUrl(const XtreamCredentials &credentials, std::string_view action,
                       std::string *url)
{
    if (!url || !ValidateXtreamCredentials(credentials))
        return false;
    std::string username;
    std::string password;
    std::string encoded_action;
    PercentEncode(credentials.username, &username);
    PercentEncode(credentials.password, &password);
    PercentEncode(action, &encoded_action);
    *url =
        credentials.server_url + "/player_api.php?username=" + username + "&password=" + password;
    if (!action.empty())
        *url += "&action=" + encoded_action;
    return url->size() <= kDefaultMaxUrlBytes;
}

bool BuildXtreamLiveUrl(const XtreamCredentials &credentials, std::string_view stream_id,
                        std::string_view extension, std::string *url)
{
    if (!url || !ValidateXtreamCredentials(credentials) || stream_id.empty() ||
        stream_id.size() > 64u || extension.size() > 12u)
        return false;
    std::string selected_extension = extension.empty() ? "ts" : std::string(extension);
    if (!std::all_of(selected_extension.begin(), selected_extension.end(),
                     [](unsigned char value) { return std::isalnum(value) != 0; }))
        return false;
    std::string username;
    std::string password;
    std::string stream;
    PercentEncode(credentials.username, &username);
    PercentEncode(credentials.password, &password);
    PercentEncode(stream_id, &stream);
    *url = credentials.server_url + "/live/" + username + "/" + password + "/" + stream + "." +
           selected_extension;
    return url->size() <= kDefaultMaxUrlBytes;
}

XtreamStatus SaveXtreamCredentials(const std::string &path, const XtreamCredentials &credentials)
{
    if (path.empty() || !ValidateXtreamCredentials(credentials))
        return XtreamStatus::invalid_argument;
    const std::string temporary = path + ".tmp";
    std::FILE *output = std::fopen(temporary.c_str(), "wb");
    if (!output)
        return XtreamStatus::io_error;
    const std::array<std::string_view, 4> lines = {kCredentialsMagic, credentials.server_url,
                                                   credentials.username, credentials.password};
    bool written = true;
    for (const std::string_view line : lines)
    {
        written = written && std::fwrite(line.data(), 1, line.size(), output) == line.size();
        written = written && std::fwrite("\n", 1, 1, output) == 1;
    }
    written = written && std::fflush(output) == 0;
    written = std::fclose(output) == 0 && written;
    if (!written)
    {
        std::remove(temporary.c_str());
        return XtreamStatus::io_error;
    }
    const XtreamStatus replaced = ReplaceFile(temporary, path);
    if (replaced != XtreamStatus::ok)
        std::remove(temporary.c_str());
    return replaced;
}

XtreamStatus LoadXtreamCredentials(const std::string &path, XtreamCredentials *credentials)
{
    if (path.empty() || !credentials)
        return XtreamStatus::invalid_argument;
    std::FILE *input = std::fopen(path.c_str(), "rb");
    if (!input)
        return XtreamStatus::not_found;
    constexpr std::size_t capacity =
        sizeof(kCredentialsMagic) + kMaxXtreamServerBytes + 2u * kMaxXtreamCredentialBytes + 8u;
    std::array<char, capacity> file{};
    const std::size_t bytes = std::fread(file.data(), 1, file.size(), input);
    const bool failed = std::ferror(input) != 0 || std::fclose(input) != 0;
    if (failed)
        return XtreamStatus::io_error;
    if (bytes == file.size())
        return XtreamStatus::too_large;
    std::array<std::string, 4> lines;
    std::size_t start = 0;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::size_t newline = std::string_view(file.data(), bytes).find('\n', start);
        if (newline == std::string_view::npos)
            return XtreamStatus::corrupt;
        std::size_t end = newline;
        if (end > start && file[end - 1u] == '\r')
            --end;
        lines[index].assign(file.data() + start, end - start);
        start = newline + 1u;
    }
    if (start != bytes || lines[0] != kCredentialsMagic)
        return XtreamStatus::corrupt;
    XtreamCredentials loaded{lines[1], lines[2], lines[3]};
    if (!ValidateXtreamCredentials(loaded))
        return XtreamStatus::corrupt;
    *credentials = std::move(loaded);
    return XtreamStatus::ok;
}

XtreamStatus SaveXtreamCredentials(const XtreamCredentials &credentials)
{
    return SaveXtreamCredentials(kDefaultXtreamCredentialsPath, credentials);
}

XtreamStatus LoadXtreamCredentials(XtreamCredentials *credentials)
{
    return LoadXtreamCredentials(kDefaultXtreamCredentialsPath, credentials);
}

XtreamStatus ParseXtreamAuth(std::string_view json, XtreamAuth *auth)
{
    if (!auth)
        return XtreamStatus::invalid_argument;
    *auth = {};
    if (json.empty() || json.size() > kMaxXtreamResponseBytes)
        return json.size() > kMaxXtreamResponseBytes ? XtreamStatus::too_large
                                                     : XtreamStatus::malformed_json;
    JsonReader reader(json);
    bool found_user = false;
    std::string authenticated;
    const bool valid =
        ReadObject(&reader,
                   [&](const std::string &key, JsonReader *value)
                   {
                       if (key != "user_info")
                           return value->SkipValue();
                       found_user = true;
                       return ReadObject(value,
                                         [&](const std::string &field, JsonReader *entry)
                                         {
                                             if (field == "auth")
                                                 return entry->StringOrScalar(&authenticated, 16u);
                                             if (field == "status")
                                                 return entry->StringOrScalar(&auth->status, 64u);
                                             if (field == "message")
                                                 return entry->StringOrScalar(&auth->message, 256u);
                                             return entry->SkipValue();
                                         });
                   });
    if (!valid || !reader.Finished() || !found_user)
        return XtreamStatus::malformed_json;
    auth->authenticated = authenticated == "1" || EqualsCi(authenticated, "true");
    if (!auth->authenticated)
        return XtreamStatus::authentication_failed;
    if (!auth->status.empty() && !EqualsCi(auth->status, "active"))
        return XtreamStatus::account_inactive;
    return XtreamStatus::ok;
}

XtreamStatus ParseXtreamCategories(std::string_view json, std::vector<XtreamCategory> *categories)
{
    if (!categories)
        return XtreamStatus::invalid_argument;
    categories->clear();
    if (json.empty() || json.size() > kMaxXtreamResponseBytes)
        return json.size() > kMaxXtreamResponseBytes ? XtreamStatus::too_large
                                                     : XtreamStatus::malformed_json;
    JsonReader reader(json);
    bool found = false;
    const bool valid = ReadArrayResponse(
        &reader,
        [&](JsonReader *entry)
        {
            XtreamCategory category;
            if (!ReadObject(entry,
                            [&](const std::string &key, JsonReader *value)
                            {
                                if (key == "category_id")
                                    return value->StringOrScalar(&category.id, 64u);
                                if (key == "category_name")
                                    return value->StringOrScalar(&category.name);
                                return value->SkipValue();
                            }))
                return false;
            if (!category.id.empty() && categories->size() < kMaxCategories)
                categories->push_back(std::move(category));
            return true;
        },
        &found);
    return valid && found ? XtreamStatus::ok : XtreamStatus::malformed_json;
}

XtreamStatus ParseXtreamLiveStreams(std::string_view json, const XtreamCredentials &credentials,
                                    const std::vector<XtreamCategory> &categories,
                                    std::uint64_t source_id, CatalogState *catalog,
                                    ParseReport *report)
{
    if (!catalog || !ValidateXtreamCredentials(credentials) || source_id == 0)
        return XtreamStatus::invalid_argument;
    *catalog = {};
    catalog->source_id = source_id;
    if (report)
        *report = {};
    if (json.empty() || json.size() > kMaxXtreamResponseBytes)
        return json.size() > kMaxXtreamResponseBytes ? XtreamStatus::too_large
                                                     : XtreamStatus::malformed_json;

    std::unordered_map<std::string, std::string> category_names;
    category_names.reserve(categories.size());
    for (const XtreamCategory &category : categories)
        if (!category.id.empty())
            category_names.emplace(category.id, category.name);
    std::unordered_set<std::string> stream_ids;
    JsonReader reader(json);
    bool found = false;
    std::uint32_t source_line = 0;
    const bool valid = ReadArrayResponse(
        &reader,
        [&](JsonReader *entry)
        {
            ++source_line;
            if (report)
                ++report->lines_seen;
            std::string stream_id;
            std::string name;
            std::string logo;
            std::string epg_id;
            std::string category_id;
            std::string extension;
            std::string direct_source;
            std::string stream_url;
            if (!ReadObject(entry,
                            [&](const std::string &key, JsonReader *value)
                            {
                                if (key == "stream_id")
                                    return value->StringOrScalar(&stream_id, 64u);
                                if (key == "name")
                                    return value->StringOrScalar(&name);
                                if (key == "stream_icon")
                                    return value->StringOrScalar(&logo, kDefaultMaxUrlBytes);
                                if (key == "epg_channel_id")
                                    return value->StringOrScalar(&epg_id);
                                if (key == "category_id")
                                    return value->StringOrScalar(&category_id, 64u);
                                if (key == "container_extension")
                                    return value->StringOrScalar(&extension, 12u);
                                if (key == "direct_source")
                                    return value->StringOrScalar(&direct_source,
                                                                 kDefaultMaxUrlBytes);
                                if (key == "stream_url")
                                    return value->StringOrScalar(&stream_url, kDefaultMaxUrlBytes);
                                return value->SkipValue();
                            }))
                return false;
            std::string generated_url;
            if (stream_id.empty() || !stream_ids.emplace(stream_id).second ||
                !BuildXtreamLiveUrl(credentials, stream_id, extension, &generated_url))
            {
                if (report)
                    ++report->skipped;
                return true;
            }
            if (catalog->channels.size() >= kDefaultMaxChannels)
            {
                if (report)
                    ++report->skipped;
                return true;
            }
            Channel channel;
            channel.id = StableXtreamChannelId(source_id, stream_id);
            channel.source_id = source_id;
            channel.name = name.empty() ? "Channel " + stream_id : std::move(name);
            channel.tvg_name = channel.name;
            channel.tvg_id = std::move(epg_id);
            channel.group_title = CategoryName(category_names, category_id);
            channel.source_line = source_line;
            std::string canonical;
            if (CanonicalizeStreamUrl(logo, &canonical))
                channel.tvg_logo = std::move(canonical);
            if (direct_source.empty())
                direct_source = std::move(stream_url);
            if (CanonicalizeStreamUrl(direct_source, &canonical) && canonical != generated_url)
            {
                channel.url = std::move(canonical);
                channel.alternate_urls.push_back(std::move(generated_url));
            }
            else
            {
                channel.url = std::move(generated_url);
            }
            catalog->channels.push_back(std::move(channel));
            if (report)
                ++report->accepted;
            return true;
        },
        &found);
    if (!valid || !found)
    {
        *catalog = {};
        return XtreamStatus::malformed_json;
    }
    return catalog->channels.empty() ? XtreamStatus::no_channels : XtreamStatus::ok;
}

const char *XtreamStatusDescription(XtreamStatus status)
{
    switch (status)
    {
    case XtreamStatus::ok:
        return "ready";
    case XtreamStatus::invalid_argument:
        return "invalid Xtream server or credentials";
    case XtreamStatus::not_found:
        return "Xtream credentials are not configured";
    case XtreamStatus::too_large:
        return "Xtream response exceeds the supported size";
    case XtreamStatus::io_error:
        return "Xtream credentials could not be stored";
    case XtreamStatus::corrupt:
        return "saved Xtream credentials are invalid";
    case XtreamStatus::malformed_json:
        return "provider returned malformed Xtream data";
    case XtreamStatus::authentication_failed:
        return "Xtream username or password was rejected";
    case XtreamStatus::account_inactive:
        return "Xtream account is expired or inactive";
    case XtreamStatus::no_channels:
        return "Xtream provider returned no live channels";
    }
    return "Xtream request failed";
}

} // namespace iptv
