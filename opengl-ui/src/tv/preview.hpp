// ProsperoTV - A delayed, muted picture in the channel's television.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tv/images.hpp"
#include "tv/model.hpp"
#include "iptv_native_backend.h"
#include <optional>

namespace ptv
{
// Linear 4:2:0 to an aspect-correct RGBA picture, at most 640 x 360.
bool preview_pixels(const iptv_native_picture_t &picture, ImagePixels *pixels);

class LivePreview
{
  public:
    using Upload = std::function<std::uint32_t(std::uint32_t, const ImagePixels &)>;
    ~LivePreview();
    void configure(Upload upload, ImageCache::Release release);
    void update(std::optional<PlayRequest> request, float dt);
    ImageTexture find(std::string_view channel) const;
    // Focus changes cancel asynchronously. Closing the menu joins its worker.
    void clear();

  private:
    static void *work(void *self);
    void release_texture();
    Upload upload_;
    ImageCache::Release release_;
    ImageTexture texture_;
    std::optional<PlayRequest> wanted_;
    PlayRequest playing_;
    float delay_ = 0;
    bool attempted_ = false;
    void *thread_ = nullptr;
    std::atomic<bool> stop_{false}, done_{false}, ready_{false};
    ImagePixels pending_;
};
} // namespace ptv

namespace ptv::platform
{
// Owns an independent connection and decoder until cancellation or failure.
// The callback runs on the decoder thread. It never opens an audio port.
void preview(const PlayRequest &request, const iptv::http::RequestControl &control,
             void (*picture)(void *, const iptv_native_picture_t *), void *context);
} // namespace ptv::platform
