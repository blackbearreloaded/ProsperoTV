// ProsperoTV - Separate console profiles and one-time legacy migration.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/profiles.hpp"
#include "tv/backup.hpp"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace ptv
{
namespace
{
bool directory(const std::string &path)
{
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        return false;
    struct stat status
    {
    };
    return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

bool read_marker(const std::string &path, std::string &value)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    char bytes[32];
    const auto count = ::read(fd, bytes, sizeof(bytes));
    ::close(fd);
    if (count < 0 || count == static_cast<ssize_t>(sizeof(bytes)))
        return false;
    value.assign(bytes, static_cast<std::size_t>(count));
    return true;
}

// O_EXCL makes the first launch's ownership permanent before any account is copied.
bool write_marker(const std::string &path, std::string_view value)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    const bool ok = ::write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size()) &&
                    fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
}
} // namespace

bool prepare_profile(const std::string &root, const std::string &legacy_config,
                     std::int32_t user_id, ProfilePaths &paths, std::string &error)
{
    paths = {};
    if (user_id < 0)
    {
        error = "The console user could not be identified. Reopen ProsperoTV from your user.";
        return false;
    }
    char identity[16];
    std::snprintf(identity, sizeof(identity), "%08x\n", static_cast<unsigned>(user_id));
    const auto users = root + "/profiles";
    const auto profile = users + '/' + std::string(identity, 8);
    ProfilePaths next{profile + "/config", profile + "/cache", profile + "/logs"};
    if (!directory(root) || !directory(users) || !directory(profile) || !directory(next.config) ||
        !directory(next.cache) || !directory(next.logs))
    {
        error = "Your console profile could not be opened. Check the app's storage.";
        return false;
    }

    const auto owner_path = users + "/legacy-owner-v1.txt";
    std::string owner;
    if (!read_marker(owner_path, owner))
    {
        if (!write_marker(owner_path, identity) || !read_marker(owner_path, owner))
        {
            error = "The owner of the previous settings could not be recorded. No account was "
                    "imported.";
            return false;
        }
    }
    if (owner.size() != 9 || owner.back() != '\n' ||
        std::string_view(owner).substr(0, 8).find_first_not_of("0123456789abcdef") !=
            std::string_view::npos)
    {
        error = "The previous profile record is incomplete. Keep it in place for recovery.";
        return false;
    }
    const auto imported = profile + "/legacy-imported-v1.txt";
    std::string complete;
    if (!read_marker(imported, complete))
    {
        if (owner == identity && !copy_legacy_settings(legacy_config, next.config, error))
            return false;
        if (!write_marker(imported, "1\n"))
        {
            error =
                "Your profile migration could not be completed. The original files are unchanged.";
            return false;
        }
    }
    else if (complete != "1\n")
    {
        error = "Your profile migration record is incomplete. Keep it in place for recovery.";
        return false;
    }
    paths = std::move(next);
    error.clear();
    return true;
}
} // namespace ptv
