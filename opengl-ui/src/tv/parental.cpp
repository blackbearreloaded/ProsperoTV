// ProsperoTV - Persistent parental PIN and category policy.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/parental.hpp"
#include "tv/category_path.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ptv
{
namespace
{
std::string lower(std::string_view text)
{
    std::string out(text);
    for (auto &c : out)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return out;
}
bool category_has(std::string_view category, bool adult)
{
    for (category = category_trim(category); !category.empty();
         category = category_parent(category))
    {
        const auto leaf = lower(category_leaf(category));
        if (adult ? leaf == "adult" || leaf == "adults" || leaf == "xxx" || leaf == "18+" ||
                        leaf == "+18"
                  : leaf == "kids" || leaf == "children" || leaf == "children's")
            return true;
    }
    return false;
}
std::string hex(const auto &bytes)
{
    std::string text;
    for (auto byte : bytes)
    {
        text += "0123456789abcdef"[byte >> 4];
        text += "0123456789abcdef"[byte & 15];
    }
    return text;
}
bool unhex(std::string_view text, auto &bytes)
{
    if (text.size() != bytes.size() * 2)
        return false;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        unsigned value = 0;
        const auto parsed =
            std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + i * 2 + 2)
            return false;
        bytes[i] = static_cast<unsigned char>(value);
    }
    return true;
}
bool derive(std::string_view pin, const auto &salt, auto &hash)
{
    return PKCS5_PBKDF2_HMAC(pin.data(), static_cast<int>(pin.size()), salt.data(),
                             static_cast<int>(salt.size()), 200000, EVP_sha256(),
                             static_cast<int>(hash.size()), hash.data()) == 1;
}
} // namespace
bool adult_category(std::string_view category)
{
    return category_has(category, true);
}
bool kids_category(std::string_view category)
{
    return category_has(category, false);
}

bool Parental::valid_pin(std::string_view pin)
{
    return pin.size() >= 4 && pin.size() <= 8 &&
           std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool Parental::load(std::string file)
{
    *this = {};
    file_ = std::move(file);
    const int fd = ::open(file_.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return valid_ = errno == ENOENT;
    struct stat st
    {
    };
    char data[192]{};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 192;
    if (ok)
        ok = read(fd, data, static_cast<std::size_t>(st.st_size)) == st.st_size;
    close(fd);
    char salt[33]{}, hash[65]{};
    unsigned kids = 0, failures = 0;
    unsigned long long until = 0;
    int used = 0;
    ok = ok &&
         std::sscanf(data, "PTV_PARENTAL_1\n%32[0-9a-f]\n%64[0-9a-f]\n%u %u %llu\n%n", salt, hash,
                     &kids, &failures, &until, &used) == 5 &&
         used == st.st_size && kids <= 1 && failures < 5 && until <= UINT64_C(253402300799) &&
         unhex(salt, salt_) && unhex(hash, hash_);
    enabled_ = true; // A malformed existing file never turns protection off.
    valid_ = ok;
    kids_ = kids != 0;
    failures_ = failures;
    until_ = until;
    return ok;
}
bool Parental::save()
{
    const std::string text = "PTV_PARENTAL_1\n" + hex(salt_) + '\n' + hex(hash_) + '\n' +
                             std::to_string(kids_ ? 1 : 0) + ' ' + std::to_string(failures_) + ' ' +
                             std::to_string(until_) + '\n';
    const auto temporary = file_ + ".tmp";
    // Profile storage is private. Refuse links and remove only our prior temporary file.
    if (unlink(temporary.c_str()) != 0 && errno != ENOENT)
        return false;
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    bool ok =
        write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (ok)
        ok = rename(temporary.c_str(), file_.c_str()) == 0;
    if (!ok)
        unlink(temporary.c_str());
    return ok;
}
bool Parental::set_pin(std::string_view pin)
{
    if (!unlocked() || !valid_pin(pin))
        return false;
    auto next = *this;
    if (RAND_bytes(next.salt_.data(), static_cast<int>(next.salt_.size())) != 1 ||
        !derive(pin, next.salt_, next.hash_))
        return false;
    next.enabled_ = true;
    next.unlocked_ = false;
    next.failures_ = 0;
    next.until_ = 0;
    if (!next.save())
        return false;
    *this = std::move(next);
    return true;
}
bool Parental::unlock(std::string_view pin, std::uint64_t now)
{
    if (!valid_ || !enabled_ || now == 0 || now < until_)
        return false;
    std::array<unsigned char, 32> hash{};
    const bool match = valid_pin(pin) && derive(pin, salt_, hash) &&
                       CRYPTO_memcmp(hash.data(), hash_.data(), hash.size()) == 0;
    OPENSSL_cleanse(hash.data(), hash.size());
    auto next = *this;
    next.unlocked_ = false;
    if (match)
    {
        next.failures_ = 0;
        next.until_ = 0;
    }
    else if (++next.failures_ >= 5)
    {
        next.failures_ = 0;
        next.until_ = now + 30;
    }
    // A storage failure cannot reset the retry count by reopening the app.
    if (!next.save())
    {
        valid_ = false;
        unlocked_ = false;
        return false;
    }
    next.unlocked_ = match;
    *this = std::move(next);
    return match;
}
bool Parental::set_kids(bool enabled)
{
    if (!enabled_ || !unlocked())
        return false;
    auto next = *this;
    next.kids_ = enabled;
    if (!next.save())
        return false;
    if (enabled)
        next.lock();
    *this = std::move(next);
    return true;
}
bool Parental::remove()
{
    if (!unlocked() || unlink(file_.c_str()) != 0)
        return false;
    const auto file = file_;
    *this = {};
    file_ = file;
    return true;
}
} // namespace ptv
