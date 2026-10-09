// ProsperoTV - A delayed, muted picture in the channel's television.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tv/images.hpp"
#include "tv/model.hpp"
#include "iptv_native_backend.h"
#include <atomic>
#include <optional>

namespace ptv
{
// Linear 4:2:0 to an aspect-correct RGBA picture, up to 960 x 540 for multiview.
bool preview_pixels(const iptv_native_picture_t &picture, ImagePixels *pixels,
                    unsigned max_width = 640);

struct PreviewAudio
{
    std::atomic<bool> requested{false}, active{false}, failed{false};
    // One audible preview across all mosaics, including an in-flight port open.
    bool claim()
    {
        PreviewAudio *previous = nullptr;
        return owner_.compare_exchange_strong(previous, this) || previous == this;
    }
    void release()
    {
        active.store(false);
        PreviewAudio *previous = this;
        owner_.compare_exchange_strong(previous, nullptr);
    }

  private:
    inline static std::atomic<PreviewAudio *> owner_{nullptr};
};

class LivePreview
{
  public:
    using Upload = std::function<std::uint32_t(std::uint32_t, const ImagePixels &)>;
    ~LivePreview();
    void configure(Upload upload, ImageCache::Release release, bool multiview = false);
    void update(std::optional<PlayRequest> request, float dt);
    ImageTexture find(std::string_view channel) const;
    // Focus changes cancel asynchronously. Closing the menu joins its worker.
    void clear();
    void request_stop()
    {
        audio_.requested.store(false);
        stop_.store(true);
    }
    void set_audio(bool audible)
    {
        audio_.requested.store(audible);
    }
    bool audio_active() const
    {
        return audio_.active.load();
    }
    bool audio_failed() const
    {
        return audio_.failed.load();
    }
    bool finished() const
    {
        return attempted_ && !thread_;
    }

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
    bool multiview_ = false;
    PreviewAudio audio_;
};
} // namespace ptv

namespace ptv::platform
{
// Owns an independent connection and decoder until cancellation or failure.
// The callback runs on the decoder thread. Audio is opt-in for multiview;
// ordinary menu previews pass null and never open an audio port.
void preview(const PlayRequest &request, const iptv::http::RequestControl &control,
             void (*picture)(void *, const iptv_native_picture_t *), void *context,
             PreviewAudio *audio = nullptr);
} // namespace ptv::platform
