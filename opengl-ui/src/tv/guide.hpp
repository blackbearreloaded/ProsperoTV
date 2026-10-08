// ProsperoTV - XMLTV programmes and provider catch-up addresses.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "iptv_catalog.h"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ptv
{
struct Programme
{
    std::int64_t start = 0, end = 0;
    std::string title, description, catchup_id;
};

// Uses UTC internally; only display strings use the console's timezone.
std::int64_t xmltv_time(std::string_view value);
std::string programme_time(std::int64_t value, bool date = false);
std::string catchup_url(const iptv::ChannelView &channel, const Programme &programme,
                        std::int64_t now);

class Guide
{
  public:
    static constexpr std::size_t kMaxProgrammes = 500000;
    static constexpr std::size_t kMaxTextBytes = 128u * 1024u * 1024u;
    std::unordered_map<std::string, std::vector<Programme>> channels;
    std::uint64_t source_id = 0, saved_unix = 0;
    bool truncated = false;
    void sort();
    std::size_t count() const;
    std::span<const Programme> programmes(std::string_view channel_id) const;
    const Programme *now(std::string_view channel_id, std::int64_t time) const;
    const Programme *next(std::string_view channel_id, std::int64_t time) const;
    bool matches_now(std::string_view channel_id, std::string_view query, std::int64_t time) const;
    bool save(const std::string &path) const;
    bool load(const std::string &path, std::uint64_t expected_source);
};

// Feed arbitrary network chunks (XML or gzip). Only channels in this catalog
// and programmes within 14 days of now are retained. No external entities.
class XmltvReader
{
  public:
    XmltvReader(const iptv::Catalog &catalog, std::int64_t now, Guide &guide);
    ~XmltvReader();
    XmltvReader(const XmltvReader &) = delete;
    XmltvReader &operator=(const XmltvReader &) = delete;
    bool feed(std::string_view bytes);
    bool finish();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ptv
