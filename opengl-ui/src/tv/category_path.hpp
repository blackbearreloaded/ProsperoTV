// ProsperoTV - Provider category paths, without changing their original names.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>

namespace ptv
{
inline std::string_view category_trim(std::string_view path)
{
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
        path.remove_prefix(1);
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t'))
        path.remove_suffix(1);
    return path;
}
// Where the last level of a path starts: at "|", at "::", or at a "/" that
// stands apart from the words ("US / Sports", "US/ Sports"). A slash inside a
// word is part of a name: "24/7", "HD/RAW", "B/R Sports" are one level.
inline std::size_t category_separator(std::string_view path)
{
    for (std::size_t at = path.size(); at-- > 0;)
    {
        const char c = path[at];
        if (c == '|')
            return at;
        if (c == ':' && at > 0 && path[at - 1] == ':')
            return at - 1;
        if (c == '/' && ((at > 0 && (path[at - 1] == ' ' || path[at - 1] == '\t')) ||
                         (at + 1 < path.size() && (path[at + 1] == ' ' || path[at + 1] == '\t'))))
            return at;
    }
    return path.npos;
}
inline std::string_view category_parent(std::string_view path)
{
    const auto at = category_separator(path);
    return at == path.npos ? std::string_view() : category_trim(path.substr(0, at));
}
inline std::string_view category_leaf(std::string_view path)
{
    const auto at = category_separator(path);
    return category_trim(at == path.npos ? path : path.substr(at + (path[at] == ':' ? 2 : 1)));
}
inline bool category_belongs(std::string_view path, std::string_view parent)
{
    parent = category_trim(parent);
    for (path = category_trim(path); !path.empty(); path = category_parent(path))
        if (path == parent)
            return true;
    return false;
}
} // namespace ptv
