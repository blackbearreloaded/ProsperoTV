/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_source_state.h"

#include "iptv_http.h"

#include <array>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace iptv
{
namespace
{

SourceStateStatus CopyValidated(std::string_view input,
                                std::array<char, kMaxCustomSourceUrlBytes + 1u> *output)
{
    if (!output || input.empty())
        return SourceStateStatus::invalid_argument;
    if (input.size() > kMaxCustomSourceUrlBytes)
        return SourceStateStatus::too_large;
    if (std::memchr(input.data(), '\0', input.size()) != nullptr)
        return SourceStateStatus::invalid_argument;
    std::memcpy(output->data(), input.data(), input.size());
    (*output)[input.size()] = '\0';
    return http::IsSupportedPlaylistUrl(output->data()) ? SourceStateStatus::ok
                                                        : SourceStateStatus::invalid_argument;
}

SourceStateStatus ReplaceFile(const std::string &temporary, const std::string &path)
{
#ifdef _WIN32
    return MoveFileExA(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? SourceStateStatus::ok
               : SourceStateStatus::io_error;
#else
    return std::rename(temporary.c_str(), path.c_str()) == 0 ? SourceStateStatus::ok
                                                             : SourceStateStatus::io_error;
#endif
}

} // namespace

SourceStateStatus SaveCustomSourceUrl(const std::string &path, std::string_view url)
{
    if (path.empty())
        return SourceStateStatus::invalid_argument;
    std::array<char, kMaxCustomSourceUrlBytes + 1u> value{};
    const SourceStateStatus validation = CopyValidated(url, &value);
    if (validation != SourceStateStatus::ok)
        return validation;

    const std::string temporary = path + ".tmp";
    std::FILE *output = std::fopen(temporary.c_str(), "wb");
    if (!output)
        return SourceStateStatus::io_error;
    bool written = std::fwrite(value.data(), 1, url.size(), output) == url.size();
    written = written && std::fwrite("\n", 1, 1, output) == 1;
    written = written && std::fflush(output) == 0;
    written = std::fclose(output) == 0 && written;
    if (!written)
    {
        std::remove(temporary.c_str());
        return SourceStateStatus::io_error;
    }
    const SourceStateStatus replaced = ReplaceFile(temporary, path);
    if (replaced != SourceStateStatus::ok)
        std::remove(temporary.c_str());
    return replaced;
}

SourceStateStatus LoadCustomSourceUrl(const std::string &path, std::string *url)
{
    if (path.empty() || !url)
        return SourceStateStatus::invalid_argument;
    std::FILE *input = std::fopen(path.c_str(), "rb");
    if (!input)
        return SourceStateStatus::not_found;

    std::array<char, kMaxCustomSourceUrlBytes + 2u> file{};
    const std::size_t bytes = std::fread(file.data(), 1, file.size(), input);
    const bool read_error = std::ferror(input) != 0;
    const bool close_ok = std::fclose(input) == 0;
    if (read_error || !close_ok)
        return SourceStateStatus::io_error;
    if (bytes == file.size())
        return SourceStateStatus::too_large;
    if (!bytes)
        return SourceStateStatus::corrupt;

    std::size_t size = bytes;
    if (file[size - 1u] == '\n')
        --size;
    if (size && file[size - 1u] == '\r')
        --size;
    if (!size || std::memchr(file.data(), '\n', size) != nullptr ||
        std::memchr(file.data(), '\r', size) != nullptr)
        return SourceStateStatus::corrupt;

    std::array<char, kMaxCustomSourceUrlBytes + 1u> value{};
    const SourceStateStatus validation = CopyValidated(std::string_view(file.data(), size), &value);
    if (validation != SourceStateStatus::ok)
        return validation == SourceStateStatus::too_large ? validation : SourceStateStatus::corrupt;
    *url = std::string(value.data(), size);
    return SourceStateStatus::ok;
}

SourceStateStatus SaveActiveSource(const std::string &path, bool custom)
{
    if (path.empty())
        return SourceStateStatus::invalid_argument;
    constexpr char built_in[] = "built-in\n";
    constexpr char custom_source[] = "custom\n";
    const char *value = custom ? custom_source : built_in;
    const std::size_t bytes = custom ? sizeof(custom_source) - 1u : sizeof(built_in) - 1u;
    const std::string temporary = path + ".tmp";
    std::FILE *output = std::fopen(temporary.c_str(), "wb");
    if (!output)
        return SourceStateStatus::io_error;
    bool written = std::fwrite(value, 1, bytes, output) == bytes;
    written = written && std::fflush(output) == 0;
    written = std::fclose(output) == 0 && written;
    if (!written)
    {
        std::remove(temporary.c_str());
        return SourceStateStatus::io_error;
    }
    const SourceStateStatus replaced = ReplaceFile(temporary, path);
    if (replaced != SourceStateStatus::ok)
        std::remove(temporary.c_str());
    return replaced;
}

SourceStateStatus LoadActiveSource(const std::string &path, bool *custom)
{
    if (path.empty() || !custom)
        return SourceStateStatus::invalid_argument;
    std::FILE *input = std::fopen(path.c_str(), "rb");
    if (!input)
        return SourceStateStatus::not_found;
    char value[10] = {};
    const std::size_t bytes = std::fread(value, 1, sizeof(value), input);
    const bool failed = std::ferror(input) != 0 || std::fclose(input) != 0;
    if (failed)
        return SourceStateStatus::io_error;
    if (bytes == sizeof(value))
        return SourceStateStatus::corrupt;
    if (std::strcmp(value, "built-in\n") == 0)
    {
        *custom = false;
        return SourceStateStatus::ok;
    }
    if (std::strcmp(value, "custom\n") == 0)
    {
        *custom = true;
        return SourceStateStatus::ok;
    }
    return SourceStateStatus::corrupt;
}

SourceStateStatus SaveCustomSourceUrl(std::string_view url)
{
    return SaveCustomSourceUrl(kDefaultCustomSourcePath, url);
}

SourceStateStatus LoadCustomSourceUrl(std::string *url)
{
    return LoadCustomSourceUrl(kDefaultCustomSourcePath, url);
}

SourceStateStatus SaveActiveSource(bool custom)
{
    return SaveActiveSource(kDefaultActiveSourcePath, custom);
}

SourceStateStatus LoadActiveSource(bool *custom)
{
    return LoadActiveSource(kDefaultActiveSourcePath, custom);
}

std::uint64_t CustomSourceId(std::string_view url)
{
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : url)
    {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return UINT64_C(0x4355000000000000) | (hash & UINT64_C(0x0000ffffffffffff));
}

} // namespace iptv
