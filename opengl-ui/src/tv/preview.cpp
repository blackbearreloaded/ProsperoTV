// ProsperoTV - Preview lifetime, frame mailbox and bounded colour conversion.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/preview.hpp"
#include "tv/platform.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace ptv
{
bool preview_pixels(const iptv_native_picture_t &p, ImagePixels *pixels)
{
    if (!pixels || !p.data || !p.width || !p.height || p.width > 3840 || p.height > 2160 ||
        p.pitch < p.width || p.pitch > 4096 || (p.pitch & 1) || p.surface_height < p.height ||
        p.surface_height > 2304 || (p.bit_depth != 8 && p.bit_depth != 10))
        return false;
    const unsigned component = p.bit_depth == 10 ? 2 : 1;
    const std::size_t luma = std::size_t(p.pitch) * p.surface_height;
    const std::size_t needed =
        (luma + std::size_t(p.pitch) * ((p.surface_height + 1) / 2)) * component;
    if (p.bytes < needed)
        return false;
    const float scale = std::min({1.0f, 640.0f / p.width, 360.0f / p.height});
    pixels->width = std::max(1, static_cast<int>(p.width * scale));
    pixels->height = std::max(1, static_cast<int>(p.height * scale));
    pixels->rgba.resize(std::size_t(pixels->width) * pixels->height * 4);
    const auto *source = static_cast<const std::uint8_t *>(p.data);
    const auto sample = [&](std::size_t offset) -> int
    {
        if (component == 1)
            return source[offset];
        // P010 stores its ten bits in the high bits of each little-endian word.
        return source[offset * 2 + 1];
    };
    const auto byte = [](int value)
    { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); };
    for (int y = 0; y < pixels->height; ++y)
    {
        const unsigned sy = unsigned(y) * p.height / unsigned(pixels->height);
        for (int x = 0; x < pixels->width; ++x)
        {
            const unsigned sx = unsigned(x) * p.width / unsigned(pixels->width);
            const int yy = sample(std::size_t(sy) * p.pitch + sx) - 16;
            const std::size_t uv = luma + std::size_t(sy / 2) * p.pitch + (sx & ~1u);
            const int u = sample(uv) - 128, v = sample(uv + 1) - 128;
            auto *out = pixels->rgba.data() + (std::size_t(y) * pixels->width + x) * 4;
            // Broadcast HD uses limited-range BT.709; SD uses BT.601.
            const bool hd = p.width >= 1280 || p.height > 576;
            out[0] = byte((298 * yy + (hd ? 459 : 409) * v + 128) >> 8);
            out[1] = byte((298 * yy - (hd ? 55 : 100) * u - (hd ? 136 : 208) * v + 128) >> 8);
            out[2] = byte((298 * yy + (hd ? 541 : 516) * u + 128) >> 8);
            out[3] = 255;
        }
    }
    return true;
}

LivePreview::~LivePreview()
{
    clear();
}
void LivePreview::configure(Upload upload, ImageCache::Release release)
{
    clear();
    upload_ = std::move(upload);
    release_ = std::move(release);
}
void LivePreview::release_texture()
{
    if (texture_.id && release_)
        release_(texture_.id);
    texture_ = {};
}
ImageTexture LivePreview::find(std::string_view channel) const
{
    return wanted_ && wanted_->channel_id == channel && playing_.channel_id == channel &&
                   !stop_.load()
               ? texture_
               : ImageTexture{};
}
void LivePreview::update(std::optional<PlayRequest> request, float dt)
{
    if (!upload_)
        return;
    const bool changed =
        wanted_.has_value() != request.has_value() ||
        (wanted_ &&
         (wanted_->channel_id != request->channel_id || wanted_->source_id != request->source_id ||
          wanted_->urls != request->urls || wanted_->user_agent != request->user_agent ||
          wanted_->referrer != request->referrer ||
          wanted_->portal_command != request->portal_command));
    if (changed)
    {
        stop_.store(true);
        wanted_ = std::move(request);
        delay_ = 0;
        attempted_ = false;
        release_texture();
    }
    if (thread_ && ready_.load() && !stop_.load())
    {
        if (texture_.width != pending_.width || texture_.height != pending_.height)
            release_texture();
        texture_ = {upload_(texture_.id, pending_), pending_.width, pending_.height};
        ready_.store(false);
    }
    if (thread_ && done_.load())
    {
        platform::thread_join(thread_);
        thread_ = nullptr;
        ready_.store(false);
    }
    delay_ += std::max(0.0f, dt);
    if (!thread_ && wanted_ && !attempted_ && delay_ >= 1.2f)
    {
        attempted_ = true;
        playing_ = *wanted_;
        stop_.store(false);
        done_.store(false);
        ready_.store(false);
        thread_ = platform::thread_start(work, this, 2u * 1024u * 1024u, "ptv-preview");
    }
}
void LivePreview::clear()
{
    stop_.store(true);
    if (thread_)
        platform::thread_join(thread_);
    thread_ = nullptr;
    ready_.store(false);
    wanted_.reset();
    playing_ = {};
    attempted_ = false;
    delay_ = 0;
    pending_ = {};
    release_texture();
}
void *LivePreview::work(void *self)
{
    auto &p = *static_cast<LivePreview *>(self);
    struct Receiver
    {
        LivePreview &preview;
        std::chrono::steady_clock::time_point previous{};
    } receiver{p};
    const iptv::http::RequestControl control{
        [](void *self) { return static_cast<LivePreview *>(self)->stop_.load(); }, &p};
    platform::preview(
        p.playing_, control,
        [](void *self, const iptv_native_picture_t *picture)
        {
            auto &r = *static_cast<Receiver *>(self);
            auto &p = r.preview;
            const auto now = std::chrono::steady_clock::now();
            if (p.stop_.load() || p.ready_.load() ||
                now - r.previous < std::chrono::milliseconds(80))
                return;
            if (preview_pixels(*picture, &p.pending_))
            {
                r.previous = now;
                p.ready_.store(true);
            }
        },
        &receiver);
    p.done_.store(true);
    return nullptr;
}
} // namespace ptv
