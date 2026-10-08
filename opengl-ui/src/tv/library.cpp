// ProsperoTV - Saved sources, category visibility and favorite folders.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv/library.hpp"

#include "iptv_catalog.h"
#include "iptv_xtream.h"

#include <sqlite3.h>
#include <algorithm>

namespace ptv
{
namespace
{
class Statement
{
  public:
    Statement(sqlite3 *db, const char *sql)
    {
        if (db != nullptr && sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    }
    ~Statement()
    {
        sqlite3_finalize(stmt);
    }
    bool text(int at, std::string_view value)
    {
        return stmt != nullptr &&
               sqlite3_bind_text(stmt, at, value.data(), static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT) == SQLITE_OK;
    }
    bool integer(int at, std::int64_t value)
    {
        return stmt != nullptr && sqlite3_bind_int64(stmt, at, value) == SQLITE_OK;
    }
    bool done()
    {
        return stmt != nullptr && sqlite3_step(stmt) == SQLITE_DONE;
    }
    bool row()
    {
        return stmt != nullptr && sqlite3_step(stmt) == SQLITE_ROW;
    }
    std::string string(int at) const
    {
        const auto *value = sqlite3_column_text(stmt, at);
        return value == nullptr ? std::string() : reinterpret_cast<const char *>(value);
    }
    sqlite3_stmt *stmt = nullptr;
};

bool valid_text(std::string_view text, std::size_t max)
{
    return !text.empty() && text.size() <= max &&
           std::none_of(text.begin(), text.end(),
                        [](unsigned char c) { return c < 32 || c == 127; });
}

} // namespace

bool valid_source(const SavedSource &source)
{
    if (source.id < 0 || source.kind < 0 || source.kind > 5 || !valid_text(source.name, 128) ||
        source.schedule < RefreshSchedule::daily || source.schedule > RefreshSchedule::manual)
        return false;
    if (source.kind == 0)
        return source.id == 1;
    // An original source slot may remain unconfigured after migration.
    if (source.kind <= 3 && source.id == source.kind + 1 && source.url.empty())
        return source.username.empty() && source.password.empty() && source.mac.empty();
    std::string normalized;
    if (!iptv::CanonicalizeStreamUrl(source.url, &normalized) || source.url.size() > 4096)
        return false;
    if (source.kind == 2)
        return iptv::ValidateXtreamCredentials({source.url, source.username, source.password});
    if (source.kind == 5)
        return (source.username.empty() ? source.password.empty()
                                        : valid_text(source.username, 255) &&
                                              source.username.find(':') == std::string::npos) &&
               (source.password.empty() || valid_text(source.password, 255));
    if (source.kind == 3)
    {
        if (source.mac.size() != 17)
            return false;
        for (std::size_t i = 0; i < source.mac.size(); ++i)
        {
            const char c = source.mac[i];
            if (i % 3 == 2
                    ? c != ':'
                    : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
    }
    return true;
}
bool refresh_due(RefreshSchedule schedule, std::uint64_t saved, std::uint64_t now)
{
    if (saved == 0)
        return true;
    if (schedule == RefreshSchedule::manual || now == 0 || now < saved)
        return false;
    const std::uint64_t interval = schedule == RefreshSchedule::weekly ? 7u * 86400u : 86400u;
    return now - saved >= interval;
}

const char *schedule_name(RefreshSchedule schedule)
{
    switch (schedule)
    {
    case RefreshSchedule::weekly:
        return "Weekly";
    case RefreshSchedule::manual:
        return "Only when asked";
    default:
        return "Daily";
    }
}

Library::~Library()
{
    sqlite3_close(db_);
}

bool Library::open(const std::string &path)
{
    if (db_ != nullptr)
        return true;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK)
    {
        sqlite3_close(db);
        return false;
    }
    constexpr const char *schema =
        "PRAGMA foreign_keys=ON; BEGIN;"
        "CREATE TABLE IF NOT EXISTS sources (id INTEGER PRIMARY KEY,kind INTEGER NOT NULL,"
        "name TEXT NOT NULL,url TEXT NOT NULL,username TEXT NOT NULL,password TEXT NOT NULL,"
        "mac TEXT NOT NULL,schedule INTEGER NOT NULL CHECK(schedule BETWEEN 0 AND 2));"
        "CREATE TABLE IF NOT EXISTS selection (singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
        "source INTEGER REFERENCES sources(id) ON DELETE SET NULL);"
        "CREATE TABLE IF NOT EXISTS last_channel (singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
        "source INTEGER REFERENCES sources(id) ON DELETE SET NULL,channel TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS hidden_categories (source TEXT NOT NULL,category TEXT NOT NULL,"
        "PRIMARY KEY(source,category));"
        "CREATE TABLE IF NOT EXISTS folders (name TEXT PRIMARY KEY);"
        "CREATE TABLE IF NOT EXISTS folder_channels (folder TEXT NOT NULL REFERENCES folders(name)"
        " ON DELETE CASCADE ON UPDATE CASCADE,channel TEXT NOT NULL,PRIMARY KEY(folder,channel));"
        "COMMIT;";
    if (sqlite3_exec(db, schema, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        sqlite3_close(db);
        return false;
    }
    db_ = db;
    return true;
}

std::vector<SavedSource> Library::sources() const
{
    std::vector<SavedSource> result;
    Statement query(
        db_, "SELECT id,kind,name,url,username,password,mac,schedule FROM sources ORDER BY id");
    while (query.row())
    {
        SavedSource source{sqlite3_column_int64(query.stmt, 0),
                           sqlite3_column_int(query.stmt, 1),
                           query.string(2),
                           query.string(3),
                           query.string(4),
                           query.string(5),
                           query.string(6),
                           static_cast<RefreshSchedule>(sqlite3_column_int(query.stmt, 7))};
        if (valid_source(source))
            result.push_back(std::move(source));
    }
    return result;
}

bool Library::save_source(SavedSource *source)
{
    if (source == nullptr || !valid_source(*source))
        return false;
    Statement query(
        db_,
        "INSERT INTO sources VALUES(NULLIF(?1,0),?2,?3,?4,?5,?6,?7,?8)"
        " ON CONFLICT(id) DO UPDATE SET kind=excluded.kind,name=excluded.name,url=excluded.url,"
        "username=excluded.username,password=excluded.password,mac=excluded.mac,schedule=excluded."
        "schedule");
    const bool saved = query.integer(1, source->id) && query.integer(2, source->kind) &&
                       query.text(3, source->name) && query.text(4, source->url) &&
                       query.text(5, source->username) && query.text(6, source->password) &&
                       query.text(7, source->mac) &&
                       query.integer(8, static_cast<int>(source->schedule)) && query.done();
    if (saved && source->id == 0)
        source->id = sqlite3_last_insert_rowid(db_);
    return saved;
}

bool Library::remove_source(std::int64_t id)
{
    if (id <= 4)
        return false;
    Statement query(db_, "DELETE FROM sources WHERE id=?1");
    return query.integer(1, id) && query.done();
}

std::int64_t Library::selected_source() const
{
    Statement query(db_, "SELECT source FROM selection WHERE singleton=1");
    return query.row() ? sqlite3_column_int64(query.stmt, 0) : 0;
}

bool Library::select_source(std::int64_t id)
{
    Statement query(db_, "INSERT INTO selection VALUES(1,?1) ON CONFLICT(singleton) DO UPDATE SET "
                         "source=excluded.source");
    return query.integer(1, id) && query.done();
}

bool Library::remember_channel(std::int64_t source, std::string_view channel)
{
    if (!valid_text(channel, 256))
        return false;
    Statement query(db_, "INSERT INTO last_channel VALUES(1,?1,?2) ON CONFLICT(singleton)"
                         " DO UPDATE SET source=excluded.source,channel=excluded.channel");
    return query.integer(1, source) && query.text(2, channel) && query.done();
}

std::pair<std::int64_t, std::string> Library::last_channel() const
{
    Statement query(db_, "SELECT source,channel FROM last_channel WHERE singleton=1");
    if (!query.row())
        return {};
    return {sqlite3_column_int64(query.stmt, 0), query.string(1)};
}

std::unordered_set<std::string> Library::hidden_categories(std::uint64_t source) const
{
    std::unordered_set<std::string> result;
    Statement query(db_, "SELECT category FROM hidden_categories WHERE source=?1");
    if (query.text(1, std::to_string(source)))
        while (query.row())
            result.insert(query.string(0));
    return result;
}

bool Library::hide_category(std::uint64_t source, std::string_view category, bool hidden)
{
    if (!valid_text(category, iptv::kDefaultMaxFieldBytes))
        return false;
    Statement query(db_, hidden ? "INSERT OR IGNORE INTO hidden_categories VALUES(?1,?2)"
                                : "DELETE FROM hidden_categories WHERE source=?1 AND category=?2");
    return query.text(1, std::to_string(source)) && query.text(2, category) && query.done();
}

std::vector<std::string> Library::folders() const
{
    std::vector<std::string> result;
    Statement query(db_, "SELECT name FROM folders ORDER BY name COLLATE NOCASE,name");
    while (query.row())
        result.push_back(query.string(0));
    return result;
}

bool Library::add_folder(std::string_view name)
{
    if (!valid_text(name, 128))
        return false;
    Statement query(db_, "INSERT INTO folders VALUES(?1)");
    return query.text(1, name) && query.done();
}

bool Library::rename_folder(std::string_view name, std::string_view replacement)
{
    if (!valid_text(replacement, 128))
        return false;
    Statement query(db_, "UPDATE folders SET name=?2 WHERE name=?1");
    return query.text(1, name) && query.text(2, replacement) && query.done() &&
           sqlite3_changes(db_) == 1;
}

bool Library::remove_folder(std::string_view name)
{
    Statement query(db_, "DELETE FROM folders WHERE name=?1");
    return query.text(1, name) && query.done();
}

std::unordered_set<std::string> Library::folder_channels(std::string_view folder) const
{
    std::unordered_set<std::string> result;
    Statement query(db_, "SELECT channel FROM folder_channels WHERE folder=?1");
    if (query.text(1, folder))
        while (query.row())
            result.insert(query.string(0));
    return result;
}

bool Library::put_in_folder(std::string_view folder, std::string_view channel, bool included)
{
    if (!valid_text(channel, 256))
        return false;
    Statement query(db_, included ? "INSERT OR IGNORE INTO folder_channels VALUES(?1,?2)"
                                  : "DELETE FROM folder_channels WHERE folder=?1 AND channel=?2");
    return query.text(1, folder) && query.text(2, channel) && query.done();
}

bool Library::remove_from_folders(std::string_view channel)
{
    Statement query(db_, "DELETE FROM folder_channels WHERE channel=?1");
    return query.text(1, channel) && query.done();
}
} // namespace ptv
