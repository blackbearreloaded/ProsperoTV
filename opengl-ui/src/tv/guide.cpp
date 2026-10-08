// ProsperoTV - Bounded XMLTV reader and programme cache.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/guide.hpp"
#include "tv/channel_text.hpp"
#include <expat.h>
#include <sqlite3.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

namespace ptv
{
namespace
{
std::string_view trim(std::string_view text)
{
    constexpr std::string_view whitespace = " \r\n\t";
    const auto first = text.find_first_not_of(whitespace);
    return first == text.npos ? std::string_view()
                              : text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}
int number(std::string_view value)
{
    int result = -1;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() ? result : -1;
}
void replace_all(std::string &value, std::string_view from, std::string_view to)
{
    std::size_t at = 0;
    while ((at = value.find(from, at)) != value.npos)
    {
        value.replace(at, from.size(), to);
        at += to.size();
    }
}
std::string encode(std::string_view text)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    return out;
}
struct Statement
{
    sqlite3_stmt *p = nullptr;
    Statement(sqlite3 *db, const char *sql)
    {
        sqlite3_prepare_v2(db, sql, -1, &p, nullptr);
    }
    ~Statement()
    {
        sqlite3_finalize(p);
    }
    void text(int col, std::string_view value)
    {
        sqlite3_bind_text(p, col, value.empty() ? "" : value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT);
    }
    std::string_view text(int col) const
    {
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(p, col));
        return value
                   ? std::string_view(value, static_cast<std::size_t>(sqlite3_column_bytes(p, col)))
                   : std::string_view();
    }
};
} // namespace

std::int64_t xmltv_time(std::string_view value)
{
    value = trim(value);
    const auto space = value.find(' ');
    const auto digits = value.substr(0, space);
    if (digits.size() != 12 && digits.size() != 14)
        return 0;
    const int year = number(digits.substr(0, 4)), month = number(digits.substr(4, 2)),
              day = number(digits.substr(6, 2));
    const int hour = number(digits.substr(8, 2)), minute = number(digits.substr(10, 2));
    const int second = digits.size() == 14 ? number(digits.substr(12, 2)) : 0;
    const std::chrono::year_month_day date{std::chrono::year(year),
                                           std::chrono::month(static_cast<unsigned>(month)),
                                           std::chrono::day(static_cast<unsigned>(day))};
    if (year < 1970 || year > 9999 || !date.ok() || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59)
        return 0;
    int offset = 0;
    if (space != value.npos)
    {
        const auto zone = trim(value.substr(space));
        if (zone == "UTC" || zone == "GMT" || zone == "Z")
        {
        }
        else if (zone == "BST")
            offset = 3600;
        else if (zone.size() == 5 && (zone[0] == '+' || zone[0] == '-'))
        {
            const int hours = number(zone.substr(1, 2)), minutes = number(zone.substr(3, 2));
            if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59)
                return 0;
            offset = (hours * 60 + minutes) * 60 * (zone[0] == '-' ? -1 : 1);
        }
        else
            return 0;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::sys_days(date).time_since_epoch())
               .count() +
           hour * 3600 + minute * 60 + second - offset;
}

std::string programme_time(std::int64_t value, bool date)
{
    const auto seconds = static_cast<std::time_t>(value);
    const std::tm *parts = std::localtime(&seconds);
    char text[64]{};
    if (parts)
        std::strftime(text, sizeof(text), date ? "%a %d %b  %H:%M" : "%H:%M", parts);
    return text;
}

std::string catchup_url(const iptv::ChannelView &channel, const Programme &p, std::int64_t now)
{
    const int days = number(channel.catchup_days);
    if (p.start <= 0 || p.end <= p.start || p.end > now || days <= 0 || days > 365 ||
        now - p.start > static_cast<std::int64_t>(days) * 86400)
        return {};
    std::string url(channel.catchup_source);
    const auto seconds = static_cast<std::time_t>(p.start);
    const auto *time = std::gmtime(&seconds);
    if (!time)
        return {};
    const std::tm utc = *time;
    if (channel.catchup == "xc")
    {
        // /live/user/pass/id.ts -> /timeshift/user/pass/minutes/YYYY-MM-DD:HH-MM/id.ts
        const auto live = url.find("/live/", url.find("://") + 3);
        const auto last = url.rfind('/');
        if (live == url.npos || last <= live + 6)
            return {};
        char start[32]{};
        std::strftime(start, sizeof(start), "%Y-%m-%d:%H-%M", &utc);
        url = url.substr(0, live) + "/timeshift/" + url.substr(live + 6, last - live - 6) + "/" +
              std::to_string((p.end - p.start + 59) / 60) + "/" + start + url.substr(last);
    }
    else
    {
        if (channel.catchup != "default" && channel.catchup != "append" && !channel.catchup.empty())
            return {};
        if (url.empty())
            return {};
        if (channel.catchup == "append")
            url = std::string(channel.url) + url;
        const std::pair<std::string_view, std::string> values[] = {
            {"${start}", std::to_string(p.start)}, {"{utc}", std::to_string(p.start)},
            {"${end}", std::to_string(p.end)},     {"{utcend}", std::to_string(p.end)},
            {"${now}", std::to_string(now)},       {"${timestamp}", std::to_string(now)},
            {"{lutc}", std::to_string(now)},       {"{duration}", std::to_string(p.end - p.start)},
            {"{catchup-id}", encode(p.catchup_id)}};
        for (const auto &[key, value] : values)
            replace_all(url, key, value);
        for (const char token : std::string_view("YmdHMS"))
        {
            const char format[] = {'%', token, '\0'};
            const char key[] = {'{', token, '}', '\0'};
            char value[16]{};
            std::strftime(value, sizeof(value), format, &utc);
            replace_all(url, key, value);
        }
        // Never hand an unexpanded provider template to the player.
        if (url.find('{') != url.npos || url.find('}') != url.npos)
            return {};
    }
    std::string canonical;
    return iptv::CanonicalizeStreamUrl(url, &canonical) ? canonical : std::string();
}

void Guide::sort()
{
    for (auto &[id, entries] : channels)
    {
        (void)id;
        std::stable_sort(entries.begin(), entries.end(),
                         [](const Programme &a, const Programme &b) { return a.start < b.start; });
        entries.erase(std::unique(entries.begin(), entries.end(),
                                  [](const Programme &a, const Programme &b)
                                  { return a.start == b.start; }),
                      entries.end());
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].end <= entries[i].start && i + 1 < entries.size())
                entries[i].end = entries[i + 1].start;
        // A last programme without a stop remains visible in the grid but is
        // never guessed to be on now, nor offered as a catch-up recording.
    }
}
std::size_t Guide::count() const
{
    std::size_t total = 0;
    for (const auto &[id, entries] : channels)
    {
        (void)id;
        total += entries.size();
    }
    return total;
}
std::span<const Programme> Guide::programmes(std::string_view id) const
{
    const auto at = channels.find(std::string(id));
    return at == channels.end() ? std::span<const Programme>()
                                : std::span<const Programme>(at->second);
}
const Programme *Guide::now(std::string_view id, std::int64_t time) const
{
    const auto entries = programmes(id);
    auto at = std::upper_bound(entries.begin(), entries.end(), time,
                               [](auto t, const Programme &p) { return t < p.start; });
    if (at == entries.begin())
        return nullptr;
    --at;
    return at->start <= time && time < at->end ? &*at : nullptr;
}
const Programme *Guide::next(std::string_view id, std::int64_t time) const
{
    const auto entries = programmes(id);
    const auto at = std::upper_bound(entries.begin(), entries.end(), time,
                                     [](auto t, const Programme &p) { return t < p.start; });
    return at == entries.end() ? nullptr : &*at;
}
bool Guide::matches_now(std::string_view id, std::string_view query, std::int64_t time) const
{
    const auto *p = now(id, time);
    return p && contains_nocase(p->title, query);
}

bool Guide::save(const std::string &path) const
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
    {
        sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 1000);
    bool ok = sqlite3_exec(db,
                           "BEGIN IMMEDIATE;CREATE TABLE IF NOT EXISTS info(source INTEGER,saved "
                           "INTEGER,truncated INTEGER);"
                           "CREATE TABLE IF NOT EXISTS programmes(channel TEXT,start INTEGER,end "
                           "INTEGER,title TEXT,description TEXT,catchup_id TEXT);"
                           "DELETE FROM info;DELETE FROM programmes;",
                           nullptr, nullptr, nullptr) == SQLITE_OK;
    {
        Statement info(db, "INSERT INTO info VALUES(?,?,?)");
        if (ok && info.p)
        {
            sqlite3_bind_int64(info.p, 1, static_cast<sqlite3_int64>(source_id));
            sqlite3_bind_int64(info.p, 2, static_cast<sqlite3_int64>(saved_unix));
            sqlite3_bind_int(info.p, 3, truncated);
            ok = sqlite3_step(info.p) == SQLITE_DONE;
        }
        else
            ok = false;
        Statement row(db, "INSERT INTO programmes VALUES(?,?,?,?,?,?)");
        ok = ok && row.p;
        for (const auto &[id, entries] : channels)
        {
            if (!ok)
                break;
            for (const auto &p : entries)
            {
                row.text(1, id);
                sqlite3_bind_int64(row.p, 2, p.start);
                sqlite3_bind_int64(row.p, 3, p.end);
                row.text(4, p.title);
                row.text(5, p.description);
                row.text(6, p.catchup_id);
                ok = sqlite3_step(row.p) == SQLITE_DONE;
                sqlite3_reset(row.p);
                if (!ok)
                    break;
            }
        }
    }
    if (ok)
        ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ok)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return ok;
}
bool Guide::load(const std::string &path, std::uint64_t expected_source)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        sqlite3_close(db);
        return false;
    }
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 16384);
    Guide read;
    bool ok = false;
    {
        Statement info(db, "SELECT source,saved,truncated FROM info");
        ok = info.p && sqlite3_step(info.p) == SQLITE_ROW;
        if (ok)
        {
            read.source_id = static_cast<std::uint64_t>(sqlite3_column_int64(info.p, 0));
            read.saved_unix = static_cast<std::uint64_t>(sqlite3_column_int64(info.p, 1));
            read.truncated = sqlite3_column_int(info.p, 2) != 0;
            ok = read.source_id == expected_source;
        }
        Statement row(db, "SELECT channel,start,end,title,description,catchup_id FROM programmes");
        ok = ok && row.p;
        std::size_t count = 0, bytes = 0;
        int result = SQLITE_DONE;
        while (ok && (result = sqlite3_step(row.p)) == SQLITE_ROW)
        {
            const auto id = row.text(0), title = row.text(3), desc = row.text(4),
                       catchup = row.text(5);
            bytes += id.size() + title.size() + desc.size() + catchup.size();
            ok = ++count <= kMaxProgrammes && bytes <= kMaxTextBytes && !id.empty() &&
                 id.size() <= 2048 && !title.empty() && title.size() <= 512 &&
                 desc.size() <= 2048 && catchup.size() <= 512;
            if (ok)
                read.channels[std::string(id)].push_back(
                    {sqlite3_column_int64(row.p, 1), sqlite3_column_int64(row.p, 2),
                     std::string(title), std::string(desc), std::string(catchup)});
        }
        ok = ok && result == SQLITE_DONE;
    }
    sqlite3_close(db);
    if (ok)
    {
        read.sort();
        *this = std::move(read);
    }
    return ok;
}

struct XmltvReader::State
{
    XML_Parser parser = nullptr;
    z_stream zip{};
    Guide &guide;
    std::unordered_map<std::string, std::vector<std::string>> ids;
    std::unordered_map<std::string, std::string> names;
    Programme programme;
    std::string xml_channel, field, text, prefix;
    std::int64_t clock;
    std::size_t bytes = 0, text_bytes = 0, count = 0;
    unsigned depth = 0;
    bool checked = false, compressed = false, zip_end = false, invalid = false, root = false;

    State(const iptv::Catalog &catalog, std::int64_t now, Guide &target) : guide(target), clock(now)
    {
        guide.source_id = catalog.source_id;
        for (const auto channel : catalog)
        {
            if (!channel.tvg_id.empty())
                ids[std::string(channel.tvg_id)].emplace_back(channel.id);
            for (const auto name : {channel.tvg_name, channel.name})
                if (!name.empty())
                {
                    auto [at, fresh] = names.try_emplace(std::string(name), channel.id);
                    if (!fresh && at->second != channel.id)
                        at->second.clear();
                }
        }
        count = guide.count();
        for (const auto &[id, entries] : guide.channels)
            for (const auto &p : entries)
                text_bytes +=
                    id.size() + p.title.size() + p.description.size() + p.catchup_id.size();
        parser = XML_ParserCreate(nullptr);
        if (!parser)
        {
            invalid = true;
            return;
        }
        XML_SetUserData(parser, this);
        XML_SetElementHandler(parser, start, end);
        XML_SetCharacterDataHandler(parser, characters);
        XML_SetStartDoctypeDeclHandler(
            parser,
            [](void *user, const char *, const char *, const char *, int subset)
            {
                if (subset)
                    static_cast<State *>(user)->fail();
            });
        XML_SetExternalEntityRefHandler(
            parser, [](XML_Parser, const char *, const char *, const char *, const char *) -> int
            { return XML_STATUS_ERROR; });
        XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER);
    }
    ~State()
    {
        if (parser)
            XML_ParserFree(parser);
        if (compressed)
            inflateEnd(&zip);
    }
    void fail()
    {
        invalid = true;
        if (parser)
            XML_StopParser(parser, XML_FALSE);
    }
    static std::string_view attribute(const char **attrs, std::string_view key)
    {
        for (auto at = attrs; at && *at; at += 2)
            if (key == at[0])
                return at[1];
        return {};
    }
    static void start(void *user, const char *tag, const char **attrs)
    {
        auto &s = *static_cast<State *>(user);
        if (++s.depth > 32)
        {
            s.fail();
            return;
        }
        const std::string_view name(tag);
        if (s.depth == 1)
        {
            s.root = name == "tv";
            if (!s.root)
                s.fail();
        }
        if (s.depth == 2 && (name == "channel" || name == "programme"))
        {
            s.xml_channel = attribute(attrs, name == "channel" ? "id" : "channel").substr(0, 2048);
            s.programme = {};
            if (name == "programme")
            {
                s.programme.start = xmltv_time(attribute(attrs, "start"));
                s.programme.end = xmltv_time(attribute(attrs, "stop"));
                s.programme.catchup_id = attribute(attrs, "catchup-id").substr(0, 512);
            }
        }
        if (s.depth == 3 && (name == "title" || name == "desc" || name == "display-name"))
        {
            s.field = name;
            s.text.clear();
        }
    }
    static void characters(void *user, const char *data, int length)
    {
        auto &s = *static_cast<State *>(user);
        const std::size_t limit = s.field == "desc" ? 2048 : 512;
        if (s.depth == 3 && !s.field.empty() && s.text.size() < limit)
            s.text.append(data, std::min(static_cast<std::size_t>(length), limit - s.text.size()));
    }
    static void end(void *user, const char *tag)
    {
        auto &s = *static_cast<State *>(user);
        const std::string_view name(tag);
        if (s.depth == 3 && s.field == name)
        {
            const auto text = trim(s.text);
            if (name == "title" && s.programme.title.empty())
                s.programme.title = text;
            if (name == "desc" && s.programme.description.empty())
                s.programme.description = text;
            if (name == "display-name" && !s.ids.contains(s.xml_channel))
            {
                const auto found = s.names.find(std::string(text));
                if (found != s.names.end() && !found->second.empty())
                    s.ids[s.xml_channel].push_back(found->second);
            }
            s.field.clear();
        }
        if (s.depth == 2 && name == "programme" && s.programme.start > 0 &&
            !s.programme.title.empty() &&
            (s.clock == 0 || (s.programme.start >= s.clock - 14 * 86400 &&
                              s.programme.start <= s.clock + 14 * 86400)))
        {
            const auto found = s.ids.find(s.xml_channel);
            if (found != s.ids.end())
                for (const auto &id : found->second)
                {
                    const auto bytes = id.size() + s.programme.title.size() +
                                       s.programme.description.size() +
                                       s.programme.catchup_id.size();
                    if (s.count >= Guide::kMaxProgrammes ||
                        s.text_bytes + bytes > Guide::kMaxTextBytes)
                    {
                        s.guide.truncated = true;
                        break;
                    }
                    s.guide.channels[id].push_back(s.programme);
                    ++s.count;
                    s.text_bytes += bytes;
                }
        }
        --s.depth;
    }
    bool xml(std::string_view data)
    {
        bytes += data.size();
        if (bytes > 512u * 1024u * 1024u || data.size() > 1024u * 1024u ||
            XML_Parse(parser, data.data(), static_cast<int>(data.size()), XML_FALSE) !=
                XML_STATUS_OK)
            invalid = true;
        return !invalid;
    }
    bool feed(std::string_view data)
    {
        if (invalid)
            return false;
        if (!checked)
        {
            prefix.append(data);
            if (prefix.size() < 2)
                return true;
            checked = true;
            compressed = static_cast<unsigned char>(prefix[0]) == 0x1f &&
                         static_cast<unsigned char>(prefix[1]) == 0x8b;
            if (compressed && inflateInit2(&zip, 16 + MAX_WBITS) != Z_OK)
            {
                compressed = false;
                invalid = true;
                return false;
            }
            const auto first = std::exchange(prefix, {});
            return feed(first);
        }
        if (!compressed)
            return xml(data);
        if (zip_end)
        {
            invalid = !data.empty();
            return !invalid;
        }
        if (data.size() > 1024u * 1024u)
        {
            invalid = true;
            return false;
        }
        zip.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
        zip.avail_in = static_cast<uInt>(data.size());
        std::array<char, 32768> output{};
        do
        {
            zip.next_out = reinterpret_cast<Bytef *>(output.data());
            zip.avail_out = static_cast<uInt>(output.size());
            const int result = inflate(&zip, Z_NO_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
            {
                invalid = true;
                return false;
            }
            if (!xml({output.data(), output.size() - zip.avail_out}))
                return false;
            if (result == Z_STREAM_END)
            {
                zip_end = true;
                invalid = zip.avail_in != 0;
                return !invalid;
            }
            if (result == Z_BUF_ERROR)
                break;
        } while (zip.avail_in || zip.avail_out == 0);
        return true;
    }
};

XmltvReader::XmltvReader(const iptv::Catalog &catalog, std::int64_t now, Guide &guide)
    : state_(new State(catalog, now, guide))
{
}
XmltvReader::~XmltvReader() = default;
bool XmltvReader::feed(std::string_view bytes)
{
    while (!bytes.empty())
    {
        const auto piece = bytes.substr(0, 65536);
        if (!state_->feed(piece))
            return false;
        bytes.remove_prefix(piece.size());
    }
    return !state_->invalid;
}
bool XmltvReader::finish()
{
    auto &s = *state_;
    const bool ok = !s.invalid && s.root && (!s.compressed || s.zip_end) &&
                    XML_Parse(s.parser, "", 0, XML_TRUE) == XML_STATUS_OK;
    if (ok)
        s.guide.sort();
    return ok;
}
} // namespace ptv
