// ProsperoTV - Where the app keeps its files, and the request for filesystem access.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tv_storage.hpp"

#include "elevation/elevation.hpp"
#include "platform/ps5/system.hpp"
#include "tv_build_options.h"
#include "tv_paths.h"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

// runtime_shims.c: the log follows the app's data.
extern "C" void tv_log_move(const char *directory);

namespace tv::storage
{

namespace
{

using hui::sys::log;

constexpr char kDataRoot[] = "/data/prosperotv";
constexpr char kSandboxData[] = "/download0";
constexpr char kSandboxApp[] = "/app0";
constexpr char kSandboxLogs[] = "/download0/prosperotv";

// What an earlier version kept in the title's storage. Small files only: the
// downloaded channel lists are fetched again.
constexpr const char *kCarried[] = {
    "prosperotv-interface-v1.txt",
    "iptv-active-source-v1.txt",
    "iptv-custom-source-v1.txt",
    "prosperotv-xtream-v1.txt",
    "iptv-favorites-v1.bin",
    "iptv-history-v1.bin",
    "prosperotv-playback-history.sqlite3",
    "prosperotv-library.sqlite3",
};
constexpr std::size_t kCarriedLimit = 32u * 1024u * 1024u;

enum class Place
{
    config,
    cache,
    logs,
    app,
};

struct Slot
{
    char name[120];
    char path[260];
    Place place;
};

constexpr int kSlots = 64;
Slot g_slots[kSlots];
int g_slot_count = 0;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int g_status = -1;
bool g_elevated = false;
std::string &app()
{
    static std::string value = kSandboxApp;
    return value;
}
std::string &config()
{
    static std::string value = kSandboxData;
    return value;
}
std::string &cache()
{
    static std::string value = kSandboxData;
    return value;
}
std::string &logs()
{
    static std::string value = kSandboxLogs;
    return value;
}

Place place_of(const char *name)
{
    if (std::strstr(name, "catalog") != nullptr)
        return Place::cache;
    if (std::strstr(name, "receipt") != nullptr || std::strstr(name, "probe") != nullptr)
        return Place::logs;
    return Place::config;
}

// The receipts were beside everything else in the sandbox, not in its log folder.
const char *folder_of(Place place)
{
    switch (place)
    {
    case Place::cache:
        return cache().c_str();
    case Place::logs:
        return g_elevated ? logs().c_str() : kSandboxData;
    case Place::app:
        return app().c_str();
    case Place::config:
        break;
    }
    return config().c_str();
}

void fill(Slot &slot)
{
    std::snprintf(slot.path, sizeof(slot.path), "%s/%s", folder_of(slot.place), slot.name);
}

const char *slot_for(const char *name, Place place)
{
    if (name == nullptr || std::strlen(name) >= sizeof(Slot::name))
        return "";
    pthread_mutex_lock(&g_lock);
    Slot *found = nullptr;
    for (int i = 0; i < g_slot_count && found == nullptr; ++i)
        if (g_slots[i].place == place && std::strcmp(g_slots[i].name, name) == 0)
            found = &g_slots[i];
    if (found == nullptr && g_slot_count < kSlots)
    {
        found = &g_slots[g_slot_count++];
        std::snprintf(found->name, sizeof(found->name), "%s", name);
        found->place = place;
        fill(*found);
    }
    pthread_mutex_unlock(&g_lock);
    return found != nullptr ? found->path : "";
}

bool is_file(const std::string &path)
{
    struct stat facts
    {
    };
    return stat(path.c_str(), &facts) == 0 && S_ISREG(facts.st_mode);
}

bool read_small(const std::string &path, std::string *bytes)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    bytes->clear();
    char chunk[16384];
    std::size_t count = 0;
    while ((count = std::fread(chunk, 1, sizeof(chunk), file)) != 0 &&
           bytes->size() + count <= kCarriedLimit)
        bytes->append(chunk, count);
    const bool whole = count == 0 && std::ferror(file) == 0;
    std::fclose(file);
    return whole;
}

bool write_whole(const std::string &path, const std::string &bytes)
{
    const std::string temporary = path + ".tmp";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr)
        return false;
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (std::fclose(file) != 0 || !written || std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

// The app's own folder once the sandbox's /app0 is gone: where the console
// mounts the running app (whatever drive it is installed on), then the usual
// install folder.
std::string find_app()
{
    const std::string candidates[] = {
        kSandboxApp,
        std::string("/system_ex/app/") + TV_TITLE_ID,
        std::string("/data/homebrew/") + TV_TITLE_ID,
        std::string("/mnt/sandbox/") + TV_TITLE_ID + "_000/app0",
    };
    for (const std::string &candidate : candidates)
        if (is_file(candidate + "/eboot.bin"))
            return candidate;
    return candidates[2];
}

} // namespace

void initialize()
{
    // ---- what the sandbox holds, while it can still be read ----
    struct Carried
    {
        std::string bytes;
        bool present = false;
    };
    Carried carried[sizeof(kCarried) / sizeof(kCarried[0])];
    for (std::size_t i = 0; i < sizeof(kCarried) / sizeof(kCarried[0]); ++i)
        carried[i].present =
            read_small(std::string(kSandboxData) + "/" + kCarried[i], &carried[i].bytes);

    // ---- filesystem access ----
    // A test build stays in its sandbox when dev/no-elevation.txt is beside it.
    const bool refused_by_test =
        TV_DEV_SCRIPTS != 0 && is_file(std::string(kSandboxApp) + "/dev/no-elevation.txt");
    if (!refused_by_test)
        g_status = static_cast<int>(elevation::request(elevation::Capability::filesystem));
    g_elevated = g_status == 0;
    // Access leaves the effective group apart from the real one; libraries that
    // compare the two (caches, temporary files) then turn themselves off.
    if (getegid() != getgid())
        (void)setegid(getgid());

    // ---- every path, settled ----
    if (g_elevated)
    {
        app() = find_app();
        config() = std::string(kDataRoot) + "/config";
        cache() = std::string(kDataRoot) + "/cache";
        logs() = std::string(kDataRoot) + "/logs";
        (void)mkdir(kDataRoot, 0777);
    }
    for (const std::string *folder : {&config(), &cache(), &logs()})
        (void)mkdir(folder->c_str(), 0777);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_slot_count; ++i)
        fill(g_slots[i]);
    pthread_mutex_unlock(&g_lock);

    tv_log_move(logs().c_str());
    log("[TV] filesystem access: status=%d route=%s app=%s data=%s uid=%d/%d gid=%d/%d", g_status,
        route(), app().c_str(), g_elevated ? kDataRoot : kSandboxData, static_cast<int>(getuid()),
        static_cast<int>(geteuid()), static_cast<int>(getgid()), static_cast<int>(getegid()));

    // ---- what an earlier version kept moves over, once ----
    if (!g_elevated)
        return;
    for (std::size_t i = 0; i < sizeof(kCarried) / sizeof(kCarried[0]); ++i)
    {
        const std::string target = config() + "/" + kCarried[i];
        if (!carried[i].present || is_file(target))
            continue;
        log("[TV] carried over %s (%zu bytes): %s", kCarried[i], carried[i].bytes.size(),
            write_whole(target, carried[i].bytes) ? "ok" : "FAILED");
    }
}

bool elevated()
{
    return g_elevated;
}

int status()
{
    return g_status;
}

const char *route()
{
    return g_status < 0 ? "none" : elevation::path();
}

const std::string &app_dir()
{
    return app();
}

const std::string &config_dir()
{
    return config();
}

const std::string &cache_dir()
{
    return cache();
}

const std::string &logs_dir()
{
    return logs();
}

std::string app_file(const std::string &relative)
{
    return app() + "/" + relative;
}

} // namespace tv::storage

extern "C" const char *tv_data_file(const char *name)
{
    return tv::storage::slot_for(name, tv::storage::place_of(name));
}

extern "C" const char *tv_app_file(const char *relative)
{
    return tv::storage::slot_for(relative, tv::storage::Place::app);
}
