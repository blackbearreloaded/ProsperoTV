/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
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
#include <string_view>
#include <utility>

namespace iptv
{
namespace
{

// The version written, and the oldest one still read.
constexpr int kSchemaVersion = 4;
constexpr int kOldestSchemaVersion = 1;
constexpr int kPlaybackSchemaVersion = 1;

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

// A catalog's text stays where it is until the row is written: no copy is made.
bool BindCatalogText(sqlite3_stmt *statement, int index, std::string_view value)
{
    return sqlite3_bind_text(statement, index, value.data() != nullptr ? value.data() : "",
                             static_cast<int>(value.size()), SQLITE_STATIC) == SQLITE_OK;
}

std::string ReadText(sqlite3_stmt *statement, int column)
{
    const unsigned char *value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    return value && bytes > 0
               ? std::string(reinterpret_cast<const char *>(value), static_cast<std::size_t>(bytes))
               : std::string{};
}

// The column as it lies in the row: good until the statement steps again.
std::string_view ColumnText(sqlite3_stmt *statement, int column)
{
    const unsigned char *value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    return value && bytes > 0 ? std::string_view(reinterpret_cast<const char *>(value),
                                                 static_cast<std::size_t>(bytes))
                              : std::string_view();
}

bool Fits(std::string_view value, std::size_t limit)
{
    return value.size() <= limit && value.size() <= static_cast<std::size_t>(INT_MAX);
}

bool ValidChannel(const ChannelView &channel, const StoreLimits &limits)
{
    if (channel.id.empty() || channel.name.empty() || channel.url.empty())
        return false;
    const std::string_view fields[] = {
        channel.id,
        channel.name,
        channel.tvg_id,
        channel.tvg_name,
        channel.tvg_logo,
        channel.group_title,
        channel.tvg_country,
        channel.tvg_language,
        channel.http_user_agent,
        channel.http_referrer,
        channel.catchup,
        channel.catchup_source,
        channel.catchup_days,
        channel.portal_command,
    };
    for (const std::string_view field : fields)
    {
        if (!Fits(field, limits.max_string_bytes))
            return false;
    }
    return Fits(channel.url, limits.max_url_bytes);
}

bool Prepare(sqlite3 *database, const char *sql, sqlite3_stmt **statement)
{
    return sqlite3_prepare_v2(database, sql, -1, statement, nullptr) == SQLITE_OK;
}

// Version 2: a channel's place in the list is its row, so the rows are written
// and read in order with no index beside them, and a channel's other addresses
// and categories name it by that place. Version 1 keyed every table by the
// channel's id and kept five indexes nothing read; it cost twice the space and
// several times the writing. Files of version 1 are still read.
bool CreateSchema(sqlite3 *database)
{
    return Execute(
        database,
        "PRAGMA journal_mode=MEMORY;"
        "PRAGMA synchronous=OFF;"
        "PRAGMA temp_store=MEMORY;"
        "CREATE TABLE metadata(key TEXT PRIMARY KEY NOT NULL,value INTEGER NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE channels("
        "position INTEGER PRIMARY KEY,id TEXT NOT NULL,source_line INTEGER NOT NULL,"
        "name TEXT NOT NULL,url TEXT NOT NULL,tvg_id TEXT NOT NULL,"
        "tvg_name TEXT NOT NULL,tvg_logo TEXT NOT NULL,group_title TEXT NOT NULL,"
        "tvg_country TEXT NOT NULL,tvg_language TEXT NOT NULL,user_agent TEXT NOT NULL,"
        "referrer TEXT NOT NULL,catchup TEXT NOT NULL,catchup_source TEXT NOT NULL,catchup_days "
        "TEXT NOT NULL,portal_command TEXT NOT NULL);"
        "CREATE TABLE guide_urls(url TEXT NOT NULL);"
        "CREATE TABLE alternate_urls(channel INTEGER NOT NULL,position INTEGER NOT NULL,"
        "url TEXT NOT NULL,PRIMARY KEY(channel,position)) WITHOUT ROWID;"
        "CREATE TABLE alternate_groups(channel INTEGER NOT NULL,position INTEGER NOT NULL,"
        "value TEXT NOT NULL,PRIMARY KEY(channel,position)) WITHOUT ROWID;"
        "PRAGMA user_version=4;");
}

bool CreatePlaybackSchema(sqlite3 *database)
{
    return Execute(database, "PRAGMA journal_mode=DELETE;"
                             "PRAGMA synchronous=FULL;"
                             "CREATE TABLE IF NOT EXISTS playback_results("
                             "source_id INTEGER NOT NULL,channel_id TEXT NOT NULL,"
                             "playable INTEGER NOT NULL CHECK(playable IN(0,1)),"
                             "result INTEGER NOT NULL,checked_unix INTEGER NOT NULL,"
                             "PRIMARY KEY(source_id,channel_id)) WITHOUT ROWID;"
                             "CREATE INDEX IF NOT EXISTS playback_results_checked "
                             "ON playback_results(checked_unix);"
                             "PRAGMA user_version=1;");
}

bool InsertAlternates(sqlite3_stmt *statement, std::size_t channel, const TextList &values,
                      std::size_t limit_count, std::size_t limit_bytes)
{
    std::size_t position = 0;
    for (const std::string_view value : values)
    {
        if (position >= limit_count || !Fits(value, limit_bytes))
            return false;
        sqlite3_reset(statement);
        if (sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(channel)) != SQLITE_OK ||
            sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(position)) != SQLITE_OK ||
            !BindCatalogText(statement, 3, value) || sqlite3_step(statement) != SQLITE_DONE)
            return false;
        ++position;
    }
    return true;
}

bool InsertCatalog(sqlite3 *database, const Catalog &catalog, const StoreLimits &limits)
{
    sqlite3_stmt *channel_statement = nullptr;
    sqlite3_stmt *url_statement = nullptr;
    sqlite3_stmt *group_statement = nullptr;
    sqlite3_stmt *meta_statement = nullptr;
    const bool prepared =
        Prepare(database,
                "INSERT INTO channels(position,id,source_line,name,url,tvg_id,tvg_name,"
                "tvg_logo,group_title,tvg_country,tvg_language,user_agent,referrer,catchup,catchup_"
                "source,catchup_days,portal_command)"
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                &channel_statement) &&
        Prepare(database, "INSERT INTO alternate_urls(channel,position,url) VALUES(?,?,?)",
                &url_statement) &&
        Prepare(database, "INSERT INTO alternate_groups(channel,position,value) VALUES(?,?,?)",
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
    const std::size_t count = catalog.size();
    for (std::size_t index = 0; ok && index < count; ++index)
    {
        const ChannelView channel = catalog[index];
        if (!ValidChannel(channel, limits))
        {
            ok = false;
            break;
        }
        sqlite3_reset(channel_statement);
        ok = sqlite3_bind_int64(channel_statement, 1, static_cast<sqlite3_int64>(index)) ==
                 SQLITE_OK &&
             BindCatalogText(channel_statement, 2, channel.id) &&
             sqlite3_bind_int64(channel_statement, 3, channel.source_line) == SQLITE_OK &&
             BindCatalogText(channel_statement, 4, channel.name) &&
             BindCatalogText(channel_statement, 5, channel.url) &&
             BindCatalogText(channel_statement, 6, channel.tvg_id) &&
             BindCatalogText(channel_statement, 7, channel.tvg_name) &&
             BindCatalogText(channel_statement, 8, channel.tvg_logo) &&
             BindCatalogText(channel_statement, 9, channel.group_title) &&
             BindCatalogText(channel_statement, 10, channel.tvg_country) &&
             BindCatalogText(channel_statement, 11, channel.tvg_language) &&
             BindCatalogText(channel_statement, 12, channel.http_user_agent) &&
             BindCatalogText(channel_statement, 13, channel.http_referrer) &&
             BindCatalogText(channel_statement, 14, channel.catchup) &&
             BindCatalogText(channel_statement, 15, channel.catchup_source) &&
             BindCatalogText(channel_statement, 16, channel.catchup_days) &&
             BindCatalogText(channel_statement, 17, channel.portal_command) &&
             sqlite3_step(channel_statement) == SQLITE_DONE &&
             InsertAlternates(url_statement, index, channel.alternate_urls,
                              limits.max_alternate_urls, limits.max_url_bytes) &&
             InsertAlternates(group_statement, index, channel.alternate_group_titles,
                              limits.max_alternate_groups, limits.max_string_bytes);
    }

    sqlite3_finalize(channel_statement);
    sqlite3_finalize(url_statement);
    sqlite3_finalize(group_statement);
    sqlite3_finalize(meta_statement);
    sqlite3_stmt *guide_statement = nullptr;
    ok = ok && catalog.guide_urls.size() <= 8 &&
         Prepare(database, "INSERT INTO guide_urls(url) VALUES(?)", &guide_statement);
    for (const auto &url : catalog.guide_urls)
    {
        if (!ok)
            break;
        sqlite3_reset(guide_statement);
        ok = Fits(url, limits.max_url_bytes) && BindText(guide_statement, 1, url) &&
             sqlite3_step(guide_statement) == SQLITE_DONE;
    }
    sqlite3_finalize(guide_statement);
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

// What a failed step says: SQLite's own reason, or that the catalog itself
// could not be stored as it is.
StoreStatus FailureOf(int sqlite_result)
{
    return sqlite_result == SQLITE_OK || sqlite_result == SQLITE_ROW || sqlite_result == SQLITE_DONE
               ? StoreStatus::invalid_argument
               : MapSqlite(sqlite_result);
}

} // namespace

StoreStatus SaveCatalog(const std::string &path, const Catalog &catalog, const StoreLimits &limits,
                        StoreReport *report)
{
    SetReport(report, StoreStatus::invalid_argument, 0, 0);
    if (path.empty() || catalog.empty() || catalog.size() > limits.max_channels ||
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
    const bool ok = CreateSchema(database) && Execute(database, "BEGIN IMMEDIATE") &&
                    InsertCatalog(database, catalog, limits) && Execute(database, "COMMIT") &&
                    QuickCheck(database);
    result = ok ? SQLITE_OK : sqlite3_errcode(database);
    if (!ok)
        Execute(database, "ROLLBACK");
    sqlite3_close_v2(database);
    if (!ok)
    {
        std::remove(staging.c_str());
        const StoreStatus status = FailureOf(result);
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
    SetReport(report, StoreStatus::ok, catalog.size(), bytes);
    return StoreStatus::ok;
}

namespace
{

// A channel's other addresses or categories, from either version of the file:
// version 2 names the channel by its place, version 1 by its id.
bool LoadAlternates(sqlite3 *database, int version, bool urls, const StoreLimits &limits,
                    Catalog *loaded)
{
    const char *sql =
        urls ? (version == 1
                    ? "SELECT channel_id,url FROM alternate_urls ORDER BY channel_id,position"
                    : "SELECT channel,url FROM alternate_urls ORDER BY channel,position")
             : (version == 1
                    ? "SELECT channel_id,value FROM alternate_groups ORDER BY channel_id,position"
                    : "SELECT channel,value FROM alternate_groups ORDER BY channel,position");
    sqlite3_stmt *statement = nullptr;
    bool ok = Prepare(database, sql, &statement);
    while (ok && sqlite3_step(statement) == SQLITE_ROW)
    {
        std::size_t index = Catalog::npos;
        if (version == 1)
        {
            index = loaded->Find(ColumnText(statement, 0));
        }
        else
        {
            const sqlite3_int64 place = sqlite3_column_int64(statement, 0);
            if (place >= 0 && static_cast<std::uint64_t>(place) < loaded->size())
                index = static_cast<std::size_t>(place);
        }
        const std::string_view value = ColumnText(statement, 1);
        if (index == Catalog::npos)
        {
            ok = false;
        }
        else if (urls)
        {
            ok = Fits(value, limits.max_url_bytes) &&
                 (*loaded)[index].alternate_urls.size() < limits.max_alternate_urls &&
                 loaded->AddAlternateUrl(index, value);
        }
        else
        {
            ok = Fits(value, limits.max_string_bytes) &&
                 (*loaded)[index].alternate_group_titles.size() < limits.max_alternate_groups &&
                 loaded->AddAlternateGroup(index, value);
        }
    }
    sqlite3_finalize(statement);
    return ok;
}

} // namespace

static StoreStatus LoadCatalogFile(const std::string &path, Catalog *catalog,
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
    if (!ok || version < kOldestSchemaVersion || version > kSchemaVersion)
    {
        sqlite3_close_v2(database);
        const StoreStatus status = ok ? StoreStatus::unsupported_version : StoreStatus::corrupt;
        SetReport(report, status, 0, bytes);
        return status;
    }

    Catalog loaded;
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

    // Each row goes from SQLite's page into the catalog; nothing is copied
    // on the way.
    statement = nullptr;
    const std::string query =
        "SELECT id,source_line,name,url,tvg_id,tvg_name,tvg_logo,group_title,"
        "tvg_country,tvg_language,user_agent,referrer" +
        std::string(version >= 3 ? ",catchup,catchup_source,catchup_days" : "") +
        std::string(version >= 4 ? ",portal_command" : "") + " FROM channels ORDER BY position";
    ok = ok && Prepare(database, query.c_str(), &statement);
    while (ok && sqlite3_step(statement) == SQLITE_ROW)
    {
        ChannelView channel;
        channel.id = ColumnText(statement, 0);
        channel.source_id = loaded.source_id;
        channel.source_line = static_cast<std::uint32_t>(sqlite3_column_int64(statement, 1));
        channel.name = ColumnText(statement, 2);
        channel.url = ColumnText(statement, 3);
        channel.tvg_id = ColumnText(statement, 4);
        channel.tvg_name = ColumnText(statement, 5);
        channel.tvg_logo = ColumnText(statement, 6);
        channel.group_title = ColumnText(statement, 7);
        channel.tvg_country = ColumnText(statement, 8);
        channel.tvg_language = ColumnText(statement, 9);
        channel.http_user_agent = ColumnText(statement, 10);
        channel.http_referrer = ColumnText(statement, 11);
        if (version >= 3)
        {
            channel.catchup = ColumnText(statement, 12);
            channel.catchup_source = ColumnText(statement, 13);
            channel.catchup_days = ColumnText(statement, 14);
        }
        if (version >= 4)
            channel.portal_command = ColumnText(statement, 15);
        ok = loaded.size() < count && ValidChannel(channel, limits) && loaded.Add(channel);
    }
    sqlite3_finalize(statement);
    ok = ok && loaded.size() == count;
    if (ok && version >= 3)
    {
        statement = nullptr;
        ok = Prepare(database, "SELECT url FROM guide_urls", &statement);
        int step = SQLITE_DONE;
        while (ok && (step = sqlite3_step(statement)) == SQLITE_ROW)
        {
            const auto url = ColumnText(statement, 0);
            ok = loaded.guide_urls.size() < 8 && Fits(url, limits.max_url_bytes);
            if (ok)
                loaded.guide_urls.emplace_back(url);
        }
        ok = ok && step == SQLITE_DONE;
        sqlite3_finalize(statement);
    }
    ok = ok && LoadAlternates(database, version, true, limits, &loaded) &&
         LoadAlternates(database, version, false, limits, &loaded);
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
    SetReport(report, StoreStatus::ok, catalog->size(), bytes, saved_unix);
    return StoreStatus::ok;
}

StoreStatus LoadCatalog(const std::string &path, Catalog *catalog, const StoreLimits &limits,
                        StoreReport *report)
{
    const StoreStatus primary = LoadCatalogFile(path, catalog, limits, report);
    if (primary == StoreStatus::ok || path.empty() || !catalog)
        return primary;

    Catalog recovered;
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

StoreStatus RecordPlaybackResult(const std::string &path, std::uint64_t source_id,
                                 const std::string &channel_id, bool playable, int result)
{
    if (path.empty() || channel_id.empty() || !Fits(channel_id, kDefaultMaxFieldBytes))
        return StoreStatus::invalid_argument;

    sqlite3 *database = nullptr;
    int sqlite_result = sqlite3_open_v2(
        path.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (sqlite_result != SQLITE_OK)
    {
        if (database)
            sqlite3_close_v2(database);
        return MapSqlite(sqlite_result);
    }
    sqlite3_busy_timeout(database, 2000);

    sqlite3_stmt *statement = nullptr;
    const std::time_t now = std::time(nullptr);
    bool ok =
        CreatePlaybackSchema(database) &&
        Prepare(database,
                "INSERT OR REPLACE INTO playback_results("
                "source_id,channel_id,playable,result,checked_unix) VALUES(?,?,?,?,?)",
                &statement) &&
        sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(source_id)) == SQLITE_OK &&
        BindText(statement, 2, channel_id) &&
        sqlite3_bind_int(statement, 3, playable ? 1 : 0) == SQLITE_OK &&
        sqlite3_bind_int(statement, 4, result) == SQLITE_OK &&
        sqlite3_bind_int64(statement, 5, now > 0 ? static_cast<sqlite3_int64>(now) : 0) ==
            SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_DONE;
    sqlite_result = ok ? SQLITE_OK : sqlite3_errcode(database);
    sqlite3_finalize(statement);
    sqlite3_close_v2(database);
    return ok ? StoreStatus::ok : MapSqlite(sqlite_result);
}

StoreStatus LoadPlaybackResults(const std::string &path, std::uint64_t source_id, Catalog *catalog,
                                const StoreLimits &limits)
{
    if (path.empty() || !catalog || catalog->source_id != source_id)
        return StoreStatus::invalid_argument;
    const std::size_t bytes = FileSize(path);
    if (bytes == std::numeric_limits<std::size_t>::max())
        return StoreStatus::not_found;
    if (bytes > limits.max_file_bytes)
        return StoreStatus::too_large;

    sqlite3 *database = nullptr;
    int sqlite_result = sqlite3_open_v2(path.c_str(), &database,
                                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (sqlite_result != SQLITE_OK)
    {
        if (database)
            sqlite3_close_v2(database);
        return MapSqlite(sqlite_result);
    }
    sqlite3_busy_timeout(database, 2000);

    sqlite3_stmt *statement = nullptr;
    bool ok = Prepare(database, "PRAGMA user_version", &statement) &&
              sqlite3_step(statement) == SQLITE_ROW &&
              sqlite3_column_int(statement, 0) == kPlaybackSchemaVersion;
    sqlite3_finalize(statement);

    // The catalog knows where each of its channels is: only the channels
    // someone has played are looked up.
    statement = nullptr;
    ok = ok &&
         Prepare(database,
                 "SELECT channel_id,playable,result,checked_unix FROM playback_results "
                 "WHERE source_id=?",
                 &statement) &&
         sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(source_id)) == SQLITE_OK;
    int step = SQLITE_DONE;
    while (ok && (step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        const std::size_t index = catalog->Find(ColumnText(statement, 0));
        if (index == Catalog::npos)
            continue;
        const sqlite3_int64 checked = sqlite3_column_int64(statement, 3);
        catalog->SetPlayback(index,
                             sqlite3_column_int(statement, 1) != 0 ? PlaybackStatus::playable
                                                                   : PlaybackStatus::failed,
                             sqlite3_column_int(statement, 2),
                             checked > 0 ? static_cast<std::uint64_t>(checked) : 0);
    }
    ok = ok && step == SQLITE_DONE;
    sqlite_result = ok ? SQLITE_OK : sqlite3_errcode(database);
    sqlite3_finalize(statement);
    sqlite3_close_v2(database);
    return ok ? StoreStatus::ok : MapSqlite(sqlite_result);
}

} // namespace iptv
