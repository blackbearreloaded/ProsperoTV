// ProsperoTV - PNG/JPEG decoding and a small cache for the pictures in view.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/images.hpp"
#include "tv/platform.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <memory>
#include <jpeglib.h>
#include <png.h>

namespace ptv
{
namespace
{
constexpr std::size_t kMaxBytes = 2u * 1024u * 1024u;
constexpr std::size_t kCacheSize = 64;
bool dimensions(unsigned width, unsigned height)
{
    return width > 0 && height > 0 && width <= 1024 && height <= 1024;
}
struct JpegError
{
    jpeg_error_mgr base;
    std::jmp_buf jump;
};
void jpeg_error(j_common_ptr decoder)
{
    auto *error = reinterpret_cast<JpegError *>(decoder->err);
    std::longjmp(error->jump, 1);
}
void jpeg_quiet(j_common_ptr)
{
}
bool decode_jpeg(std::span<const std::uint8_t> bytes, ImagePixels *out)
{
    // Objects that own memory live outside the setjmp frame. libjpeg errors
    // must not skip a C++ destructor or terminate the application.
    // Decoder fields change after setjmp; heap storage keeps them defined
    // when libjpeg jumps back after an error.
    struct State
    {
        jpeg_decompress_struct decoder{};
        JpegError error{};
    };
    auto state = std::make_unique<State>();
    auto &decoder = state->decoder;
    auto &error = state->error;
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error;
    error.base.output_message = jpeg_quiet;
    if (setjmp(error.jump))
    {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, bytes.data(), static_cast<unsigned long>(bytes.size()));
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK ||
        !dimensions(decoder.image_width, decoder.image_height))
    {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    out->width = static_cast<int>(decoder.output_width);
    out->height = static_cast<int>(decoder.output_height);
    out->rgba.resize(static_cast<std::size_t>(out->width) * out->height * 4);
    unsigned char row[1024 * 3];
    while (decoder.output_scanline < decoder.output_height)
    {
        const auto y = decoder.output_scanline;
        JSAMPROW rows[] = {row};
        if (jpeg_read_scanlines(&decoder, rows, 1) != 1)
        {
            jpeg_destroy_decompress(&decoder);
            return false;
        }
        for (int x = 0; x < out->width; ++x)
        {
            auto *pixel = out->rgba.data() + (static_cast<std::size_t>(y) * out->width + x) * 4;
            std::memcpy(pixel, row + x * 3, 3);
            pixel[3] = 255;
        }
    }
    const bool ok = jpeg_finish_decompress(&decoder) != 0 && error.base.num_warnings == 0;
    jpeg_destroy_decompress(&decoder);
    return ok;
}
} // namespace

bool decode_image(std::span<const std::uint8_t> bytes, ImagePixels *image)
{
    if (image == nullptr || bytes.size() < 8 || bytes.size() > kMaxBytes)
        return false;
    ImagePixels decoded;
    if (png_sig_cmp(bytes.data(), 0, 8) == 0)
    {
        png_image png{};
        png.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size()))
        {
            png_image_free(&png);
            return false;
        }
        if (!dimensions(png.width, png.height))
        {
            png_image_free(&png);
            return false;
        }
        png.format = PNG_FORMAT_RGBA;
        decoded.width = static_cast<int>(png.width);
        decoded.height = static_cast<int>(png.height);
        decoded.rgba.resize(PNG_IMAGE_SIZE(png));
        const bool ok = png_image_finish_read(&png, nullptr, decoded.rgba.data(), 0, nullptr) != 0;
        png_image_free(&png);
        if (!ok)
            return false;
    }
    else if (bytes[0] != 0xff || bytes[1] != 0xd8 || !decode_jpeg(bytes, &decoded))
        return false;

    const int longest = std::max(decoded.width, decoded.height);
    if (longest <= 256)
    {
        *image = std::move(decoded);
        return true;
    }
    ImagePixels scaled;
    scaled.width = std::max(1, decoded.width * 256 / longest);
    scaled.height = std::max(1, decoded.height * 256 / longest);
    scaled.rgba.resize(static_cast<std::size_t>(scaled.width) * scaled.height * 4);
    // Area average in premultiplied alpha keeps transparent logo edges clean.
    for (int y = 0; y < scaled.height; ++y)
        for (int x = 0; x < scaled.width; ++x)
        {
            unsigned color[3]{}, alpha = 0, count = 0;
            for (int sy = y * decoded.height / scaled.height;
                 sy < (y + 1) * decoded.height / scaled.height; ++sy)
                for (int sx = x * decoded.width / scaled.width;
                     sx < (x + 1) * decoded.width / scaled.width; ++sx)
                {
                    const auto *p = decoded.rgba.data() + (sy * decoded.width + sx) * 4;
                    for (int c = 0; c < 3; ++c)
                        color[c] += p[c] * p[3];
                    alpha += p[3];
                    ++count;
                }
            auto *p = scaled.rgba.data() + (y * scaled.width + x) * 4;
            for (int c = 0; c < 3; ++c)
                p[c] = alpha == 0 ? 0 : static_cast<std::uint8_t>(color[c] / alpha);
            p[3] = static_cast<std::uint8_t>(alpha / count);
        }
    *image = std::move(scaled);
    return true;
}

ImageCache::~ImageCache()
{
    clear();
}
void ImageCache::configure(Upload upload, Release release)
{
    clear();
    upload_ = std::move(upload);
    release_ = std::move(release);
}
void ImageCache::clear()
{
    stop_.store(true);
    if (thread_ != nullptr)
        platform::thread_join(thread_);
    thread_ = nullptr;
    for (const auto &[url, entry] : entries_)
        if (entry.texture.id != 0 && release_)
            release_(entry.texture.id);
    entries_.clear();
    pending_ = {};
}
ImageTexture ImageCache::find(std::string_view url) const
{
    const auto found = entries_.find(std::string(url));
    return found == entries_.end() ? ImageTexture{} : found->second.texture;
}
void *ImageCache::work(void *self)
{
    auto &cache = *static_cast<ImageCache *>(self);
    std::vector<std::uint8_t> bytes;
    const iptv::http::RequestControl control{
        [](void *context) { return static_cast<ImageCache *>(context)->stop_.load(); }, &cache};
    if (platform::fetch_image(cache.fetching_.c_str(), &bytes, &control) && !cache.stop_.load())
        (void)decode_image(bytes, &cache.pending_);
    cache.done_.store(true);
    return nullptr;
}
void ImageCache::update(const std::vector<std::string> &urls)
{
    if (!upload_ || !release_)
        return;
    const auto now = platform::unix_time();
    if (thread_ != nullptr && done_.load())
    {
        platform::thread_join(thread_);
        thread_ = nullptr;
        auto &entry = entries_[fetching_];
        if (!pending_.rgba.empty())
            entry.texture = {upload_(pending_), pending_.width, pending_.height};
        entry.retry = now + 60;
        pending_ = {};
    }
    ++tick_;
    for (const auto &url : urls)
        if (const auto found = entries_.find(url); found != entries_.end())
            found->second.used = tick_;
    if (thread_ != nullptr)
    {
        if (std::find(urls.begin(), urls.end(), fetching_) == urls.end())
            stop_.store(true);
        return;
    }
    for (const auto &url : urls)
    {
        if (url.empty() || url.size() > iptv::http::kMaxUrlBytes ||
            !iptv::http::IsSupportedPlaylistUrl(url.c_str()))
            continue;
        const auto found = entries_.find(url);
        if (found != entries_.end() && (found->second.texture.id != 0 || found->second.retry > now))
            continue;
        if (found == entries_.end() && entries_.size() >= kCacheSize)
        {
            const auto oldest =
                std::min_element(entries_.begin(), entries_.end(), [](const auto &a, const auto &b)
                                 { return a.second.used < b.second.used; });
            if (oldest->second.used == tick_)
                return;
            if (oldest->second.texture.id != 0)
                release_(oldest->second.texture.id);
            entries_.erase(oldest);
        }
        auto &entry = entries_[url];
        entry.used = tick_;
        entry.retry = now + 60;
        fetching_ = url;
        done_.store(false);
        stop_.store(false);
        thread_ = platform::thread_start(work, this, 1024u * 1024u, "ptv-image");
        return;
    }
}
} // namespace ptv
