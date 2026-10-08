/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IPTV_CATALOG_H
#define IPTV_CATALOG_H

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace iptv
{

inline constexpr std::size_t kDefaultMaxPlaylistBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t kDefaultMaxRecordBytes = 16u * 1024u;
inline constexpr std::size_t kDefaultMaxUrlBytes = 4096u;
inline constexpr std::size_t kDefaultMaxFieldBytes = 2048u;
// A quarter of a million: more than the largest providers list as live channels.
inline constexpr std::size_t kDefaultMaxChannels = 250000u;
inline constexpr std::size_t kDefaultMaxAlternateUrls = 3u;
inline constexpr std::size_t kDefaultMaxAlternateGroups = 4u;
inline constexpr std::size_t kDefaultMaxDiagnostics = 128u;

struct ParseLimits
{
    std::size_t max_playlist_bytes = kDefaultMaxPlaylistBytes;
    std::size_t max_record_bytes = kDefaultMaxRecordBytes;
    std::size_t max_url_bytes = kDefaultMaxUrlBytes;
    std::size_t max_field_bytes = kDefaultMaxFieldBytes;
    std::size_t max_channels = kDefaultMaxChannels;
    std::size_t max_alternate_urls = kDefaultMaxAlternateUrls;
    std::size_t max_alternate_groups = kDefaultMaxAlternateGroups;
    std::size_t max_diagnostics = kDefaultMaxDiagnostics;
};

enum class ParseIssueCode : std::uint8_t
{
    input_too_large,
    overlong_record,
    malformed_extinf,
    attribute_too_long,
    missing_url,
    url_without_extinf,
    url_too_long,
    unsupported_url_scheme,
    unsafe_url,
    malformed_url,
    catalog_full,
};

struct ParseDiagnostic
{
    std::size_t line = 0;
    ParseIssueCode code = ParseIssueCode::malformed_extinf;
};

struct ParseReport
{
    std::size_t lines_seen = 0;
    std::size_t accepted = 0;
    std::size_t duplicates = 0;
    std::size_t skipped = 0;
    bool input_too_large = false;
    // The source lists more channels than a catalog takes: the first ones are
    // in it, the others were left out.
    bool catalog_full = false;
    std::vector<ParseDiagnostic> diagnostics;
};

enum class PlaybackStatus : std::uint8_t
{
    unknown,
    playable,
    failed,
};

// One channel with texts of its own: what a test or a form fills in. A catalog
// does not hold these (see Catalog); it reads them through a ChannelView.
struct Channel
{
    std::string id;
    std::uint64_t source_id = 0;
    std::string name;
    std::string url;
    std::vector<std::string> alternate_urls;
    std::string tvg_id;
    std::string tvg_name;
    std::string tvg_logo;
    std::string group_title;
    std::vector<std::string> alternate_group_titles;
    std::string tvg_country;
    std::string tvg_language;
    std::string http_user_agent;
    std::string http_referrer;
    std::string catchup;
    std::string catchup_source;
    std::string catchup_days;
    // Opaque portal command sent only to the provider's create_link API.
    std::string portal_command;
    std::uint32_t source_line = 0;
    PlaybackStatus playback_status = PlaybackStatus::unknown;
    int playback_result = 0;
    std::uint64_t playback_checked_unix = 0;
};

class Catalog;

// A channel's other addresses, or its other categories: a few texts in order.
// It borrows them, like the ChannelView it belongs to.
class TextList
{
  public:
    class Iterator
    {
      public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using pointer = const std::string_view *;
        using reference = std::string_view;

        std::string_view operator*() const;
        Iterator &operator++();
        bool operator==(const Iterator &other) const
        {
            return at_ == other.at_;
        }
        bool operator!=(const Iterator &other) const
        {
            return at_ != other.at_;
        }

      private:
        friend class TextList;
        const std::vector<std::string> *owned_ = nullptr;
        const Catalog *catalog_ = nullptr;
        std::uint32_t at_ = 0;
    };

    TextList() = default;
    explicit TextList(const std::vector<std::string> &texts) : owned_(&texts)
    {
    }

    bool empty() const;
    std::size_t size() const;
    std::string_view operator[](std::size_t index) const;
    Iterator begin() const;
    Iterator end() const;
    std::vector<std::string> Copy() const;

  private:
    friend class Catalog;
    TextList(const Catalog *catalog, std::uint32_t first) : catalog_(catalog), first_(first)
    {
    }

    const std::vector<std::string> *owned_ = nullptr;
    const Catalog *catalog_ = nullptr;
    std::uint32_t first_ = 0xffffffffu;
};

bool operator==(const TextList &left, const TextList &right);
bool operator==(const TextList &left, const std::vector<std::string> &right);

// One channel as a catalog (or a Channel) holds it. Nothing here is a copy:
// the view is good for as long as what it was taken from is there and is not
// added to, so it is read and let go, never kept. Every text is followed by a
// NUL, so data() is also a C string.
struct ChannelView
{
    std::string_view id;
    std::uint64_t source_id = 0;
    std::string_view name;
    std::string_view url;
    TextList alternate_urls;
    std::string_view tvg_id;
    std::string_view tvg_name;
    std::string_view tvg_logo;
    std::string_view group_title;
    TextList alternate_group_titles;
    std::string_view tvg_country;
    std::string_view tvg_language;
    std::string_view http_user_agent;
    std::string_view http_referrer;
    std::string_view catchup;
    std::string_view catchup_source;
    std::string_view catchup_days;
    std::string_view portal_command;
    std::uint32_t source_line = 0;
    PlaybackStatus playback_status = PlaybackStatus::unknown;
    int playback_result = 0;
    std::uint64_t playback_checked_unix = 0;

    ChannelView() = default;
    // A Channel is read like any other: this is meant to be implicit.
    ChannelView(const Channel &channel); // NOLINT(google-explicit-constructor)
    // The same channel with texts of its own, to keep.
    Channel Copy() const;
};

// The channels of one source. A provider can list a quarter of a million of
// them, so they are not kept as objects: every text goes once into blocks of
// memory that never move, the values many channels share (a category, a
// country) are stored once, and a channel holds compact places and lengths.
// That is about a third of what a std::string per field costs (300 bytes a
// channel for a typical provider, against more than 800), and none of it is
// ever copied or reallocated while the list grows.
class Catalog
{
  public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    enum class Field : std::uint8_t
    {
        id,
        name,
        url,
        tvg_id,
        tvg_name,
        tvg_logo,
        group_title,
        tvg_country,
        tvg_language,
        http_user_agent,
        http_referrer,
        catchup,
        catchup_source,
        catchup_days,
        portal_command,
        count,
    };

    class Iterator
    {
      public:
        using iterator_category = std::input_iterator_tag;
        using value_type = ChannelView;
        using difference_type = std::ptrdiff_t;
        using pointer = const ChannelView *;
        using reference = ChannelView;

        ChannelView operator*() const;
        Iterator &operator++()
        {
            ++index_;
            return *this;
        }
        bool operator==(const Iterator &other) const
        {
            return index_ == other.index_;
        }
        bool operator!=(const Iterator &other) const
        {
            return index_ != other.index_;
        }

      private:
        friend class Catalog;
        const Catalog *catalog_ = nullptr;
        std::size_t index_ = 0;
    };

    Catalog();
    ~Catalog();
    Catalog(Catalog &&other) noexcept;
    Catalog &operator=(Catalog &&other) noexcept;
    // A whole second catalog: for tests and tools, not for the app.
    Catalog(const Catalog &other);
    Catalog &operator=(const Catalog &other);

    // Every channel of a catalog comes from this source.
    std::uint64_t source_id = 0;

    // XMLTV addresses advertised by the playlist (kept with its cached copy).
    std::vector<std::string> guide_urls;

    std::size_t size() const;
    bool empty() const
    {
        return size() == 0;
    }
    ChannelView operator[](std::size_t index) const;
    ChannelView front() const
    {
        return (*this)[0];
    }
    ChannelView back() const
    {
        return (*this)[size() - 1u];
    }
    Iterator begin() const;
    Iterator end() const;

    // Adds a channel at the end. False when there is no room for it (memory,
    // or a text longer than a catalog holds); the catalog is then as it was.
    bool Add(const ChannelView &channel);
    // One more address or category for a channel that is already there.
    bool AddAlternateUrl(std::size_t index, std::string_view url);
    bool AddAlternateGroup(std::size_t index, std::string_view group);
    // Replaces one text of a channel (an id cannot be replaced).
    bool Set(std::size_t index, Field field, std::string_view value);
    void SetPlayback(std::size_t index, PlaybackStatus status, int result,
                     std::uint64_t checked_unix);

    // Where the channel with that id is, or npos.
    std::size_t Find(std::string_view id) const;

    void Clear();
    // What the catalog holds in memory, in bytes.
    std::size_t MemoryBytes() const;

  private:
    friend class TextList;
    struct Storage;
    std::string_view AlternateText(std::uint32_t node) const;
    std::uint32_t AlternateNext(std::uint32_t node) const;

    std::unique_ptr<Storage> storage_;
};

// Returns a normalized http(s) URL suitable for a playable catalog entry.
bool CanonicalizeStreamUrl(std::string_view raw, std::string *canonical);

// Reads an extended M3U playlist as it arrives: Feed it the bytes in order, in
// pieces of any size, then Finish. The channels go into `catalog` as their
// lines complete, so a playlist is never held whole. A playlist that names more
// channels than limits.max_channels keeps its first ones: the others are
// counted in the report, and full() says so, which is when a download can stop.
class M3uParser
{
  public:
    M3uParser(Catalog *catalog, std::uint64_t source_id, const ParseLimits &limits = ParseLimits{},
              ParseReport *report = nullptr);
    ~M3uParser();
    M3uParser(const M3uParser &) = delete;
    M3uParser &operator=(const M3uParser &) = delete;

    // False once the playlist is over limits.max_playlist_bytes: it is then
    // not a playlist this app loads, the catalog is emptied, and nothing more
    // is read.
    bool Feed(std::string_view bytes);
    void Finish();
    bool full() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// The same, for a playlist that is already whole in memory.
Catalog ParseExtendedM3u(std::string_view input, std::uint64_t source_id,
                         const ParseLimits &limits = ParseLimits{}, ParseReport *report = nullptr);

} // namespace iptv

#endif
