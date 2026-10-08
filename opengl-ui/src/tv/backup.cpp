// ProsperoTV - Bounded SQLite backup containers with rollback on interrupted restore.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/backup.hpp"
#include <sqlite3.h>
#include <zlib.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace ptv
{
namespace
{
constexpr std::size_t kFileLimit = 32u * 1024u * 1024u;
constexpr std::size_t kTotalLimit = 128u * 1024u * 1024u;
constexpr int kApplication = 0x50545642;
constexpr const char *kJournal = "/prosperotv-restore-journal.sqlite3";
// Pairing tokens and downloaded catalogues belong to this installation.
constexpr std::array kNames = {"prosperotv-interface-v1.txt", "prosperotv-library.sqlite3",
                               "iptv-active-source-v1.txt",   "iptv-custom-source-v1.txt",
                               "prosperotv-xtream-v1.txt",    "iptv-favorites-v1.bin",
                               "iptv-history-v1.bin",         "prosperotv-playback-history.sqlite3",
                               "prosperotv-parental-v1.txt"};
struct File
{
    bool present = false;
    std::string data;
};
using Files = std::array<File, kNames.size()>;

bool regular(const std::string &path, std::size_t limit, bool &exists)
{
    struct stat status
    {
    };
    exists = false;
    if (lstat(path.c_str(), &status) != 0)
        return errno == ENOENT;
    exists = true;
    return S_ISREG(status.st_mode) && status.st_size >= 0 &&
           static_cast<std::uint64_t>(status.st_size) <= limit;
}

bool read_files(const std::string &directory, Files &files)
{
    std::size_t total = 0;
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        const auto path = directory + '/' + kNames[i];
        if (!regular(path, kFileLimit, files[i].present))
            return false;
        if (!files[i].present)
            continue;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
        auto *file = fd < 0 ? nullptr : fdopen(fd, "rb");
        if (!file && fd >= 0)
            ::close(fd);
        if (!file)
            return false;
        char buffer[16384];
        std::size_t count;
        bool valid = true;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        {
            if (files[i].data.size() + count > kFileLimit || total + count > kTotalLimit)
            {
                valid = false;
                break;
            }
            files[i].data.append(buffer, count);
            total += count;
        }
        valid &= std::ferror(file) == 0;
        std::fclose(file);
        if (!valid)
            return false;
    }
    return true;
}

bool replace_file(const std::string &path, const File &file)
{
    bool exists;
    if (!regular(path, kFileLimit, exists))
        return false;
    if (!file.present)
        return !exists || std::remove(path.c_str()) == 0;
    const auto temporary = path + ".restore-tmp";
    if (!regular(temporary, kFileLimit, exists))
        return false;
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    auto *out = fd < 0 ? nullptr : fdopen(fd, "wb");
    if (!out && fd >= 0)
        ::close(fd);
    if (!out)
        return false;
    const bool written =
        std::fwrite(file.data.data(), 1, file.data.size(), out) == file.data.size() &&
        std::fflush(out) == 0 && fsync(fileno(out)) == 0;
    const bool closed = std::fclose(out) == 0;
    if (!written || !closed || std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

bool apply_files(const std::string &directory, const Files &files)
{
    for (std::size_t i = 0; i < files.size(); ++i)
        if (!replace_file(directory + '/' + kNames[i], files[i]))
            return false;
    return true;
}

bool write_archive(const std::string &path, const Files &files)
{
    const auto temporary = path + ".partial";
    bool exists;
    if (!regular(path, kTotalLimit + 1024u * 1024u, exists) ||
        !regular(temporary, kTotalLimit + 1024u * 1024u, exists))
        return false;
    if (exists && std::remove(temporary.c_str()) != 0)
        return false;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(temporary.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK)
    {
        sqlite3_close(db);
        return false;
    }
    bool ok =
        sqlite3_exec(
            db,
            "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA application_id=1347704386;"
            "PRAGMA user_version=1; CREATE TABLE files(name TEXT PRIMARY KEY,present INTEGER,data "
            "BLOB,crc INTEGER); BEGIN;",
            nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt *row = nullptr;
    ok &= sqlite3_prepare_v2(db, "INSERT INTO files VALUES(?1,?2,?3,?4)", -1, &row, nullptr) ==
          SQLITE_OK;
    for (std::size_t i = 0; ok && i < files.size(); ++i)
    {
        const auto &file = files[i];
        sqlite3_reset(row);
        const auto checksum = crc32(0, reinterpret_cast<const Bytef *>(file.data.data()),
                                    static_cast<uInt>(file.data.size()));
        ok = sqlite3_bind_text(row, 1, kNames[i], -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_int(row, 2, file.present) == SQLITE_OK &&
             sqlite3_bind_blob(row, 3, file.data.data(), static_cast<int>(file.data.size()),
                               SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_int64(row, 4, checksum) == SQLITE_OK && sqlite3_step(row) == SQLITE_DONE;
    }
    sqlite3_finalize(row);
    if (ok)
        ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
    ok &= sqlite3_close(db) == SQLITE_OK;
    if (!ok || std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

bool read_archive(const std::string &path, Files &files)
{
    bool exists;
    if (!regular(path, kTotalLimit + 1024u * 1024u, exists) || !exists)
        return false;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        sqlite3_close(db);
        return false;
    }
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, kFileLimit + 1024u);
    sqlite3_exec(db, "PRAGMA trusted_schema=OFF", nullptr, nullptr, nullptr);
    unsigned operations = 0;
    sqlite3_progress_handler(
        db, 1000, [](void *p) -> int { return ++*static_cast<unsigned *>(p) > 10000; },
        &operations);
    sqlite3_stmt *row = nullptr;
    bool ok = sqlite3_prepare_v2(db, "PRAGMA application_id", -1, &row, nullptr) == SQLITE_OK &&
              sqlite3_step(row) == SQLITE_ROW && sqlite3_column_int(row, 0) == kApplication;
    sqlite3_finalize(row);
    row = nullptr;
    ok = ok && sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &row, nullptr) == SQLITE_OK &&
         sqlite3_step(row) == SQLITE_ROW && sqlite3_column_int(row, 0) == 1;
    sqlite3_finalize(row);
    row = nullptr;
    ok = ok && sqlite3_prepare_v2(db, "SELECT name,present,data,crc FROM files", -1, &row,
                                  nullptr) == SQLITE_OK;
    std::array<bool, kNames.size()> seen{};
    std::size_t total = 0, count = 0;
    int step = SQLITE_DONE;
    while (ok && (step = sqlite3_step(row)) == SQLITE_ROW)
    {
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(row, 0));
        std::size_t index = 0;
        while (index < kNames.size() && (!name || std::strcmp(name, kNames[index]) != 0))
            ++index;
        if (index == kNames.size() || seen[index] || sqlite3_column_type(row, 0) != SQLITE_TEXT ||
            sqlite3_column_bytes(row, 0) != static_cast<int>(std::strlen(kNames[index])) ||
            sqlite3_column_type(row, 1) != SQLITE_INTEGER ||
            sqlite3_column_type(row, 2) != SQLITE_BLOB ||
            sqlite3_column_type(row, 3) != SQLITE_INTEGER)
        {
            ok = false;
            break;
        }
        seen[index] = true;
        const int present = sqlite3_column_int(row, 1), bytes = sqlite3_column_bytes(row, 2);
        if ((present != 0 && present != 1) || bytes < 0 ||
            static_cast<std::size_t>(bytes) > kFileLimit || total + bytes > kTotalLimit ||
            (!present && bytes))
        {
            ok = false;
            break;
        }
        const auto *data = static_cast<const Bytef *>(sqlite3_column_blob(row, 2));
        if (crc32(0, data, static_cast<uInt>(bytes)) !=
            static_cast<uLong>(sqlite3_column_int64(row, 3)))
        {
            ok = false;
            break;
        }
        files[index].present = present;
        if (bytes)
            files[index].data.assign(reinterpret_cast<const char *>(data), bytes);
        total += bytes;
        ++count;
    }
    sqlite3_finalize(row);
    sqlite3_close(db);
    return ok && step == SQLITE_DONE && count == kNames.size();
}
} // namespace

std::vector<std::string> usb_drives(const std::string &mount_root)
{
    std::vector<std::string> result;
    for (unsigned i = 0; i < 8; ++i)
    {
        const auto path = mount_root + "/usb" + std::to_string(i);
        struct stat status
        {
        };
        if (lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
            access(path.c_str(), R_OK | X_OK) == 0)
            result.push_back(path);
    }
    return result;
}

bool backup_settings(const std::string &directory, const std::string &archive, std::string &error)
{
    Files files;
    if (!read_files(directory, files) || !write_archive(archive, files))
    {
        error = "The backup could not be written. Check the USB drive and free space.";
        return false;
    }
    error.clear();
    return true;
}

bool recover_settings(const std::string &directory, std::string &error)
{
    const auto journal = directory + kJournal;
    bool exists;
    Files files;
    if (!regular(journal, kTotalLimit + 1024u * 1024u, exists) ||
        (exists && (!read_archive(journal, files) || !apply_files(directory, files) ||
                    std::remove(journal.c_str()) != 0)))
    {
        error = "An interrupted restore could not be recovered. Keep the restore journal and "
                "restart after checking storage.";
        return false;
    }
    error.clear();
    return true;
}

bool restore_settings(const std::string &directory, const std::string &archive, std::string &error)
{
    Files next, before;
    if (!recover_settings(directory, error))
        return false;
    if (!read_archive(archive, next))
    {
        error = "This is not a complete, valid ProsperoTV backup. No settings were changed.";
        return false;
    }
    if (!read_files(directory, before) || !write_archive(directory + kJournal, before))
    {
        error = "The current settings could not be protected. No settings were changed.";
        return false;
    }
    if (!apply_files(directory, next))
    {
        if (recover_settings(directory, error))
            error = "Restore failed. Your previous settings were restored.";
        return false;
    }
    if (std::remove((directory + kJournal).c_str()) != 0)
    {
        error = "Restore could not be committed. Restart to recover your previous settings.";
        return false;
    }
    error.clear();
    return true;
}

namespace
{
std::string sanitize_report(std::string_view receipt)
{
    std::string report = "PROSPEROTV_FAILURE_REPORT_V1\n";
    // Only known numeric decoder/network fields leave the console. Free-form
    // provider responses, channel names, URLs, account details and logs do not.
    constexpr std::array fields = {"version",
                                   "unix_time",
                                   "result",
                                   "attempts",
                                   "stream_state",
                                   "stream_result",
                                   "codec",
                                   "profile",
                                   "level",
                                   "coded",
                                   "visible",
                                   "bit_depth",
                                   "audio_stream_type",
                                   "audio_rate",
                                   "audio_channels",
                                   "direct_elapsed_ms",
                                   "direct_bytes",
                                   "direct_reads",
                                   "direct_read_errors",
                                   "direct_native_error",
                                   "native_state",
                                   "native_result",
                                   "native_cleanup",
                                   "decoded_frames",
                                   "presented_frames",
                                   "audio_decoded_frames",
                                   "native_audio_result",
                                   "stream_cleanup_result",
                                   "player_cleanup_result",
                                   "decoder_output_error",
                                   "decoder_output_reject_flags",
                                   "transport_continuity_errors",
                                   "direct_open_attempts",
                                   "direct_reconnects",
                                   "stream_audio_disabled",
                                   "first_frame_latency_us",
                                   "actual_frame_rate_x100",
                                   "bitrate_kbps"};
    std::array<bool, fields.size()> seen{};
    while (!receipt.empty())
    {
        const auto end = receipt.find('\n');
        const auto line = receipt.substr(0, end);
        const auto equal = line.find('=');
        if (equal != line.npos)
        {
            const auto key = line.substr(0, equal), value = line.substr(equal + 1);
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                if (key != fields[i] || seen[i] || value.empty() || value.size() > 32)
                    continue;
                bool valid;
                if (key == "version")
                    valid = value.find_first_not_of("0123456789.") == value.npos;
                else if (key == "coded" || key == "visible")
                {
                    const auto x = value.find('x');
                    valid = x > 0 && x < value.size() - 1 &&
                            value.substr(0, x).find_first_not_of("0123456789") == value.npos &&
                            value.substr(x + 1).find_first_not_of("0123456789") == value.npos;
                }
                else
                {
                    auto digits = value;
                    if (digits.front() == '-')
                        digits.remove_prefix(1);
                    const bool hex = digits.starts_with("0x") || digits.starts_with("0X");
                    if (hex)
                        digits.remove_prefix(2);
                    valid = !digits.empty() &&
                            digits.find_first_not_of(hex ? "0123456789abcdefABCDEF"
                                                         : "0123456789") == digits.npos;
                }
                if (valid)
                {
                    report += std::string(line) + '\n';
                    seen[i] = true;
                }
            }
        }
        if (end == receipt.npos)
            break;
        receipt.remove_prefix(end + 1);
    }
    return report;
}
} // namespace

bool export_failure_report(const std::string &logs, const std::string &target, std::string &error)
{
    const auto path = logs + "/prosperotv-last-failure.txt";
    bool exists;
    if (!regular(path, 8192, exists) || !exists)
    {
        error = "No saved playback failure is available yet.";
        return false;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    auto *file = fd < 0 ? nullptr : fdopen(fd, "rb");
    if (!file)
    {
        if (fd >= 0)
            ::close(fd);
        error = "The saved failure report could not be read.";
        return false;
    }
    char bytes[8193];
    const auto count = std::fread(bytes, 1, sizeof(bytes), file);
    const bool valid = !std::ferror(file) && count <= 8192 &&
                       std::string_view(bytes, count).starts_with("PROSPEROTV_FAILURE_REPORT_V1\n");
    std::fclose(file);
    // Re-sanitize a saved report in case it was edited outside the app.
    if (!valid || !replace_file(target, {true, sanitize_report(std::string_view(bytes, count))}))
    {
        error = "The report could not be written. Check the report, USB drive and free space.";
        return false;
    }
    error.clear();
    return true;
}

std::string failure_report(std::string_view receipt, std::string_view version,
                           std::uint64_t timestamp, int result, unsigned attempts)
{
    std::string report;
    if (!version.empty() && version.size() <= 32 &&
        version.find_first_not_of("0123456789.") == version.npos)
        report = "version=" + std::string(version) + '\n';
    report += "unix_time=" + std::to_string(timestamp) + "\nresult=" + std::to_string(result) +
              "\nattempts=" + std::to_string(attempts) + '\n';
    report.append(receipt.substr(0, 65536));
    return sanitize_report(report);
}
} // namespace ptv
