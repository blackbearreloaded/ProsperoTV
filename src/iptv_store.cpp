/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_store.h"

#include <sqlite3.h>

#include <climits>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace iptv
{
namespace
{

constexpr int kSchemaVersion = 1;

StoreStatus MapSqlite(int result)
{
    switch (result)
    {
    case SQLITE_OK:
    case SQLITE_DONE:
    case SQLITE_ROW:
        return StoreStatus::ok;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_SCHEMA:
        return StoreStatus::corrupt;
    case SQLITE_TOOBIG:
    case SQLITE_NOMEM:
    case SQLITE_FULL:
        return StoreStatus::too_large;
    case SQLITE_MISUSE:
    case SQLITE_RANGE:
        return StoreStatus::invalid_argument;
    default:
        return StoreStatus::io_error;
    }
}

std::size_t FileSize(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return std::numeric_limits<std::size_t>::max();
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return std::numeric_limits<std::size_t>::max();
    }
    const long end = std::ftell(file);
    std::fclose(file);
    return end < 0 ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(end);
}

bool Execute(sqlite3 *database, const char *sql)
{
    char *message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    sqlite3_free(message);
    return result == SQLITE_OK;
}

bool BindText(sqlite3_stmt *statement, int index, const std::string &value)
{
    return sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string ReadText(sqlite3_stmt *statement, int column)
{
    const unsigned char *value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    return value && bytes > 0
               ? std::string(reinterpret_cast<const char *>(value), static_cast<std::size_t>(bytes))
               : std::string{};
}

bool Fits(const std::string &value, std::size_t limit)
{
    return value.size() <= limit && value.size() <= static_cast<std::size_t>(INT_MAX);
}

bool ValidChannel(const Channel &channel, const CatalogState &catalog, const StoreLimits &limits)
{
    if (channel.source_id != catalog.source_id || channel.id.empty() || channel.name.empty() ||
        channel.url.empty() || channel.alternate_urls.size() > limits.max_alternate_urls ||
        channel.alternate_group_titles.size() > limits.max_alternate_groups)
    {
        return false;
    }
    const std::string *fields[] = {
        &channel.id,
        &channel.name,
        &channel.tvg_id,
        &channel.tvg_name,
        &channel.tvg_logo,
        &channel.group_title,
        &channel.tvg_country,
        &channel.tvg_language,
        &channel.http_user_agent,
        &channel.http_referrer,
    };
    for (const std::string *field : fields)
    {
        if (!Fits(*field, limits.max_string_bytes))
            return false;
    }
    if (!Fits(channel.url, limits.max_url_bytes))
        return false;
    for (const std::string &url : channel.alternate_urls)
    {
        if (!Fits(url, limits.max_url_bytes))
            return false;
    }
    for (const std::string &group : channel.alternate_group_titles)
    {
        if (!Fits(group, limits.max_string_bytes))
            return false;
    }
    return true;
}

bool Prepare(sqlite3 *database, const char *sql, sqlite3_stmt **statement)
{
    return sqlite3_prepare_v2(database, sql, -1, statement, nullptr) == SQLITE_OK;
}

bool CreateSchema(sqlite3 *database)
{
    return Execute(
        database,
        "PRAGMA journal_mode=MEMORY;"
        "PRAGMA synchronous=OFF;"
        "PRAGMA temp_store=MEMORY;"
        "CREATE TABLE metadata(key TEXT PRIMARY KEY NOT NULL,value INTEGER NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE channels("
        "id TEXT PRIMARY KEY NOT NULL,source_id INTEGER NOT NULL,position INTEGER NOT NULL,"
        "source_line INTEGER NOT NULL,name TEXT NOT NULL,url TEXT NOT NULL,tvg_id TEXT NOT NULL,"
        "tvg_name TEXT NOT NULL,tvg_logo TEXT NOT NULL,group_title TEXT NOT NULL,"
        "tvg_country TEXT NOT NULL,tvg_language TEXT NOT NULL,user_agent TEXT NOT NULL,"
        "referrer TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE UNIQUE INDEX channels_position ON channels(position);"
        "CREATE INDEX channels_name ON channels(name COLLATE NOCASE);"
        "CREATE INDEX channels_group ON channels(group_title COLLATE NOCASE);"
        "CREATE INDEX channels_country ON channels(tvg_country COLLATE NOCASE);"
        "CREATE INDEX channels_language ON channels(tvg_language COLLATE NOCASE);"
        "CREATE TABLE alternate_urls(channel_id TEXT NOT NULL,position INTEGER NOT NULL,"
        "url TEXT NOT NULL,PRIMARY KEY(channel_id,position)) WITHOUT ROWID;"
        "CREATE TABLE alternate_groups(channel_id TEXT NOT NULL,position INTEGER NOT NULL,"
        "value TEXT NOT NULL,PRIMARY KEY(channel_id,position)) WITHOUT ROWID;"
        "PRAGMA user_version=1;");
}

bool InsertCatalog(sqlite3 *database, const CatalogState &catalog, const StoreLimits &limits)
{
    sqlite3_stmt *channel_statement = nullptr;
    sqlite3_stmt *url_statement = nullptr;
    sqlite3_stmt *group_statement = nullptr;
    sqlite3_stmt *meta_statement = nullptr;
    const bool prepared =
        Prepare(database,
                "INSERT INTO channels(id,source_id,position,source_line,name,url,tvg_id,tvg_name,"
                "tvg_logo,group_title,tvg_country,tvg_language,user_agent,referrer)"
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                &channel_statement) &&
        Prepare(database, "INSERT INTO alternate_urls(channel_id,position,url) VALUES(?,?,?)",
                &url_statement) &&
        Prepare(database, "INSERT INTO alternate_groups(channel_id,position,value) VALUES(?,?,?)",
                &group_statement) &&
        Prepare(database, "INSERT INTO metadata(key,value) VALUES(?,?)", &meta_statement);
    if (!prepared)
    {
        sqlite3_finalize(channel_statement);
        sqlite3_finalize(url_statement);
        sqlite3_finalize(group_statement);
        sqlite3_finalize(meta_statement);
        return false;
    }

    const std::time_t saved_time = std::time(nullptr);
    bool ok = BindText(meta_statement, 1, std::string{"source_id"}) &&
              sqlite3_bind_int64(meta_statement, 2,
                                 static_cast<sqlite3_int64>(catalog.source_id)) == SQLITE_OK &&
              sqlite3_step(meta_statement) == SQLITE_DONE;
    if (ok)
    {
        sqlite3_reset(meta_statement);
        sqlite3_clear_bindings(meta_statement);
        ok = BindText(meta_statement, 1, std::string{"saved_unix"}) &&
             sqlite3_bind_int64(meta_statement, 2,
                                saved_time > 0 ? static_cast<sqlite3_int64>(saved_time) : 0) ==
                 SQLITE_OK &&
             sqlite3_step(meta_statement) == SQLITE_DONE;
    }
    for (std::size_t index = 0; ok && index < catalog.channels.size(); ++index)
    {
        const Channel &channel = catalog.channels[index];
        if (!ValidChannel(channel, catalog, limits))
        {
            ok = false;
            break;
        }
        sqlite3_reset(channel_statement);
        sqlite3_clear_bindings(channel_statement);
        ok = BindText(channel_statement, 1, channel.id) &&
             sqlite3_bind_int64(channel_statement, 2,
                                static_cast<sqlite3_int64>(channel.source_id)) == SQLITE_OK &&
             sqlite3_bind_int64(channel_statement, 3, static_cast<sqlite3_int64>(index)) ==
                 SQLITE_OK &&
             sqlite3_bind_int64(channel_statement, 4, channel.source_line) == SQLITE_OK &&
             BindText(channel_statement, 5, channel.name) &&
             BindText(channel_statement, 6, channel.url) &&
             BindText(channel_statement, 7, channel.tvg_id) &&
             BindText(channel_statement, 8, channel.tvg_name) &&
             BindText(channel_statement, 9, channel.tvg_logo) &&
             BindText(channel_statement, 10, channel.group_title) &&
             BindText(channel_statement, 11, channel.tvg_country) &&
             BindText(channel_statement, 12, channel.tvg_language) &&
             BindText(channel_statement, 13, channel.http_user_agent) &&
             BindText(channel_statement, 14, channel.http_referrer) &&
             sqlite3_step(channel_statement) == SQLITE_DONE;

        for (std::size_t alternate = 0; ok && alternate < channel.alternate_urls.size();
             ++alternate)
        {
            sqlite3_reset(url_statement);
            sqlite3_clear_bindings(url_statement);
            ok = BindText(url_statement, 1, channel.id) &&
                 sqlite3_bind_int64(url_statement, 2, static_cast<sqlite3_int64>(alternate)) ==
                     SQLITE_OK &&
                 BindText(url_statement, 3, channel.alternate_urls[alternate]) &&
                 sqlite3_step(url_statement) == SQLITE_DONE;
        }
        for (std::size_t alternate = 0; ok && alternate < channel.alternate_group_titles.size();
             ++alternate)
        {
            sqlite3_reset(group_statement);
            sqlite3_clear_bindings(group_statement);
            ok = BindText(group_statement, 1, channel.id) &&
                 sqlite3_bind_int64(group_statement, 2, static_cast<sqlite3_int64>(alternate)) ==
                     SQLITE_OK &&
                 BindText(group_statement, 3, channel.alternate_group_titles[alternate]) &&
                 sqlite3_step(group_statement) == SQLITE_DONE;
        }
    }

    sqlite3_finalize(channel_statement);
    sqlite3_finalize(url_statement);
    sqlite3_finalize(group_statement);
    sqlite3_finalize(meta_statement);
    return ok;
}

bool QuickCheck(sqlite3 *database)
{
    sqlite3_stmt *statement = nullptr;
    if (!Prepare(database, "PRAGMA quick_check(1)", &statement))
        return false;
    const bool ok = sqlite3_step(statement) == SQLITE_ROW && ReadText(statement, 0) == "ok";
    sqlite3_finalize(statement);
    return ok;
}

void SetReport(StoreReport *report, StoreStatus status, std::size_t records, std::size_t bytes,
               std::uint64_t saved_unix = 0)
{
    if (!report)
        return;
    report->status = status;
    report->records = records;
    report->bytes = bytes;
    report->saved_unix = saved_unix;
}

} // namespace

StoreStatus SaveCatalog(const std::string &path, const CatalogState &catalog,
                        const StoreLimits &limits, StoreReport *report)
{
    SetReport(report, StoreStatus::invalid_argument, 0, 0);
    if (path.empty() || catalog.channels.empty() || catalog.channels.size() > limits.max_channels ||
        limits.max_file_bytes == 0)
    {
        return StoreStatus::invalid_argument;
    }

    const std::string staging = path + ".new";
    const std::string backup = path + ".bak";
    std::remove(staging.c_str());
    sqlite3 *database = nullptr;
    int result = sqlite3_open_v2(staging.c_str(), &database,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
    if (result != SQLITE_OK)
    {
        if (database)
            sqlite3_close_v2(database);
        std::remove(staging.c_str());
        const StoreStatus status = MapSqlite(result);
        SetReport(report, status, 0, 0);
        return status;
    }
    sqlite3_busy_timeout(database, 2000);
    bool ok = CreateSchema(database) && Execute(database, "BEGIN IMMEDIATE") &&
              InsertCatalog(database, catalog, limits) && Execute(database, "COMMIT") &&
              QuickCheck(database);
    if (!ok)
        Execute(database, "ROLLBACK");
    result = ok ? SQLITE_OK : sqlite3_errcode(database);
    sqlite3_close_v2(database);
    if (!ok)
    {
        std::remove(staging.c_str());
        const StoreStatus status = MapSqlite(result);
        SetReport(report, status, 0, 0);
        return status;
    }

    const std::size_t bytes = FileSize(staging);
    if (bytes == std::numeric_limits<std::size_t>::max() || bytes > limits.max_file_bytes)
    {
        std::remove(staging.c_str());
        SetReport(report, StoreStatus::too_large, 0, bytes);
        return StoreStatus::too_large;
    }

    std::remove(backup.c_str());
    const bool had_primary = std::rename(path.c_str(), backup.c_str()) == 0;
    if (std::rename(staging.c_str(), path.c_str()) != 0)
    {
        if (had_primary)
            std::rename(backup.c_str(), path.c_str());
        std::remove(staging.c_str());
        SetReport(report, StoreStatus::io_error, 0, 0);
        return StoreStatus::io_error;
    }
    if (had_primary)
        std::remove(backup.c_str());
    SetReport(report, StoreStatus::ok, catalog.channels.size(), bytes);
    return StoreStatus::ok;
}

static StoreStatus LoadCatalogFile(const std::string &path, CatalogState *catalog,
                                   const StoreLimits &limits, StoreReport *report)
{
    SetReport(report, StoreStatus::invalid_argument, 0, 0);
    if (path.empty() || !catalog)
        return StoreStatus::invalid_argument;
    const std::size_t bytes = FileSize(path);
    if (bytes == std::numeric_limits<std::size_t>::max())
    {
        SetReport(report, StoreStatus::not_found, 0, 0);
        return StoreStatus::not_found;
    }
    if (bytes > limits.max_file_bytes)
    {
        SetReport(report, StoreStatus::too_large, 0, bytes);
        return StoreStatus::too_large;
    }

    sqlite3 *database = nullptr;
    int result = sqlite3_open_v2(path.c_str(), &database,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK)
    {
        if (database)
            sqlite3_close_v2(database);
        const StoreStatus status = MapSqlite(result);
        SetReport(report, status, 0, bytes);
        return status;
    }
    sqlite3_busy_timeout(database, 2000);

    sqlite3_stmt *statement = nullptr;
    bool ok = Prepare(database, "PRAGMA user_version", &statement) &&
              sqlite3_step(statement) == SQLITE_ROW;
    const int version = ok ? sqlite3_column_int(statement, 0) : 0;
    sqlite3_finalize(statement);
    if (!ok || version != kSchemaVersion)
    {
        sqlite3_close_v2(database);
        const StoreStatus status = ok ? StoreStatus::unsupported_version : StoreStatus::corrupt;
        SetReport(report, status, 0, bytes);
        return status;
    }

    CatalogState loaded;
    statement = nullptr;
    ok = Prepare(database, "SELECT value FROM metadata WHERE key='source_id'", &statement) &&
         sqlite3_step(statement) == SQLITE_ROW;
    if (ok)
        loaded.source_id = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);

    std::uint64_t saved_unix = 0;
    statement = nullptr;
    const bool have_saved =
        Prepare(database, "SELECT value FROM metadata WHERE key='saved_unix'", &statement) &&
        sqlite3_step(statement) == SQLITE_ROW;
    if (have_saved)
    {
        const sqlite3_int64 value = sqlite3_column_int64(statement, 0);
        if (value > 0)
            saved_unix = static_cast<std::uint64_t>(value);
    }
    sqlite3_finalize(statement);

    std::size_t count = 0;
    statement = nullptr;
    ok = ok && Prepare(database, "SELECT count(*) FROM channels", &statement) &&
         sqlite3_step(statement) == SQLITE_ROW;
    if (ok)
    {
        const sqlite3_int64 value = sqlite3_column_int64(statement, 0);
        ok = value > 0 && static_cast<std::uint64_t>(value) <= limits.max_channels;
        if (ok)
            count = static_cast<std::size_t>(value);
    }
    sqlite3_finalize(statement);
    if (ok)
        loaded.channels.reserve(count);

    statement = nullptr;
    ok = ok &&
         Prepare(database,
                 "SELECT id,source_id,source_line,name,url,tvg_id,tvg_name,tvg_logo,group_title,"
                 "tvg_country,tvg_language,user_agent,referrer FROM channels ORDER BY position",
                 &statement);
    while (ok && sqlite3_step(statement) == SQLITE_ROW)
    {
        Channel channel;
        channel.id = ReadText(statement, 0);
        channel.source_id = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
        channel.source_line = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 2));
        channel.name = ReadText(statement, 3);
        channel.url = ReadText(statement, 4);
        channel.tvg_id = ReadText(statement, 5);
        channel.tvg_name = ReadText(statement, 6);
        channel.tvg_logo = ReadText(statement, 7);
        channel.group_title = ReadText(statement, 8);
        channel.tvg_country = ReadText(statement, 9);
        channel.tvg_language = ReadText(statement, 10);
        channel.http_user_agent = ReadText(statement, 11);
        channel.http_referrer = ReadText(statement, 12);
        ok = ValidChannel(channel, loaded, limits);
        if (ok)
            loaded.channels.push_back(std::move(channel));
    }
    sqlite3_finalize(statement);
    ok = ok && loaded.channels.size() == count;

    std::unordered_map<std::string, std::size_t> positions;
    if (ok)
    {
        positions.reserve(loaded.channels.size());
        for (std::size_t index = 0; index < loaded.channels.size(); ++index)
            positions.emplace(loaded.channels[index].id, index);
    }
    statement = nullptr;
    ok = ok &&
         Prepare(database, "SELECT channel_id,url FROM alternate_urls ORDER BY channel_id,position",
                 &statement);
    while (ok && sqlite3_step(statement) == SQLITE_ROW)
    {
        const auto found = positions.find(ReadText(statement, 0));
        const std::string value = ReadText(statement, 1);
        ok = found != positions.end() && Fits(value, limits.max_url_bytes) &&
             loaded.channels[found->second].alternate_urls.size() < limits.max_alternate_urls;
        if (ok)
            loaded.channels[found->second].alternate_urls.push_back(value);
    }
    sqlite3_finalize(statement);

    statement = nullptr;
    ok = ok && Prepare(database,
                       "SELECT channel_id,value FROM alternate_groups ORDER BY channel_id,position",
                       &statement);
    while (ok && sqlite3_step(statement) == SQLITE_ROW)
    {
        const auto found = positions.find(ReadText(statement, 0));
        const std::string value = ReadText(statement, 1);
        ok = found != positions.end() && Fits(value, limits.max_string_bytes) &&
             loaded.channels[found->second].alternate_group_titles.size() <
                 limits.max_alternate_groups;
        if (ok)
            loaded.channels[found->second].alternate_group_titles.push_back(value);
    }
    sqlite3_finalize(statement);
    if (ok)
        ok = QuickCheck(database);
    result = ok ? SQLITE_OK : sqlite3_errcode(database);
    sqlite3_close_v2(database);

    if (!ok)
    {
        const StoreStatus status = result == SQLITE_OK ? StoreStatus::corrupt : MapSqlite(result);
        SetReport(report, status, 0, bytes);
        return status;
    }
    *catalog = std::move(loaded);
    SetReport(report, StoreStatus::ok, catalog->channels.size(), bytes, saved_unix);
    return StoreStatus::ok;
}

StoreStatus LoadCatalog(const std::string &path, CatalogState *catalog, const StoreLimits &limits,
                        StoreReport *report)
{
    const StoreStatus primary = LoadCatalogFile(path, catalog, limits, report);
    if (primary == StoreStatus::ok || path.empty() || !catalog)
        return primary;

    CatalogState recovered;
    StoreReport recovered_report;
    const std::string backup = path + ".bak";
    if (LoadCatalogFile(backup, &recovered, limits, &recovered_report) != StoreStatus::ok)
    {
        return primary;
    }

    *catalog = std::move(recovered);
    if (report)
        *report = recovered_report;
    std::remove(path.c_str());
    (void)std::rename(backup.c_str(), path.c_str());
    return StoreStatus::ok;
}

} // namespace iptv
