// ProsperoTV - Bounded, asynchronous channel artwork.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ptv
{
struct ImagePixels
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
// PNG and JPEG, at most 2 MiB compressed and 1024 x 1024 decoded. The
// returned picture fits within 256 x 256, retaining its aspect ratio.
bool decode_image(std::span<const std::uint8_t> bytes, ImagePixels *image);

struct ImageTexture
{
    std::uint32_t id = 0;
    int width = 0, height = 0;
};

class ImageCache
{
  public:
    using Upload = std::function<std::uint32_t(const ImagePixels &)>;
    using Release = std::function<void(std::uint32_t)>;
    ~ImageCache();
    void configure(Upload upload, Release release);
    // Called on the renderer thread. Only these visible pictures are fetched;
    // the first URL is the focused channel, so its large picture comes first.
    void update(const std::vector<std::string> &urls);
    ImageTexture find(std::string_view url) const;
    void clear();

  private:
    struct Entry
    {
        ImageTexture texture;
        std::uint64_t used = 0, retry = 0;
    };
    static void *work(void *self);
    Upload upload_;
    Release release_;
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t tick_ = 0;
    void *thread_ = nullptr;
    std::atomic<bool> done_{false}, stop_{false};
    std::string fetching_;
    ImagePixels pending_;
};
} // namespace ptv
