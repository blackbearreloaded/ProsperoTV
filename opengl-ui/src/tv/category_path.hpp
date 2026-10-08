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
inline std::size_t category_separator(std::string_view path)
{
    const auto slash = path.find_last_of("/|");
    const auto colon = path.rfind("::");
    return slash == path.npos   ? colon
           : colon == path.npos ? slash
                                : (slash > colon ? slash : colon);
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
