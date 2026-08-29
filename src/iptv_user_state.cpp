/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_user_state.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#endif

namespace iptv
{
namespace
{

constexpr char kFavoritesMagic[8] = {'I', 'P', 'T', 'V', 'F', 'A', 'V', '1'};
constexpr char kRecentMagic[8] = {'I', 'P', 'T', 'V', 'R', 'E', 'C', '1'};
constexpr char kUserStateMagic[8] = {'I', 'P', 'T', 'V', 'U', 'S', 'R', '1'};
constexpr std::uint32_t kFileVersion = 1u;
constexpr std::size_t kHeaderBytes = 32u;
constexpr std::size_t kHardMaxFavoriteIds = 512u;
constexpr std::size_t kHardMaxRecentChannelIds = 256u;
constexpr std::size_t kHardMaxIdBytes = 512u;
constexpr std::size_t kHardMaxFileBytes = 512u * 1024u;

struct EffectiveLimits
{
    std::size_t max_favorite_ids;
    std::size_t max_recent_channel_ids;
    std::size_t max_id_bytes;
    std::size_t max_file_bytes;
};

enum class FileKind : std::uint8_t
{
    favorites,
    recent,
    combined,
};

struct LoadedLists
{
    std::vector<std::string> favorites;
    std::vector<std::string> recent;
};

EffectiveLimits ClampLimits(const UserStateLimits &limits)
{
    return {
        std::min(limits.max_favorite_ids, kHardMaxFavoriteIds),
        std::min(limits.max_recent_channel_ids, kHardMaxRecentChannelIds),
        std::min(limits.max_id_bytes, kHardMaxIdBytes),
        std::min(limits.max_file_bytes, kHardMaxFileBytes),
    };
}

void PutU32(char *target, std::uint32_t value)
{
    target[0] = static_cast<char>(value & 0xffu);
    target[1] = static_cast<char>((value >> 8) & 0xffu);
    target[2] = static_cast<char>((value >> 16) & 0xffu);
    target[3] = static_cast<char>((value >> 24) & 0xffu);
}

std::uint32_t GetU32(const char *source)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(source[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(source[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(source[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(source[3])) << 24);
}

std::uint32_t Crc32(const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    std::uint32_t crc = 0xffffffffu;
    while (size-- != 0)
    {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
        }
    }
    return crc ^ 0xffffffffu;
}

bool AddSize(std::size_t amount, std::size_t *total)
{
    if (amount > std::numeric_limits<std::size_t>::max() - *total)
    {
        return false;
    }
    *total += amount;
    return true;
}

bool ValidId(std::string_view id, std::size_t max_id_bytes)
{
    return !id.empty() && id.size() <= max_id_bytes && id.find('\0') == std::string_view::npos;
}

bool Contains(const std::vector<std::string> &ids, std::string_view id)
{
    return std::any_of(ids.begin(), ids.end(),
                       [id](const std::string &value) {
                           return value.size() == id.size() &&
                                  value.compare(0, value.size(), id.data(), id.size()) == 0;
                       });
}

bool ValidList(const std::vector<std::string> &ids, std::size_t max_count, std::size_t max_id_bytes)
{
    if (ids.size() > max_count || ids.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        if (!ValidId(ids[i], max_id_bytes) ||
            std::any_of(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(i),
                        [&ids, i](const std::string &other) { return other == ids[i]; }))
        {
            return false;
        }
    }
    return true;
}

bool AddIdSize(const std::vector<std::string> &ids, std::size_t max_id_bytes,
               std::size_t *payload_bytes)
{
    for (const std::string &id : ids)
    {
        if (!ValidId(id, max_id_bytes) || id.size() > std::numeric_limits<std::uint32_t>::max() ||
            !AddSize(4u, payload_bytes) || !AddSize(id.size(), payload_bytes))
        {
            return false;
        }
    }
    return true;
}

void AppendU32(std::vector<unsigned char> *payload, std::uint32_t value)
{
    payload->push_back(static_cast<unsigned char>(value & 0xffu));
    payload->push_back(static_cast<unsigned char>((value >> 8) & 0xffu));
    payload->push_back(static_cast<unsigned char>((value >> 16) & 0xffu));
    payload->push_back(static_cast<unsigned char>((value >> 24) & 0xffu));
}

void AppendIds(std::vector<unsigned char> *payload, const std::vector<std::string> &ids)
{
    for (const std::string &id : ids)
    {
        AppendU32(payload, static_cast<std::uint32_t>(id.size()));
        payload->insert(payload->end(), id.begin(), id.end());
    }
}

UserStateStatus StatusForKind(UserStateStatus status, FileKind kind, std::size_t bytes,
                              std::size_t favorite_count, std::size_t recent_count,
                              UserStateReport *report)
{
    (void)kind;
    if (report != nullptr)
    {
        *report = UserStateReport{status, favorite_count, recent_count, bytes};
    }
    return status;
}

const char *MagicForKind(FileKind kind)
{
    switch (kind)
    {
    case FileKind::favorites:
        return kFavoritesMagic;
    case FileKind::recent:
        return kRecentMagic;
    case FileKind::combined:
        return kUserStateMagic;
    }
    return nullptr;
}

bool ReadExact(FILE *input, void *data, std::size_t size)
{
    return std::fread(data, 1, size, input) == size;
}

UserStateStatus RenameTemp(const std::string &temp_path, const std::string &path)
{
#ifdef _WIN32
    return MoveFileExA(temp_path.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? UserStateStatus::ok
               : UserStateStatus::io_error;
#else
    return std::rename(temp_path.c_str(), path.c_str()) == 0 ? UserStateStatus::ok
                                                             : UserStateStatus::io_error;
#endif
}

UserStateStatus SaveFile(const std::string &path, FileKind kind,
                         const std::vector<std::string> &favorites,
                         const std::vector<std::string> &recent, const EffectiveLimits &limits,
                         UserStateReport *report)
{
    if (path.empty())
    {
        return StatusForKind(UserStateStatus::invalid_argument, kind, 0, 0, 0, report);
    }
    if (!ValidList(favorites, limits.max_favorite_ids, limits.max_id_bytes) ||
        !ValidList(recent, limits.max_recent_channel_ids, limits.max_id_bytes))
    {
        return StatusForKind(UserStateStatus::invalid_argument, kind, 0, 0, 0, report);
    }
    if ((kind == FileKind::favorites && !recent.empty()) ||
        (kind == FileKind::recent && !favorites.empty()))
    {
        return StatusForKind(UserStateStatus::invalid_argument, kind, 0, 0, 0, report);
    }

    std::size_t payload_bytes = 0;
    if (!AddIdSize(favorites, limits.max_id_bytes, &payload_bytes) ||
        !AddIdSize(recent, limits.max_id_bytes, &payload_bytes) ||
        payload_bytes > std::numeric_limits<std::uint32_t>::max() ||
        limits.max_file_bytes < kHeaderBytes ||
        payload_bytes > limits.max_file_bytes - kHeaderBytes)
    {
        return StatusForKind(UserStateStatus::too_large, kind, 0, 0, 0, report);
    }

    std::vector<unsigned char> payload;
    payload.reserve(payload_bytes);
    AppendIds(&payload, favorites);
    AppendIds(&payload, recent);

    std::array<char, kHeaderBytes> header{};
    std::memcpy(header.data(), MagicForKind(kind), 8u);
    PutU32(header.data() + 8, kFileVersion);
    PutU32(header.data() + 12, static_cast<std::uint32_t>(kHeaderBytes));
    PutU32(header.data() + 16, static_cast<std::uint32_t>(favorites.size()));
    PutU32(header.data() + 20, static_cast<std::uint32_t>(recent.size()));
    PutU32(header.data() + 24, static_cast<std::uint32_t>(payload.size()));
    PutU32(header.data() + 28, Crc32(payload.data(), payload.size()));

    const std::size_t file_bytes = kHeaderBytes + payload.size();
    const std::string temp_path = path + ".tmp";
    FILE *output = std::fopen(temp_path.c_str(), "wb");
    if (output == nullptr)
    {
        return StatusForKind(UserStateStatus::io_error, kind, 0, 0, 0, report);
    }
    bool write_ok = std::fwrite(header.data(), 1, header.size(), output) == header.size();
    write_ok = write_ok && std::fwrite(payload.data(), 1, payload.size(), output) == payload.size();
    write_ok = write_ok && std::fflush(output) == 0;
    write_ok = std::fclose(output) == 0 && write_ok;
    if (!write_ok)
    {
        std::remove(temp_path.c_str());
        return StatusForKind(UserStateStatus::io_error, kind, 0, 0, 0, report);
    }

    const UserStateStatus rename_status = RenameTemp(temp_path, path);
    if (rename_status != UserStateStatus::ok)
    {
        std::remove(temp_path.c_str());
        return StatusForKind(rename_status, kind, 0, 0, 0, report);
    }
    return StatusForKind(UserStateStatus::ok, kind, file_bytes, favorites.size(), recent.size(),
                         report);
}

UserStateStatus ParseFile(const std::string &path, FileKind expected_kind,
                          const EffectiveLimits &limits, LoadedLists *loaded,
                          UserStateReport *report)
{
    if (path.empty() || loaded == nullptr)
    {
        return StatusForKind(UserStateStatus::invalid_argument, expected_kind, 0, 0, 0, report);
    }
    FILE *input = std::fopen(path.c_str(), "rb");
    if (input == nullptr)
    {
        return StatusForKind(UserStateStatus::not_found, expected_kind, 0, 0, 0, report);
    }
    if (std::fseek(input, 0, SEEK_END) != 0)
    {
        std::fclose(input);
        return StatusForKind(UserStateStatus::io_error, expected_kind, 0, 0, 0, report);
    }
    const long end = std::ftell(input);
    if (end < 0)
    {
        std::fclose(input);
        return StatusForKind(UserStateStatus::io_error, expected_kind, 0, 0, 0, report);
    }
    const std::size_t file_bytes = static_cast<std::size_t>(end);
    if (file_bytes < kHeaderBytes || file_bytes > limits.max_file_bytes)
    {
        std::fclose(input);
        return StatusForKind(UserStateStatus::too_large, expected_kind, file_bytes, 0, 0, report);
    }
    if (std::fseek(input, 0, SEEK_SET) != 0)
    {
        std::fclose(input);
        return StatusForKind(UserStateStatus::io_error, expected_kind, file_bytes, 0, 0, report);
    }
    std::vector<char> file(file_bytes);
    const bool read_ok = ReadExact(input, file.data(), file.size());
    const bool close_ok = std::fclose(input) == 0;
    if (!read_ok || !close_ok)
    {
        return StatusForKind(UserStateStatus::io_error, expected_kind, file_bytes, 0, 0, report);
    }

    const char *expected_magic = MagicForKind(expected_kind);
    if (std::memcmp(file.data(), expected_magic, 8u) != 0)
    {
        return StatusForKind(UserStateStatus::corrupt, expected_kind, file_bytes, 0, 0, report);
    }
    const std::uint32_t version = GetU32(file.data() + 8);
    if (version != kFileVersion)
    {
        return StatusForKind(UserStateStatus::unsupported_version, expected_kind, file_bytes, 0, 0,
                             report);
    }
    if (GetU32(file.data() + 12) != kHeaderBytes)
    {
        return StatusForKind(UserStateStatus::corrupt, expected_kind, file_bytes, 0, 0, report);
    }

    const std::uint32_t favorite_count = GetU32(file.data() + 16);
    const std::uint32_t recent_count = GetU32(file.data() + 20);
    const std::uint32_t payload_bytes = GetU32(file.data() + 24);
    if ((expected_kind == FileKind::favorites &&
         (recent_count != 0 || favorite_count > limits.max_favorite_ids)) ||
        (expected_kind == FileKind::recent &&
         (favorite_count != 0 || recent_count > limits.max_recent_channel_ids)) ||
        (expected_kind == FileKind::combined && (favorite_count > limits.max_favorite_ids ||
                                                 recent_count > limits.max_recent_channel_ids)) ||
        static_cast<std::uint64_t>(payload_bytes) != file_bytes - kHeaderBytes ||
        Crc32(file.data() + kHeaderBytes, payload_bytes) != GetU32(file.data() + 28))
    {
        return StatusForKind(UserStateStatus::corrupt, expected_kind, file_bytes, 0, 0, report);
    }

    LoadedLists candidate;
    candidate.favorites.reserve(favorite_count);
    candidate.recent.reserve(recent_count);
    const unsigned char *payload =
        reinterpret_cast<const unsigned char *>(file.data() + kHeaderBytes);
    std::size_t offset = 0;
    const auto read_list =
        [&payload, payload_bytes, &offset, &limits](std::uint32_t count, std::size_t max_count,
                                                    std::vector<std::string> *ids)
    {
        if (count > max_count)
            return false;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (payload_bytes - offset < 4u)
                return false;
            const std::uint32_t size = static_cast<std::uint32_t>(payload[offset]) |
                                       (static_cast<std::uint32_t>(payload[offset + 1]) << 8) |
                                       (static_cast<std::uint32_t>(payload[offset + 2]) << 16) |
                                       (static_cast<std::uint32_t>(payload[offset + 3]) << 24);
            offset += 4u;
            if (size > limits.max_id_bytes || size > payload_bytes - offset || size == 0u)
            {
                return false;
            }
            const std::string_view id(reinterpret_cast<const char *>(payload + offset), size);
            if (!ValidId(id, limits.max_id_bytes) || Contains(*ids, id))
                return false;
            ids->emplace_back(id);
            offset += size;
        }
        return true;
    };

    if (!read_list(favorite_count, limits.max_favorite_ids, &candidate.favorites) ||
        !read_list(recent_count, limits.max_recent_channel_ids, &candidate.recent) ||
        offset != payload_bytes)
    {
        return StatusForKind(UserStateStatus::corrupt, expected_kind, file_bytes, 0, 0, report);
    }
    *loaded = std::move(candidate);
    return StatusForKind(UserStateStatus::ok, expected_kind, file_bytes, favorite_count,
                         recent_count, report);
}

UserStateStatus LoadList(const std::string &path, FileKind kind, const EffectiveLimits &limits,
                         std::vector<std::string> *ids, UserStateReport *report)
{
    if (ids == nullptr)
    {
        return StatusForKind(UserStateStatus::invalid_argument, kind, 0, 0, 0, report);
    }
    LoadedLists loaded;
    const UserStateStatus status = ParseFile(path, kind, limits, &loaded, report);
    if (status == UserStateStatus::ok)
    {
        *ids = kind == FileKind::favorites ? std::move(loaded.favorites) : std::move(loaded.recent);
    }
    return status;
}

} // namespace

UserStateStatus SaveFavorites(const std::string &path, const std::vector<std::string> &favorite_ids,
                              const UserStateLimits &limits, UserStateReport *report)
{
    return SaveFile(path, FileKind::favorites, favorite_ids, {}, ClampLimits(limits), report);
}

UserStateStatus LoadFavorites(const std::string &path, std::vector<std::string> *favorite_ids,
                              const UserStateLimits &limits, UserStateReport *report)
{
    return LoadList(path, FileKind::favorites, ClampLimits(limits), favorite_ids, report);
}

UserStateStatus SaveRecentChannelIds(const std::string &path,
                                     const std::vector<std::string> &recent_channel_ids,
                                     const UserStateLimits &limits, UserStateReport *report)
{
    return SaveFile(path, FileKind::recent, {}, recent_channel_ids, ClampLimits(limits), report);
}

UserStateStatus LoadRecentChannelIds(const std::string &path,
                                     std::vector<std::string> *recent_channel_ids,
                                     const UserStateLimits &limits, UserStateReport *report)
{
    return LoadList(path, FileKind::recent, ClampLimits(limits), recent_channel_ids, report);
}

UserStateStatus SaveUserState(const std::string &favorites_path,
                              const std::string &recent_channel_ids_path, const UserState &state,
                              const UserStateLimits &limits, UserStateReport *report)
{
    UserStateReport first;
    const UserStateStatus first_status =
        SaveFavorites(favorites_path, state.favorite_ids, limits, &first);
    if (first_status != UserStateStatus::ok)
    {
        if (report != nullptr)
            *report = first;
        return first_status;
    }
    UserStateReport second;
    const UserStateStatus second_status =
        SaveRecentChannelIds(recent_channel_ids_path, state.recent_channel_ids, limits, &second);
    if (second_status != UserStateStatus::ok)
    {
        if (report != nullptr)
        {
            *report = second;
            report->favorite_ids = first.favorite_ids;
            report->bytes += first.bytes;
        }
        return second_status;
    }
    if (report != nullptr)
    {
        *report = UserStateReport{UserStateStatus::ok, first.favorite_ids,
                                  second.recent_channel_ids, first.bytes + second.bytes};
    }
    return UserStateStatus::ok;
}

UserStateStatus LoadUserState(const std::string &favorites_path,
                              const std::string &recent_channel_ids_path, UserState *state,
                              const UserStateLimits &limits, UserStateReport *report)
{
    if (state == nullptr)
    {
        return StatusForKind(UserStateStatus::invalid_argument, FileKind::combined, 0, 0, 0,
                             report);
    }
    LoadedLists favorites;
    UserStateReport first;
    const UserStateStatus first_status =
        ParseFile(favorites_path, FileKind::favorites, ClampLimits(limits), &favorites, &first);
    UserStateReport second;
    LoadedLists recent;
    const UserStateStatus second_status =
        ParseFile(recent_channel_ids_path, FileKind::recent, ClampLimits(limits), &recent, &second);
    if (first_status == UserStateStatus::ok)
        state->favorite_ids = std::move(favorites.favorites);
    if (second_status == UserStateStatus::ok)
        state->recent_channel_ids = std::move(recent.recent);

    const UserStateStatus status =
        first_status != UserStateStatus::ok ? first_status : second_status;
    if (report != nullptr)
    {
        *report = UserStateReport{status, state->favorite_ids.size(),
                                  state->recent_channel_ids.size(), first.bytes + second.bytes};
    }
    return status;
}

UserStateStatus SaveUserState(const std::string &path, const UserState &state,
                              const UserStateLimits &limits, UserStateReport *report)
{
    return SaveFile(path, FileKind::combined, state.favorite_ids, state.recent_channel_ids,
                    ClampLimits(limits), report);
}

UserStateStatus LoadUserState(const std::string &path, UserState *state,
                              const UserStateLimits &limits, UserStateReport *report)
{
    if (state == nullptr)
    {
        return StatusForKind(UserStateStatus::invalid_argument, FileKind::combined, 0, 0, 0,
                             report);
    }
    LoadedLists loaded;
    const UserStateStatus status =
        ParseFile(path, FileKind::combined, ClampLimits(limits), &loaded, report);
    if (status == UserStateStatus::ok)
    {
        state->favorite_ids = std::move(loaded.favorites);
        state->recent_channel_ids = std::move(loaded.recent);
    }
    return status;
}

UserStateStatus SaveUserState(const UserState &state, const UserStateLimits &limits,
                              UserStateReport *report)
{
    return SaveUserState(kDefaultFavoritesPath, kDefaultRecentChannelIdsPath, state, limits,
                         report);
}

UserStateStatus LoadUserState(UserState *state, const UserStateLimits &limits,
                              UserStateReport *report)
{
    return LoadUserState(kDefaultFavoritesPath, kDefaultRecentChannelIdsPath, state, limits,
                         report);
}

bool IsFavorite(const UserState &state, std::string_view channel_id)
{
    return Contains(state.favorite_ids, channel_id);
}

bool IsRecentChannel(const UserState &state, std::string_view channel_id)
{
    return Contains(state.recent_channel_ids, channel_id);
}

bool AddFavorite(UserState *state, std::string_view channel_id, const UserStateLimits &limits)
{
    if (state == nullptr || !ValidId(channel_id, ClampLimits(limits).max_id_bytes) ||
        IsFavorite(*state, channel_id) ||
        state->favorite_ids.size() >= ClampLimits(limits).max_favorite_ids)
    {
        return false;
    }
    state->favorite_ids.emplace_back(channel_id);
    return true;
}

bool RemoveFavorite(UserState *state, std::string_view channel_id)
{
    if (state == nullptr)
        return false;
    const auto found = std::find_if(state->favorite_ids.begin(), state->favorite_ids.end(),
                                    [channel_id](const std::string &value)
                                    {
                                        return value.size() == channel_id.size() &&
                                               value.compare(0, value.size(), channel_id.data(),
                                                             channel_id.size()) == 0;
                                    });
    if (found == state->favorite_ids.end())
        return false;
    state->favorite_ids.erase(found);
    return true;
}

bool ToggleFavorite(UserState *state, std::string_view channel_id, const UserStateLimits &limits)
{
    if (state == nullptr || !ValidId(channel_id, ClampLimits(limits).max_id_bytes))
        return false;
    if (IsFavorite(*state, channel_id))
    {
        RemoveFavorite(state, channel_id);
        return false;
    }
    return AddFavorite(state, channel_id, limits);
}

bool AddRecentChannel(UserState *state, std::string_view channel_id, const UserStateLimits &limits)
{
    const EffectiveLimits bounded = ClampLimits(limits);
    if (state == nullptr || !ValidId(channel_id, bounded.max_id_bytes) ||
        bounded.max_recent_channel_ids == 0u)
    {
        return false;
    }
    const auto found = std::find_if(
        state->recent_channel_ids.begin(), state->recent_channel_ids.end(),
        [channel_id](const std::string &value)
        {
            return value.size() == channel_id.size() &&
                   value.compare(0, value.size(), channel_id.data(), channel_id.size()) == 0;
        });
    if (found != state->recent_channel_ids.end())
        state->recent_channel_ids.erase(found);
    state->recent_channel_ids.insert(state->recent_channel_ids.begin(), std::string(channel_id));
    while (state->recent_channel_ids.size() > bounded.max_recent_channel_ids)
    {
        state->recent_channel_ids.pop_back();
    }
    return true;
}

bool RemoveRecentChannel(UserState *state, std::string_view channel_id)
{
    if (state == nullptr)
        return false;
    const auto found = std::find_if(
        state->recent_channel_ids.begin(), state->recent_channel_ids.end(),
        [channel_id](const std::string &value)
        {
            return value.size() == channel_id.size() &&
                   value.compare(0, value.size(), channel_id.data(), channel_id.size()) == 0;
        });
    if (found == state->recent_channel_ids.end())
        return false;
    state->recent_channel_ids.erase(found);
    return true;
}

} // namespace iptv
