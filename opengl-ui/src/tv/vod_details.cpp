// ProsperoTV - Provider metadata stored beside each on-demand catalog.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/vod.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <map>
namespace ptv
{
std::vector<std::string> vod_genres(std::string_view value)
{
    std::vector<std::string> out;
    while (!value.empty() && out.size() < 32)
    {
        auto end = value.find_first_of(",/;|");
        auto part = value.substr(0, end);
        while (!part.empty() && part.front() == ' ')
            part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ')
            part.remove_suffix(1);
        if (!part.empty() && std::find(out.begin(), out.end(), part) == out.end())
            out.emplace_back(part);
        if (end == std::string_view::npos)
            break;
        value.remove_prefix(end + 1);
    }
    return out;
}
bool save_vod_metadata(const std::string &path, const VodMetadata &metadata)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
    {
        sqlite3_close(db);
        return false;
    }
    bool ok = sqlite3_exec(db,
                           "BEGIN; CREATE TABLE IF NOT EXISTS vod_details(id TEXT PRIMARY KEY,plot "
                           "TEXT,cast_names TEXT,genre TEXT,year TEXT,rating TEXT,poster "
                           "TEXT,complete INTEGER); DELETE FROM vod_details;",
                           nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt *stmt = nullptr;
    ok = ok && sqlite3_prepare_v2(db, "INSERT INTO vod_details VALUES(?1,?2,?3,?4,?5,?6,?7,?8)", -1,
                                  &stmt, nullptr) == SQLITE_OK;
    for (const auto &[id, item] : metadata)
    {
        if (!ok)
            break;
        sqlite3_reset(stmt);
        const std::string *fields[] = {&id,        &item.plot,   &item.cast,  &item.genre,
                                       &item.year, &item.rating, &item.poster};
        for (int i = 0; ok && i < 7; ++i)
            ok = sqlite3_bind_text(stmt, i + 1, fields[i]->data(),
                                   static_cast<int>(fields[i]->size()),
                                   SQLITE_TRANSIENT) == SQLITE_OK;
        ok = ok && sqlite3_bind_int(stmt, 8, item.complete ? 1 : 0) == SQLITE_OK &&
             sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (ok)
        ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ok)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return ok;
}
VodSummary vod_summary(const iptv::Catalog &catalog, std::uint64_t saved_unix)
{
    VodSummary summary;
    summary.valid = true;
    summary.saved_unix = saved_unix;
    summary.source_id = catalog.source_id;
    summary.titles = static_cast<unsigned>(catalog.size());
    std::map<std::string_view, std::size_t> at;
    for (std::size_t i = 0; i < catalog.size(); ++i)
    {
        const auto item = catalog[i];
        auto found = at.find(item.group_title);
        if (found == at.end())
        {
            found = at.emplace(item.group_title, summary.categories.size()).first;
            summary.categories.push_back({std::string(item.group_title), 0, 0});
        }
        auto &category = summary.categories[found->second];
        ++category.titles;
        if (item.adult)
            ++category.adult;
    }
    return summary;
}
bool save_vod_summary(const std::string &path, const VodSummary &summary)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
    {
        sqlite3_close(db);
        return false;
    }
    bool ok = sqlite3_exec(db,
                           "BEGIN; CREATE TABLE IF NOT EXISTS vod_summary(category TEXT PRIMARY "
                           "KEY,titles INTEGER,adult INTEGER); DELETE FROM vod_summary;",
                           nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt *stmt = nullptr;
    ok = ok && sqlite3_prepare_v2(db, "INSERT INTO vod_summary VALUES(?1,?2,?3)", -1, &stmt,
                                  nullptr) == SQLITE_OK;
    for (const auto &category : summary.categories)
    {
        if (!ok)
            break;
        sqlite3_reset(stmt);
        ok =
            sqlite3_bind_text(stmt, 1, category.name.data(), static_cast<int>(category.name.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_int(stmt, 2, static_cast<int>(category.titles)) == SQLITE_OK &&
            sqlite3_bind_int(stmt, 3, static_cast<int>(category.adult)) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (ok)
        ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ok)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return ok;
}
VodSummary load_vod_summary(const std::string &path)
{
    VodSummary summary;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        sqlite3_close(db);
        return summary;
    }
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key,value FROM metadata", -1, &stmt, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const auto *key = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            if (!key)
                continue;
            // The store binds both as 64-bit integers, the source hash through the same cast.
            const auto value = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
            if (std::string_view(key) == "saved_unix")
                summary.saved_unix = value;
            else if (std::string_view(key) == "source_id")
                summary.source_id = value;
        }
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT category,titles,adult FROM vod_summary", -1, &stmt,
                           nullptr) == SQLITE_OK)
    {
        summary.valid = true;
        while (sqlite3_step(stmt) == SQLITE_ROW && summary.categories.size() < 65536)
        {
            const auto *text = sqlite3_column_text(stmt, 0);
            const int count = sqlite3_column_bytes(stmt, 0);
            if (!text || count > 1024)
                continue;
            VodSummary::Category category{
                std::string(reinterpret_cast<const char *>(text), static_cast<std::size_t>(count)),
                static_cast<unsigned>(std::max(0, sqlite3_column_int(stmt, 1))),
                static_cast<unsigned>(std::max(0, sqlite3_column_int(stmt, 2)))};
            category.adult = std::min(category.adult, category.titles);
            summary.titles += category.titles;
            summary.categories.push_back(std::move(category));
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return summary;
}
VodMetadata load_vod_metadata(const std::string &path, const iptv::Catalog &catalog)
{
    VodMetadata result;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) !=
        SQLITE_OK)
    {
        sqlite3_close(db);
        return result;
    }
    result.reserve(catalog.size());
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 16384);
    sqlite3_stmt *stmt = nullptr;
    std::size_t bytes = 0;
    if (sqlite3_prepare_v2(
            db, "SELECT id,plot,cast_names,genre,year,rating,poster,complete FROM vod_details", -1,
            &stmt, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step(stmt) == SQLITE_ROW && result.size() < catalog.size())
        {
            std::string fields[7];
            bool valid = true;
            const int limits[] = {256, 4096, 1024, 256, 4, 16, 4096};
            std::size_t size = 0;
            for (int i = 0; i < 7; ++i)
            {
                const auto *text = sqlite3_column_text(stmt, i);
                const int count = sqlite3_column_bytes(stmt, i);
                if (count > limits[i])
                {
                    valid = false;
                    break;
                }
                if (text)
                    fields[i].assign(reinterpret_cast<const char *>(text), count);
                size += count;
            }
            if (!valid || catalog.Find(fields[0]) == iptv::Catalog::npos)
                continue;
            if (size > 32u * 1024u * 1024u - bytes)
                break;
            bytes += size;
            result.emplace(std::move(fields[0]),
                           VodDetails{std::move(fields[1]), std::move(fields[2]),
                                      std::move(fields[3]), std::move(fields[4]),
                                      std::move(fields[5]), std::move(fields[6]),
                                      sqlite3_column_int(stmt, 7) == 1});
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}
} // namespace ptv
