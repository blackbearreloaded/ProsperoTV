/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

namespace iptv
{
namespace
{

constexpr std::size_t kHardMaxPlaylistBytes = 1024u * 1024u * 1024u;
constexpr std::size_t kHardMaxRecordBytes = 64u * 1024u;
constexpr std::size_t kHardMaxUrlBytes = 8192u;
constexpr std::size_t kHardMaxFieldBytes = 4096u;
constexpr std::size_t kHardMaxChannels = 1000000u;
constexpr std::size_t kHardMaxAlternateUrls = 8u;
constexpr std::size_t kHardMaxAlternateGroups = 8u;
constexpr std::size_t kHardMaxDiagnostics = 512u;

struct EffectiveLimits
{
    std::size_t max_playlist_bytes;
    std::size_t max_record_bytes;
    std::size_t max_url_bytes;
    std::size_t max_field_bytes;
    std::size_t max_channels;
    std::size_t max_alternate_urls;
    std::size_t max_alternate_groups;
    std::size_t max_diagnostics;
};

EffectiveLimits ClampLimits(const ParseLimits &limits)
{
    return {
        std::min(limits.max_playlist_bytes, kHardMaxPlaylistBytes),
        std::min(limits.max_record_bytes, kHardMaxRecordBytes),
        std::min(limits.max_url_bytes, kHardMaxUrlBytes),
        std::min(limits.max_field_bytes, kHardMaxFieldBytes),
        std::min(limits.max_channels, kHardMaxChannels),
        std::min(limits.max_alternate_urls, kHardMaxAlternateUrls),
        std::min(limits.max_alternate_groups, kHardMaxAlternateGroups),
        std::min(limits.max_diagnostics, kHardMaxDiagnostics),
    };
}

bool IsAsciiSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\v' ||
           value == '\f';
}

bool IsControl(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    return byte < 0x20u || byte == 0x7fu;
}

std::string_view Trim(std::string_view value)
{
    while (!value.empty() && IsAsciiSpace(value.front()))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && IsAsciiSpace(value.back()))
    {
        value.remove_suffix(1);
    }
    return value;
}

char LowerAscii(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    return byte < 128u ? static_cast<char>(std::tolower(byte)) : value;
}

bool EqualInsensitive(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        if (LowerAscii(left[i]) != LowerAscii(right[i]))
        {
            return false;
        }
    }
    return true;
}

bool StartsWithInsensitive(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() &&
           EqualInsensitive(value.substr(0, prefix.size()), prefix);
}

std::string LowerTrimmed(std::string_view value)
{
    value = Trim(value);
    std::string result;
    result.reserve(value.size());
    for (char byte : value)
    {
        result.push_back(LowerAscii(byte));
    }
    return result;
}

std::string Hex(std::uint64_t value)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t i = result.size(); i > 0; --i)
    {
        result[i - 1] = digits[value & 0x0fu];
        value >>= 4;
    }
    return result;
}

std::uint64_t Fnv1a(std::string_view value, std::uint64_t hash = 1469598103934665603ull)
{
    for (unsigned char byte : value)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string StableId(std::uint64_t source_id, std::string_view identity)
{
    std::string seed = "source:" + Hex(source_id) + "|";
    seed.append(identity.data(), identity.size());
    return "ch-" + Hex(Fnv1a(seed));
}

enum class UrlError : std::uint8_t
{
    none,
    too_long,
    unsupported_scheme,
    unsafe,
    malformed,
};

bool IsSchemeChar(char value, bool first)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    if (first)
    {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
    }
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || value == '+' || value == '-' || value == '.';
}

bool IsDigits(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }
    for (char byte : value)
    {
        if (byte < '0' || byte > '9')
        {
            return false;
        }
    }
    return true;
}

UrlError CanonicalizeUrl(std::string_view raw, std::string *canonical)
{
    if (canonical == nullptr)
    {
        return UrlError::malformed;
    }
    canonical->clear();
    raw = Trim(raw);
    if (raw.empty())
    {
        return UrlError::malformed;
    }
    if (raw.size() > kHardMaxUrlBytes)
    {
        return UrlError::too_long;
    }
    for (char byte : raw)
    {
        if (IsControl(byte) || IsAsciiSpace(byte) || byte == '\\')
        {
            return UrlError::unsafe;
        }
    }

    const std::size_t colon = raw.find(':');
    if (colon == std::string_view::npos || colon == 0)
    {
        return UrlError::malformed;
    }
    for (std::size_t i = 0; i < colon; ++i)
    {
        if (!IsSchemeChar(raw[i], i == 0))
        {
            return UrlError::malformed;
        }
    }
    const std::string scheme = LowerTrimmed(raw.substr(0, colon));
    if (scheme != "http" && scheme != "https")
    {
        return UrlError::unsupported_scheme;
    }
    if (raw.substr(colon, 3) != "://")
    {
        return UrlError::malformed;
    }

    const std::size_t authority_start = colon + 3;
    std::size_t authority_end = raw.find_first_of("/?#", authority_start);
    if (authority_end == std::string_view::npos)
    {
        authority_end = raw.size();
    }
    const std::string_view authority = raw.substr(authority_start, authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos)
    {
        return UrlError::unsafe;
    }

    if (authority.front() == '[')
    {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1)
        {
            return UrlError::malformed;
        }
        if (close + 1 < authority.size() &&
            (authority[close + 1] != ':' || !IsDigits(authority.substr(close + 2))))
        {
            return UrlError::malformed;
        }
    }
    else
    {
        const std::size_t port_separator = authority.find(':');
        const std::string_view host = authority.substr(0, port_separator);
        if (host.empty() || host.find('[') != std::string_view::npos ||
            host.find(']') != std::string_view::npos)
        {
            return UrlError::malformed;
        }
        if (port_separator != std::string_view::npos &&
            !IsDigits(authority.substr(port_separator + 1)))
        {
            return UrlError::malformed;
        }
    }

    const std::size_t fragment = raw.find('#', authority_end);
    const std::size_t content_end = fragment == std::string_view::npos ? raw.size() : fragment;
    canonical->reserve(content_end);
    canonical->append(scheme);
    canonical->append("://");
    for (std::size_t i = authority_start; i < authority_end; ++i)
    {
        canonical->push_back(LowerAscii(raw[i]));
    }
    canonical->append(raw.substr(authority_end, content_end - authority_end));
    if (canonical->size() > kHardMaxUrlBytes)
    {
        canonical->clear();
        return UrlError::too_long;
    }
    return UrlError::none;
}

} // namespace

bool CanonicalizeStreamUrl(std::string_view raw, std::string *canonical)
{
    return CanonicalizeUrl(raw, canonical) == UrlError::none;
}

// ---- the catalog --------------------------------------------------------------

namespace
{

constexpr std::uint32_t kNone = 0xffffffffu;
constexpr std::size_t kFieldCount = static_cast<std::size_t>(Catalog::Field::count);
constexpr std::size_t kCoreFieldCount = static_cast<std::size_t>(Catalog::Field::catchup);
using ArchiveFields = std::array<std::string_view, kFieldCount - kCoreFieldCount>;
constexpr std::size_t kNameField = static_cast<std::size_t>(Catalog::Field::name);
constexpr std::size_t kGuideNameField = static_cast<std::size_t>(Catalog::Field::tvg_name);
// From here on a field's values repeat from channel to channel.
constexpr std::size_t kFirstSharedField = static_cast<std::size_t>(Catalog::Field::group_title);
// Text lives in blocks of 1 MiB: a place is a block and an offset in 32 bits.
constexpr unsigned kBlockShift = 20u;
constexpr std::size_t kBlockBytes = std::size_t{1} << kBlockShift;
constexpr std::size_t kMaxBlocks = std::size_t{1} << (32u - kBlockShift);
constexpr std::size_t kRecordsPerChunk = 4096u;
constexpr std::size_t kMaxTextBytes = 0xffffu;
// A list where every channel has a category of its own shares nothing: past
// this many different values the rest are stored like any other text.
constexpr std::size_t kMaxSharedTexts = 32768u;
constexpr char kEmptyText[1] = {'\0'};

struct Record
{
    std::uint32_t at[kCoreFieldCount];
    std::uint16_t bytes[kCoreFieldCount];
    std::uint8_t playback;
    std::uint8_t reserved;
    std::uint32_t source_line;
    std::uint32_t urls;   // its first other address, or kNone
    std::uint32_t groups; // its first other category, or kNone
};

struct Alternate
{
    std::uint32_t at;
    std::uint32_t next;
    std::uint16_t bytes;
};

struct Played
{
    int result = 0;
    std::uint64_t checked_unix = 0;
};

std::uint64_t KeyOf(std::string_view text)
{
    // FNV-1a, then stirred so that its low bits can pick a slot.
    std::uint64_t key = Fnv1a(text);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ull;
    key ^= key >> 33;
    return key;
}

// Where a key's channel is: an open table of 64-bit keys, twelve bytes a slot.
class KeyTable
{
  public:
    // A key may be added more than once. False: there was no memory for it.
    bool Add(std::uint64_t key, std::uint32_t value)
    {
        if ((size_ + 1u) * 10u > capacity_ * 7u && !Grow())
            return false;
        Place(keys_.get(), values_.get(), capacity_, key, value);
        ++size_;
        return true;
    }

    // Adds the key unless it is there: its first channel keeps it.
    bool AddOnce(std::uint64_t key, std::uint32_t value)
    {
        return Find(key) != kNone || Add(key, value);
    }

    template <typename Accept> std::uint32_t Find(std::uint64_t key, Accept accept) const
    {
        if (capacity_ == 0)
            return kNone;
        const std::size_t mask = capacity_ - 1u;
        for (std::size_t slot = static_cast<std::size_t>(key) & mask;; slot = (slot + 1u) & mask)
        {
            if (values_[slot] == kNone)
                return kNone;
            if (keys_[slot] == key && accept(values_[slot]))
                return values_[slot];
        }
    }

    std::uint32_t Find(std::uint64_t key) const
    {
        return Find(key, [](std::uint32_t) { return true; });
    }

    std::size_t Bytes() const
    {
        return capacity_ * (sizeof(std::uint64_t) + sizeof(std::uint32_t));
    }

  private:
    static void Place(std::uint64_t *keys, std::uint32_t *values, std::size_t capacity,
                      std::uint64_t key, std::uint32_t value)
    {
        const std::size_t mask = capacity - 1u;
        std::size_t slot = static_cast<std::size_t>(key) & mask;
        while (values[slot] != kNone)
            slot = (slot + 1u) & mask;
        keys[slot] = key;
        values[slot] = value;
    }

    bool Grow()
    {
        const std::size_t capacity = capacity_ == 0 ? 1024u : capacity_ * 2u;
        std::unique_ptr<std::uint64_t[]> keys(new (std::nothrow) std::uint64_t[capacity]);
        std::unique_ptr<std::uint32_t[]> values(new (std::nothrow) std::uint32_t[capacity]);
        if (!keys || !values)
            return false;
        std::fill_n(values.get(), capacity, kNone);
        for (std::size_t slot = 0; slot < capacity_; ++slot)
            if (values_[slot] != kNone)
                Place(keys.get(), values.get(), capacity, keys_[slot], values_[slot]);
        keys_ = std::move(keys);
        values_ = std::move(values);
        capacity_ = capacity;
        return true;
    }

    std::unique_ptr<std::uint64_t[]> keys_;
    std::unique_ptr<std::uint32_t[]> values_;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

} // namespace

struct Catalog::Storage
{
    std::vector<std::unique_ptr<char[]>> blocks;
    std::size_t block_used = kBlockBytes;
    std::vector<std::unique_ptr<Record[]>> chunks;
    std::size_t count = 0;
    std::vector<Alternate> alternates;
    std::unordered_map<std::string_view, std::uint32_t> shared;
    std::unordered_map<std::uint32_t, Played> played;
    // Most public channels have no archive. Their records stay the same size.
    // These optional fields borrow text from the same stable arena.
    std::unordered_map<std::uint32_t, ArchiveFields> archives;
    KeyTable ids;

    const char *Text(std::uint32_t at) const
    {
        return blocks[at >> kBlockShift].get() + (at & (kBlockBytes - 1u));
    }

    std::string_view View(std::uint32_t at, std::size_t bytes) const
    {
        return bytes == 0 ? std::string_view(kEmptyText, 0) : std::string_view(Text(at), bytes);
    }

    Record &At(std::size_t index)
    {
        return chunks[index / kRecordsPerChunk][index % kRecordsPerChunk];
    }

    const Record &At(std::size_t index) const
    {
        return chunks[index / kRecordsPerChunk][index % kRecordsPerChunk];
    }

    // Copies a text, and the NUL after it, into the blocks.
    bool Store(std::string_view text, std::uint32_t *at)
    {
        *at = 0;
        if (text.empty())
            return true;
        if (text.size() > kMaxTextBytes)
            return false;
        const std::size_t needed = text.size() + 1u;
        if (block_used + needed > kBlockBytes)
        {
            if (blocks.size() >= kMaxBlocks)
                return false;
            std::unique_ptr<char[]> block(new (std::nothrow) char[kBlockBytes]);
            if (!block)
                return false;
            blocks.push_back(std::move(block));
            block_used = 0;
        }
        char *to = blocks.back().get() + block_used;
        std::memcpy(to, text.data(), text.size());
        to[text.size()] = '\0';
        *at = static_cast<std::uint32_t>(((blocks.size() - 1u) << kBlockShift) | block_used);
        block_used += needed;
        return true;
    }

    // The same, for a value other channels are likely to have too.
    bool StoreShared(std::string_view text, std::uint32_t *at)
    {
        *at = 0;
        if (text.empty())
            return true;
        const auto found = shared.find(text);
        if (found != shared.end())
        {
            *at = found->second;
            return true;
        }
        if (!Store(text, at))
            return false;
        if (shared.size() < kMaxSharedTexts)
            shared.emplace(std::string_view(Text(*at), text.size()), *at);
        return true;
    }

    bool AddAlternate(std::uint32_t *first, std::string_view text, bool share)
    {
        std::uint32_t at = 0;
        if (text.empty() || alternates.size() >= kNone ||
            !(share ? StoreShared(text, &at) : Store(text, &at)))
            return false;
        const std::uint32_t node = static_cast<std::uint32_t>(alternates.size());
        alternates.push_back({at, kNone, static_cast<std::uint16_t>(text.size())});
        // A channel has a handful: the end of its chain is a short walk.
        std::uint32_t *link = first;
        while (*link != kNone)
            link = &alternates[*link].next;
        *link = node;
        return true;
    }
};

Catalog::Catalog() = default;
Catalog::~Catalog() = default;

Catalog::Catalog(Catalog &&other) noexcept
    : source_id(other.source_id), guide_urls(std::move(other.guide_urls)),
      storage_(std::move(other.storage_))
{
    other.source_id = 0;
}

Catalog &Catalog::operator=(Catalog &&other) noexcept
{
    if (this != &other)
    {
        source_id = other.source_id;
        guide_urls = std::move(other.guide_urls);
        storage_ = std::move(other.storage_);
        other.source_id = 0;
    }
    return *this;
}

Catalog::Catalog(const Catalog &other) : source_id(other.source_id), guide_urls(other.guide_urls)
{
    for (std::size_t index = 0; index < other.size(); ++index)
        (void)Add(other[index]);
}

Catalog &Catalog::operator=(const Catalog &other)
{
    if (this != &other)
    {
        Catalog copy(other);
        *this = std::move(copy);
    }
    return *this;
}

std::size_t Catalog::size() const
{
    return storage_ ? storage_->count : 0u;
}

ChannelView Catalog::operator[](std::size_t index) const
{
    const Storage &storage = *storage_;
    const Record &record = storage.At(index);
    const auto text = [&](Field field)
    {
        const std::size_t at = static_cast<std::size_t>(field);
        return storage.View(record.at[at], record.bytes[at]);
    };
    ChannelView view;
    view.id = text(Field::id);
    view.source_id = source_id;
    view.name = text(Field::name);
    view.url = text(Field::url);
    view.alternate_urls = TextList(this, record.urls);
    view.tvg_id = text(Field::tvg_id);
    view.tvg_name = text(Field::tvg_name);
    view.tvg_logo = text(Field::tvg_logo);
    view.group_title = text(Field::group_title);
    view.alternate_group_titles = TextList(this, record.groups);
    view.tvg_country = text(Field::tvg_country);
    view.tvg_language = text(Field::tvg_language);
    view.http_user_agent = text(Field::http_user_agent);
    view.http_referrer = text(Field::http_referrer);
    const auto archive = storage.archives.find(static_cast<std::uint32_t>(index));
    if (archive != storage.archives.end())
    {
        view.catchup = archive->second[0];
        view.catchup_source = archive->second[1];
        view.catchup_days = archive->second[2];
        view.portal_command = archive->second[3];
    }
    view.source_line = record.source_line;
    view.playback_status = static_cast<PlaybackStatus>(record.playback);
    if (view.playback_status != PlaybackStatus::unknown)
    {
        const auto found = storage.played.find(static_cast<std::uint32_t>(index));
        if (found != storage.played.end())
        {
            view.playback_result = found->second.result;
            view.playback_checked_unix = found->second.checked_unix;
        }
    }
    return view;
}

Catalog::Iterator Catalog::begin() const
{
    Iterator iterator;
    iterator.catalog_ = this;
    return iterator;
}

Catalog::Iterator Catalog::end() const
{
    Iterator iterator;
    iterator.catalog_ = this;
    iterator.index_ = size();
    return iterator;
}

ChannelView Catalog::Iterator::operator*() const
{
    return (*catalog_)[index_];
}

bool Catalog::Add(const ChannelView &channel)
{
    const std::string_view texts[kFieldCount] = {
        channel.id,
        channel.name,
        channel.url,
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
    for (const std::string_view text : texts)
        if (text.size() > kMaxTextBytes)
            return false;
    for (const std::string_view text : channel.alternate_urls)
        if (text.size() > kMaxTextBytes)
            return false;
    for (const std::string_view text : channel.alternate_group_titles)
        if (text.size() > kMaxTextBytes)
            return false;

    if (!storage_)
    {
        storage_.reset(new (std::nothrow) Storage);
        if (!storage_)
            return false;
    }
    Storage &storage = *storage_;
    if (storage.count >= kNone)
        return false;
    if (storage.count == storage.chunks.size() * kRecordsPerChunk)
    {
        std::unique_ptr<Record[]> chunk(new (std::nothrow) Record[kRecordsPerChunk]);
        if (!chunk)
            return false;
        storage.chunks.push_back(std::move(chunk));
    }

    Record record{};
    ArchiveFields archive{};
    bool has_archive = false;
    for (std::size_t field = 0; field < kFieldCount; ++field)
    {
        std::uint32_t at = 0;
        // A playlist that names a channel twice says the same thing twice.
        if (field == kGuideNameField && texts[field] == texts[kNameField])
            at = record.at[kNameField];
        else if (!(field >= kFirstSharedField ? storage.StoreShared(texts[field], &at)
                                              : storage.Store(texts[field], &at)))
            return false;
        if (field < kCoreFieldCount)
        {
            record.at[field] = at;
            record.bytes[field] = static_cast<std::uint16_t>(texts[field].size());
        }
        else
        {
            archive[field - kCoreFieldCount] = storage.View(at, texts[field].size());
            has_archive = has_archive || !texts[field].empty();
        }
    }
    record.source_line = channel.source_line;
    record.urls = kNone;
    record.groups = kNone;
    for (const std::string_view url : channel.alternate_urls)
        if (!storage.AddAlternate(&record.urls, url, false))
            return false;
    for (const std::string_view group : channel.alternate_group_titles)
        if (!storage.AddAlternate(&record.groups, group, true))
            return false;

    const std::uint32_t index = static_cast<std::uint32_t>(storage.count);
    if (!storage.ids.Add(KeyOf(channel.id), index))
        return false;
    storage.At(index) = record;
    if (has_archive)
        storage.archives.emplace(index, archive);
    ++storage.count;
    if (channel.playback_status != PlaybackStatus::unknown)
        SetPlayback(index, channel.playback_status, channel.playback_result,
                    channel.playback_checked_unix);
    return true;
}

bool Catalog::AddAlternateUrl(std::size_t index, std::string_view url)
{
    return index < size() && url.size() <= kMaxTextBytes &&
           storage_->AddAlternate(&storage_->At(index).urls, url, false);
}

bool Catalog::AddAlternateGroup(std::size_t index, std::string_view group)
{
    return index < size() && group.size() <= kMaxTextBytes &&
           storage_->AddAlternate(&storage_->At(index).groups, group, true);
}

bool Catalog::Set(std::size_t index, Field field, std::string_view value)
{
    const std::size_t at = static_cast<std::size_t>(field);
    if (index >= size() || field == Field::id || at >= kFieldCount || value.size() > kMaxTextBytes)
        return false;
    std::uint32_t place = 0;
    if (!(at >= kFirstSharedField ? storage_->StoreShared(value, &place)
                                  : storage_->Store(value, &place)))
        return false;
    Record &record = storage_->At(index);
    if (at >= kCoreFieldCount)
    {
        const auto key = static_cast<std::uint32_t>(index);
        if (!value.empty() || storage_->archives.contains(key))
            storage_->archives[key][at - kCoreFieldCount] = storage_->View(place, value.size());
        return true;
    }
    record.at[at] = place;
    record.bytes[at] = static_cast<std::uint16_t>(value.size());
    return true;
}

void Catalog::SetPlayback(std::size_t index, PlaybackStatus status, int result,
                          std::uint64_t checked_unix)
{
    if (index >= size())
        return;
    storage_->At(index).playback = static_cast<std::uint8_t>(status);
    const std::uint32_t key = static_cast<std::uint32_t>(index);
    if (status == PlaybackStatus::unknown)
        storage_->played.erase(key);
    else
        storage_->played[key] = {result, checked_unix};
}

std::size_t Catalog::Find(std::string_view id) const
{
    if (!storage_ || id.empty())
        return npos;
    const Storage &storage = *storage_;
    const std::size_t id_field = static_cast<std::size_t>(Field::id);
    const std::uint32_t found =
        storage.ids.Find(KeyOf(id),
                         [&](std::uint32_t index)
                         {
                             if (index >= storage.count)
                                 return false;
                             const Record &record = storage.At(index);
                             return storage.View(record.at[id_field], record.bytes[id_field]) == id;
                         });
    return found == kNone ? npos : found;
}

void Catalog::Clear()
{
    guide_urls.clear();
    storage_.reset();
}

std::size_t Catalog::MemoryBytes() const
{
    if (!storage_)
        return 0;
    const Storage &storage = *storage_;
    return storage.blocks.size() * kBlockBytes +
           storage.chunks.size() * kRecordsPerChunk * sizeof(Record) +
           storage.alternates.capacity() * sizeof(Alternate) + storage.ids.Bytes() +
           (storage.shared.size() + storage.played.size()) * 64u +
           storage.archives.size() * (sizeof(std::pair<const std::uint32_t, ArchiveFields>) + 32u);
}

std::string_view Catalog::AlternateText(std::uint32_t node) const
{
    const Alternate &alternate = storage_->alternates[node];
    return storage_->View(alternate.at, alternate.bytes);
}

std::uint32_t Catalog::AlternateNext(std::uint32_t node) const
{
    return storage_->alternates[node].next;
}

// ---- a channel's few texts --------------------------------------------------------

bool TextList::empty() const
{
    return owned_ != nullptr ? owned_->empty() : first_ == kNone;
}

std::size_t TextList::size() const
{
    if (owned_ != nullptr)
        return owned_->size();
    std::size_t count = 0;
    for (std::uint32_t node = first_; node != kNone; node = catalog_->AlternateNext(node))
        ++count;
    return count;
}

std::string_view TextList::operator[](std::size_t index) const
{
    if (owned_ != nullptr)
        return (*owned_)[index];
    std::uint32_t node = first_;
    while (index-- != 0)
        node = catalog_->AlternateNext(node);
    return catalog_->AlternateText(node);
}

TextList::Iterator TextList::begin() const
{
    Iterator iterator;
    iterator.owned_ = owned_;
    iterator.catalog_ = catalog_;
    iterator.at_ = owned_ != nullptr ? 0u : first_;
    return iterator;
}

TextList::Iterator TextList::end() const
{
    Iterator iterator;
    iterator.owned_ = owned_;
    iterator.catalog_ = catalog_;
    iterator.at_ = owned_ != nullptr ? static_cast<std::uint32_t>(owned_->size()) : kNone;
    return iterator;
}

std::string_view TextList::Iterator::operator*() const
{
    return owned_ != nullptr ? std::string_view((*owned_)[at_]) : catalog_->AlternateText(at_);
}

TextList::Iterator &TextList::Iterator::operator++()
{
    at_ = owned_ != nullptr ? at_ + 1u : catalog_->AlternateNext(at_);
    return *this;
}

std::vector<std::string> TextList::Copy() const
{
    std::vector<std::string> texts;
    for (const std::string_view text : *this)
        texts.emplace_back(text);
    return texts;
}

bool operator==(const TextList &left, const TextList &right)
{
    return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

bool operator==(const TextList &left, const std::vector<std::string> &right)
{
    return left == TextList(right);
}

ChannelView::ChannelView(const Channel &channel)
    : id(channel.id), source_id(channel.source_id), name(channel.name), url(channel.url),
      alternate_urls(channel.alternate_urls), tvg_id(channel.tvg_id), tvg_name(channel.tvg_name),
      tvg_logo(channel.tvg_logo), group_title(channel.group_title),
      alternate_group_titles(channel.alternate_group_titles), tvg_country(channel.tvg_country),
      tvg_language(channel.tvg_language), http_user_agent(channel.http_user_agent),
      http_referrer(channel.http_referrer), catchup(channel.catchup),
      catchup_source(channel.catchup_source), catchup_days(channel.catchup_days),
      portal_command(channel.portal_command), source_line(channel.source_line),
      playback_status(channel.playback_status), playback_result(channel.playback_result),
      playback_checked_unix(channel.playback_checked_unix)
{
}

Channel ChannelView::Copy() const
{
    Channel channel;
    channel.id = id;
    channel.source_id = source_id;
    channel.name = name;
    channel.url = url;
    channel.alternate_urls = alternate_urls.Copy();
    channel.tvg_id = tvg_id;
    channel.tvg_name = tvg_name;
    channel.tvg_logo = tvg_logo;
    channel.group_title = group_title;
    channel.alternate_group_titles = alternate_group_titles.Copy();
    channel.tvg_country = tvg_country;
    channel.tvg_language = tvg_language;
    channel.http_user_agent = http_user_agent;
    channel.http_referrer = http_referrer;
    channel.catchup = catchup;
    channel.catchup_source = catchup_source;
    channel.catchup_days = catchup_days;
    channel.portal_command = portal_command;
    channel.source_line = source_line;
    channel.playback_status = playback_status;
    channel.playback_result = playback_result;
    channel.playback_checked_unix = playback_checked_unix;
    return channel;
}

// ---- the playlist ---------------------------------------------------------------

namespace
{

struct EntryMetadata
{
    std::string title;
    std::string tvg_id;
    std::string tvg_name;
    std::string tvg_logo;
    std::string group_title;
    std::string tvg_country;
    std::string tvg_language;
    std::string http_user_agent;
    std::string http_referrer;
    std::string catchup, catchup_source, catchup_days, guide_urls;
};

struct PendingEntry
{
    EntryMetadata metadata;
    std::uint32_t line = 0;
};

void AddDiagnostic(ParseReport *report, const EffectiveLimits &limits, std::size_t line,
                   ParseIssueCode code)
{
    ++report->skipped;
    if (report->diagnostics.size() < limits.max_diagnostics)
    {
        report->diagnostics.push_back({line, code});
    }
}

bool ValidDuration(std::string_view value)
{
    value = Trim(value);
    if (value.empty())
    {
        return false;
    }
    std::size_t i = (value.front() == '-' || value.front() == '+') ? 1 : 0;
    bool digit = false;
    bool dot = false;
    for (; i < value.size(); ++i)
    {
        const char byte = value[i];
        if (byte >= '0' && byte <= '9')
        {
            digit = true;
        }
        else if (byte == '.' && !dot)
        {
            dot = true;
        }
        else
        {
            return false;
        }
    }
    return digit;
}

bool IsAttributeKeyChar(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || value == '-' || value == '_';
}

void SetAttribute(std::string_view key, std::string &&value, EntryMetadata *metadata)
{
    if (key == "catchup")
        metadata->catchup = std::move(value);
    else if (key == "catchup-source")
        metadata->catchup_source = std::move(value);
    else if (key == "catchup-days" || key == "timeshift")
        metadata->catchup_days = std::move(value);
    else if (key == "url-tvg" || key == "x-tvg-url" || key == "tvg-url")
        metadata->guide_urls = std::move(value);
    if (key == "tvg-id")
    {
        metadata->tvg_id = std::move(value);
    }
    else if (key == "tvg-name")
    {
        metadata->tvg_name = std::move(value);
    }
    else if (key == "tvg-logo")
    {
        metadata->tvg_logo = std::move(value);
    }
    else if (key == "group-title")
    {
        metadata->group_title = std::move(value);
    }
    else if (key == "tvg-country")
    {
        metadata->tvg_country = std::move(value);
    }
    else if (key == "tvg-language")
    {
        metadata->tvg_language = std::move(value);
    }
}

bool SetHttpOption(std::string_view line, std::size_t max_field_bytes, EntryMetadata *metadata)
{
    static constexpr std::string_view prefix = "#EXTVLCOPT:";
    if (!StartsWithInsensitive(line, prefix))
        return false;
    line.remove_prefix(prefix.size());
    const std::size_t separator = line.find('=');
    if (separator == std::string_view::npos)
        return true;
    const std::string key = LowerTrimmed(line.substr(0, separator));
    const std::string_view value = Trim(line.substr(separator + 1));
    if (value.size() > max_field_bytes)
        return true;
    for (char byte : value)
    {
        if (IsControl(byte) && byte != '\t')
            return true;
    }
    if (key == "http-user-agent")
    {
        metadata->http_user_agent.assign(value);
    }
    else if (key == "http-referrer" || key == "http-referer")
    {
        metadata->http_referrer.assign(value);
    }
    return true;
}

bool ParseExtinf(std::string_view line, std::size_t max_field_bytes, EntryMetadata *metadata,
                 ParseIssueCode *error)
{
    const std::string_view body = line.substr(8); // strlen("#EXTINF:")
    bool quoted = false;
    bool escaped = false;
    std::size_t comma = std::string_view::npos;
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        const char byte = body[i];
        if (escaped)
        {
            escaped = false;
        }
        else if (byte == '\\' && quoted)
        {
            escaped = true;
        }
        else if (byte == '"')
        {
            quoted = !quoted;
        }
        else if (byte == ',' && !quoted)
        {
            comma = i;
            break;
        }
    }
    if (quoted || comma == std::string_view::npos)
    {
        *error = ParseIssueCode::malformed_extinf;
        return false;
    }

    const std::string_view prefix = Trim(body.substr(0, comma));
    std::size_t duration_end = 0;
    while (duration_end < prefix.size() && !IsAsciiSpace(prefix[duration_end]))
    {
        ++duration_end;
    }
    if (!ValidDuration(prefix.substr(0, duration_end)))
    {
        *error = ParseIssueCode::malformed_extinf;
        return false;
    }

    EntryMetadata parsed;
    const std::string_view title = Trim(body.substr(comma + 1));
    if (title.size() > max_field_bytes)
    {
        *error = ParseIssueCode::attribute_too_long;
        return false;
    }
    parsed.title.assign(title.data(), title.size());

    std::size_t cursor = duration_end;
    const std::string_view attributes = prefix.substr(cursor);
    std::size_t i = 0;
    while (i < attributes.size())
    {
        while (i < attributes.size() && IsAsciiSpace(attributes[i]))
        {
            ++i;
        }
        if (i == attributes.size())
        {
            break;
        }
        const std::size_t key_start = i;
        while (i < attributes.size() && IsAttributeKeyChar(attributes[i]))
        {
            ++i;
        }
        if (key_start == i)
        {
            *error = ParseIssueCode::malformed_extinf;
            return false;
        }
        std::string key;
        key.reserve(i - key_start);
        for (std::size_t k = key_start; k < i; ++k)
        {
            key.push_back(LowerAscii(attributes[k]));
        }
        while (i < attributes.size() && IsAsciiSpace(attributes[i]))
        {
            ++i;
        }
        if (i == attributes.size() || attributes[i] != '=')
        {
            *error = ParseIssueCode::malformed_extinf;
            return false;
        }
        ++i;
        while (i < attributes.size() && IsAsciiSpace(attributes[i]))
        {
            ++i;
        }

        std::string value;
        if (i < attributes.size() && attributes[i] == '"')
        {
            ++i;
            bool closed = false;
            while (i < attributes.size())
            {
                const char byte = attributes[i++];
                if (byte == '"')
                {
                    closed = true;
                    break;
                }
                if (byte == '\\' && i < attributes.size() &&
                    (attributes[i] == '\\' || attributes[i] == '"'))
                {
                    value.push_back(attributes[i++]);
                }
                else
                {
                    value.push_back(byte);
                }
                if (value.size() > max_field_bytes)
                {
                    *error = ParseIssueCode::attribute_too_long;
                    return false;
                }
            }
            if (!closed)
            {
                *error = ParseIssueCode::malformed_extinf;
                return false;
            }
        }
        else
        {
            const std::size_t value_start = i;
            while (i < attributes.size() && !IsAsciiSpace(attributes[i]))
            {
                ++i;
            }
            if (i == value_start || i - value_start > max_field_bytes)
            {
                *error = i - value_start > max_field_bytes ? ParseIssueCode::attribute_too_long
                                                           : ParseIssueCode::malformed_extinf;
                return false;
            }
            value.assign(attributes.substr(value_start, i - value_start));
        }
        SetAttribute(key, std::move(value), &parsed);
    }
    *metadata = std::move(parsed);
    return true;
}

bool UrlIssue(std::string_view raw, std::size_t max_url_bytes, std::string *canonical,
              ParseIssueCode *issue)
{
    if (Trim(raw).size() > max_url_bytes)
    {
        *issue = ParseIssueCode::url_too_long;
        return false;
    }
    switch (CanonicalizeUrl(raw, canonical))
    {
    case UrlError::none:
        return true;
    case UrlError::too_long:
        *issue = ParseIssueCode::url_too_long;
        return false;
    case UrlError::unsupported_scheme:
        *issue = ParseIssueCode::unsupported_url_scheme;
        return false;
    case UrlError::unsafe:
        *issue = ParseIssueCode::unsafe_url;
        return false;
    case UrlError::malformed:
        *issue = ParseIssueCode::malformed_url;
        return false;
    }
    *issue = ParseIssueCode::malformed_url;
    return false;
}

bool Contains(const TextList &values, std::string_view value)
{
    return std::any_of(values.begin(), values.end(),
                       [value](std::string_view item) { return item == value; });
}

// A later entry of the same channel: its address becomes one more to try, and
// what the first entry left unsaid is taken from it.
void MergeChannel(Catalog *catalog, std::size_t index, const EntryMetadata &metadata,
                  std::string_view canonical_url, const EffectiveLimits &limits)
{
    const ChannelView existing = (*catalog)[index];
    if (existing.url != canonical_url && !canonical_url.empty() &&
        !Contains(existing.alternate_urls, canonical_url) &&
        existing.alternate_urls.size() < limits.max_alternate_urls)
    {
        (void)catalog->AddAlternateUrl(index, canonical_url);
    }
    const auto fill = [&](Catalog::Field field, std::string_view current, const std::string &value)
    {
        if (current.empty() && !value.empty())
            (void)catalog->Set(index, field, value);
    };
    fill(Catalog::Field::name, existing.name, metadata.title);
    fill(Catalog::Field::tvg_id, existing.tvg_id, metadata.tvg_id);
    fill(Catalog::Field::tvg_name, existing.tvg_name, metadata.tvg_name);
    fill(Catalog::Field::tvg_logo, existing.tvg_logo, metadata.tvg_logo);
    fill(Catalog::Field::tvg_country, existing.tvg_country, metadata.tvg_country);
    fill(Catalog::Field::tvg_language, existing.tvg_language, metadata.tvg_language);
    fill(Catalog::Field::http_user_agent, existing.http_user_agent, metadata.http_user_agent);
    fill(Catalog::Field::http_referrer, existing.http_referrer, metadata.http_referrer);
    fill(Catalog::Field::catchup, existing.catchup, metadata.catchup);
    fill(Catalog::Field::catchup_source, existing.catchup_source, metadata.catchup_source);
    fill(Catalog::Field::catchup_days, existing.catchup_days, metadata.catchup_days);
    if (!metadata.group_title.empty())
    {
        if (existing.group_title.empty())
        {
            (void)catalog->Set(index, Catalog::Field::group_title, metadata.group_title);
        }
        else if (existing.group_title != metadata.group_title &&
                 !Contains(existing.alternate_group_titles, metadata.group_title) &&
                 existing.alternate_group_titles.size() < limits.max_alternate_groups)
        {
            (void)catalog->AddAlternateGroup(index, metadata.group_title);
        }
    }
}

} // namespace

struct M3uParser::State
{
    Catalog *catalog = nullptr;
    std::uint64_t source_id = 0;
    EffectiveLimits limits{};
    ParseReport local_report;
    ParseReport *report = nullptr;
    // Which channel a guide id or an address already belongs to.
    KeyTable by_tvg_id;
    KeyTable by_url;
    bool pending = false;
    PendingEntry entry;
    EntryMetadata defaults;
    std::size_t line_number = 0;
    std::size_t bytes_seen = 0;
    // The start of a line whose end has not arrived yet.
    std::string carry;
    bool carry_overlong = false;
    bool full = false;
    bool rejected = false;

    void Keep(std::string_view piece)
    {
        if (carry_overlong)
            return;
        // Room for the byte-order mark and the carriage return Line() takes off.
        if (carry.size() + piece.size() > limits.max_record_bytes + 4u)
        {
            carry.clear();
            carry_overlong = true;
            return;
        }
        carry.append(piece);
    }

    void Overlong()
    {
        ++line_number;
        ++report->lines_seen;
        pending = false;
        AddDiagnostic(report, limits, line_number, ParseIssueCode::overlong_record);
    }

    void NoRoom()
    {
        full = true;
        report->catalog_full = true;
        AddDiagnostic(report, limits, line_number, ParseIssueCode::catalog_full);
    }

    void Line(std::string_view line)
    {
        ++line_number;
        ++report->lines_seen;
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if (line_number == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xefu &&
            static_cast<unsigned char>(line[1]) == 0xbbu &&
            static_cast<unsigned char>(line[2]) == 0xbfu)
        {
            line.remove_prefix(3);
        }
        if (line.size() > limits.max_record_bytes)
        {
            pending = false;
            AddDiagnostic(report, limits, line_number, ParseIssueCode::overlong_record);
            return;
        }
        line = Trim(line);
        if (line.empty())
        {
            return;
        }
        if (line.front() == '#')
        {
            if (StartsWithInsensitive(line, "#EXTM3U"))
            {
                ParseIssueCode error{};
                EntryMetadata header;
                if (ParseExtinf("#EXTINF:-1 " + std::string(line.substr(7)) + ",Header",
                                limits.max_field_bytes, &header, &error))
                {
                    defaults = std::move(header);
                    std::string_view urls = defaults.guide_urls;
                    while (!urls.empty() && catalog->guide_urls.size() < 8)
                    {
                        const auto comma = urls.find(',');
                        std::string url;
                        if (CanonicalizeStreamUrl(Trim(urls.substr(0, comma)), &url) &&
                            std::find(catalog->guide_urls.begin(), catalog->guide_urls.end(),
                                      url) == catalog->guide_urls.end())
                            catalog->guide_urls.push_back(std::move(url));
                        if (comma == std::string_view::npos)
                            break;
                        urls.remove_prefix(comma + 1);
                    }
                }
            }
            if (StartsWithInsensitive(line, "#EXTINF:"))
            {
                if (pending)
                {
                    AddDiagnostic(report, limits, entry.line, ParseIssueCode::missing_url);
                    pending = false;
                }
                ParseIssueCode error = ParseIssueCode::malformed_extinf;
                EntryMetadata parsed;
                if (ParseExtinf(line, limits.max_field_bytes, &parsed, &error))
                {
                    if (parsed.catchup.empty())
                        parsed.catchup = defaults.catchup;
                    if (parsed.catchup_source.empty())
                        parsed.catchup_source = defaults.catchup_source;
                    if (parsed.catchup_days.empty())
                        parsed.catchup_days = defaults.catchup_days;
                    entry.metadata = std::move(parsed);
                    entry.line = static_cast<std::uint32_t>(std::min<std::size_t>(
                        line_number, std::numeric_limits<std::uint32_t>::max()));
                    pending = true;
                }
                else
                {
                    AddDiagnostic(report, limits, line_number, error);
                }
            }
            else if (pending)
            {
                (void)SetHttpOption(line, limits.max_field_bytes, &entry.metadata);
            }
            return;
        }
        if (!pending)
        {
            AddDiagnostic(report, limits, line_number, ParseIssueCode::url_without_extinf);
            return;
        }
        pending = false;

        std::string canonical_url;
        ParseIssueCode url_issue = ParseIssueCode::malformed_url;
        if (!UrlIssue(line, limits.max_url_bytes, &canonical_url, &url_issue))
        {
            AddDiagnostic(report, limits, line_number, url_issue);
            return;
        }

        const EntryMetadata &metadata = entry.metadata;
        const std::string normalized_tvg_id = LowerTrimmed(metadata.tvg_id);
        const std::uint64_t tvg_key = KeyOf(normalized_tvg_id);
        const std::uint64_t url_key = KeyOf(canonical_url);
        std::uint32_t existing = kNone;
        if (!normalized_tvg_id.empty())
            existing = by_tvg_id.Find(tvg_key);
        existing = std::min(existing, by_url.Find(url_key));

        if (existing != kNone)
        {
            MergeChannel(catalog, existing, metadata, canonical_url, limits);
            if (!normalized_tvg_id.empty())
                (void)by_tvg_id.AddOnce(tvg_key, existing);
            (void)by_url.AddOnce(url_key, existing);
            ++report->duplicates;
            return;
        }
        if (catalog->size() >= limits.max_channels)
        {
            NoRoom();
            return;
        }

        const std::string id =
            StableId(source_id, normalized_tvg_id.empty() ? "url:" + canonical_url
                                                          : "tvg:" + normalized_tvg_id);
        ChannelView channel;
        channel.id = id;
        channel.source_id = source_id;
        channel.url = canonical_url;
        channel.tvg_id = metadata.tvg_id;
        channel.tvg_name = metadata.tvg_name;
        channel.tvg_logo = metadata.tvg_logo;
        channel.group_title = metadata.group_title;
        channel.tvg_country = metadata.tvg_country;
        channel.tvg_language = metadata.tvg_language;
        channel.http_user_agent = metadata.http_user_agent;
        channel.http_referrer = metadata.http_referrer;
        channel.catchup = metadata.catchup;
        channel.catchup_source = metadata.catchup_source;
        channel.catchup_days = metadata.catchup_days;
        channel.name = !metadata.title.empty()      ? std::string_view(metadata.title)
                       : !metadata.tvg_name.empty() ? std::string_view(metadata.tvg_name)
                                                    : std::string_view(canonical_url);
        channel.source_line = entry.line;
        const std::uint32_t inserted = static_cast<std::uint32_t>(catalog->size());
        if (!catalog->Add(channel))
        {
            NoRoom();
            return;
        }
        if (!normalized_tvg_id.empty())
            (void)by_tvg_id.AddOnce(tvg_key, inserted);
        (void)by_url.AddOnce(url_key, inserted);
        ++report->accepted;
    }
};

M3uParser::M3uParser(Catalog *catalog, std::uint64_t source_id, const ParseLimits &limits,
                     ParseReport *report)
    : state_(new State)
{
    state_->catalog = catalog;
    state_->source_id = source_id;
    state_->limits = ClampLimits(limits);
    state_->report = report == nullptr ? &state_->local_report : report;
    *state_->report = ParseReport{};
    catalog->Clear();
    catalog->source_id = source_id;
}

M3uParser::~M3uParser() = default;

bool M3uParser::Feed(std::string_view bytes)
{
    State &state = *state_;
    if (state.rejected)
        return false;
    state.bytes_seen += bytes.size();
    if (state.bytes_seen > state.limits.max_playlist_bytes)
    {
        state.rejected = true;
        state.pending = false;
        state.carry.clear();
        state.catalog->Clear();
        state.report->accepted = 0;
        state.report->input_too_large = true;
        AddDiagnostic(state.report, state.limits, 0, ParseIssueCode::input_too_large);
        return false;
    }
    std::size_t position = 0;
    while (position < bytes.size())
    {
        const std::size_t newline = bytes.find('\n', position);
        if (newline == std::string_view::npos)
        {
            state.Keep(bytes.substr(position));
            break;
        }
        const std::string_view piece = bytes.substr(position, newline - position);
        position = newline + 1u;
        if (state.carry.empty() && !state.carry_overlong)
        {
            state.Line(piece);
            continue;
        }
        state.Keep(piece);
        if (state.carry_overlong)
            state.Overlong();
        else
            state.Line(state.carry);
        state.carry.clear();
        state.carry_overlong = false;
    }
    return true;
}

void M3uParser::Finish()
{
    State &state = *state_;
    if (state.rejected)
        return;
    if (state.carry_overlong)
        state.Overlong();
    else if (!state.carry.empty())
        state.Line(state.carry);
    state.carry.clear();
    state.carry_overlong = false;
    if (state.pending)
    {
        AddDiagnostic(state.report, state.limits, state.entry.line, ParseIssueCode::missing_url);
        state.pending = false;
    }
}

bool M3uParser::full() const
{
    return state_->full;
}

Catalog ParseExtendedM3u(std::string_view input, std::uint64_t source_id, const ParseLimits &limits,
                         ParseReport *report)
{
    Catalog catalog;
    M3uParser parser(&catalog, source_id, limits, report);
    if (parser.Feed(input))
        parser.Finish();
    return catalog;
}

} // namespace iptv
