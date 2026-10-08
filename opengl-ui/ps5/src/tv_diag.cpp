// ProsperoTV - Where the diagnostic log goes on the console: logs/debug-trace.txt.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A write to the console's data drive takes tens of milliseconds, so no line
// is written by the thread that said it: lines wait in memory and a thread of
// their own appends them a few times a second. Each line starts with the
// seconds since the app started. A file past four megabytes becomes
// debug-trace.prev.txt at the next launch, so the two never outgrow the drive.

#include "tv_diag.hpp"

#include "tv/diag.hpp"
#include "tv/platform.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace tv::diag
{

namespace
{

constexpr std::size_t kWaitingLimit = 512u * 1024u;
constexpr long long kFileLimit = 4ll * 1024 * 1024;

std::mutex g_mutex;
std::string g_waiting;
std::string g_path;
std::size_t g_dropped = 0;
timespec g_started{};

void sink(const char *line)
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double seconds = static_cast<double>(now.tv_sec - g_started.tv_sec) +
                           static_cast<double>(now.tv_nsec - g_started.tv_nsec) / 1e9;
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "[%9.3f] ", seconds);
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_waiting.size() > kWaitingLimit)
    {
        ++g_dropped; // the drive is not keeping up: say how many were lost, later
        return;
    }
    g_waiting += stamp;
    g_waiting += line;
    g_waiting += '\n';
}

void *writer(void *)
{
    for (;;)
    {
        ptv::platform::sleep_ms(300);
        flush();
    }
    return nullptr;
}

} // namespace

void flush()
{
    std::string lines;
    std::size_t dropped = 0;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        if (g_waiting.empty() && g_dropped == 0)
            return;
        lines.swap(g_waiting);
        dropped = g_dropped;
        g_dropped = 0;
    }
    // One writer at a time, in the order the lines were taken.
    static std::mutex file_mutex;
    const std::lock_guard<std::mutex> lock(file_mutex);
    std::FILE *file = std::fopen(g_path.c_str(), "a");
    if (file == nullptr)
        return;
    std::fwrite(lines.data(), 1, lines.size(), file);
    if (dropped != 0)
        std::fprintf(file, "(%zu lines were not written: the drive was too slow)\n", dropped);
    std::fclose(file);
}

void start(const std::string &logs_dir)
{
    clock_gettime(CLOCK_MONOTONIC, &g_started);
    g_path = logs_dir + "/debug-trace.txt";
    struct stat info{};
    if (stat(g_path.c_str(), &info) == 0 && info.st_size > kFileLimit)
    {
        const std::string previous = logs_dir + "/debug-trace.prev.txt";
        std::remove(previous.c_str());
        std::rename(g_path.c_str(), previous.c_str());
    }
    ptv::diag::set_sink(sink);
    void *thread = ptv::platform::thread_start(writer, nullptr, 64u * 1024u, "TvDiagLog");
    if (thread != nullptr)
        (void)ptv::platform::thread_detach(thread);
}

} // namespace tv::diag
