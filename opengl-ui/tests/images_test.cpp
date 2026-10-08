// ProsperoTV - Artwork limits, asynchronous lifetime, and cache eviction.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/images.hpp"
#include "host_platform.hpp"
#include <gtest/gtest.h>
#include <png.h>
#include <cstdio>
#include <jpeglib.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
std::vector<std::uint8_t> png_bytes(unsigned width, unsigned height)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 255);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i] = 32;
        pixels[i + 2] = 64;
        pixels[i + 3] = 128;
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = width;
    png.height = height;
    png.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    EXPECT_NE(png_image_write_to_memory(&png, nullptr, &size, 0, pixels.data(), 0, nullptr), 0);
    std::vector<std::uint8_t> bytes(size);
    EXPECT_NE(png_image_write_to_memory(&png, bytes.data(), &size, 0, pixels.data(), 0, nullptr),
              0);
    bytes.resize(size);
    png_image_free(&png);
    return bytes;
}

TEST(Images, PngKeepsAspectRatioAndTransparencyAndRejectsBadData)
{
    ptv::ImagePixels image;
    const auto bytes = png_bytes(512, 128);
    ASSERT_TRUE(ptv::decode_image(bytes, &image));
    EXPECT_EQ(image.width, 256);
    EXPECT_EQ(image.height, 64);
    ASSERT_EQ(image.rgba.size(), 256u * 64u * 4u);
    EXPECT_EQ(image.rgba[0], 32);
    EXPECT_EQ(image.rgba[1], 255);
    EXPECT_EQ(image.rgba[2], 64);
    EXPECT_EQ(image.rgba[3], 128);
    EXPECT_FALSE(ptv::decode_image(std::span(bytes).first(bytes.size() / 2), &image));
    EXPECT_FALSE(ptv::decode_image(png_bytes(1025, 1), &image));
    EXPECT_FALSE(ptv::decode_image(std::vector<std::uint8_t>(2u * 1024u * 1024u + 1), &image));
    EXPECT_EQ(image.width, 256); // failed decoding does not replace a good image
}

TEST(Images, JpegDecodesWithoutAllowingTruncatedPictures)
{
    jpeg_compress_struct encoder{};
    jpeg_error_mgr error{};
    encoder.err = jpeg_std_error(&error);
    jpeg_create_compress(&encoder);
    unsigned char *memory = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&encoder, &memory, &size);
    encoder.image_width = 8;
    encoder.image_height = 4;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_start_compress(&encoder, TRUE);
    unsigned char row[8 * 3];
    std::fill(std::begin(row), std::end(row), 150);
    while (encoder.next_scanline < encoder.image_height)
    {
        JSAMPROW rows[] = {row};
        jpeg_write_scanlines(&encoder, rows, 1);
    }
    jpeg_finish_compress(&encoder);
    ptv::ImagePixels image;
    EXPECT_TRUE(ptv::decode_image({memory, size}, &image));
    EXPECT_EQ(image.width, 8);
    EXPECT_EQ(image.height, 4);
    EXPECT_EQ(image.rgba[3], 255);
    EXPECT_FALSE(ptv::decode_image({memory, size - 8}, &image));
    std::free(memory);
    jpeg_destroy_compress(&encoder);
}

TEST(Images, CacheUploadsOnCallerThreadBoundsTexturesAndCancels)
{
    host::reset();
    char pattern[] = "/tmp/prosperotv-images-XXXXXX";
    const std::string dir = mkdtemp(pattern);
    const auto bytes = png_bytes(32, 16);
    const std::string file = dir + "/logo.png";
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    host::set_network(true, "", 0);
    host::set_unix_time(1000);
    unsigned uploads = 0, released = 0;
    const auto owner = std::this_thread::get_id();
    ptv::ImageCache cache;
    cache.configure(
        [&](const ptv::ImagePixels &image)
        {
            EXPECT_EQ(std::this_thread::get_id(), owner);
            EXPECT_EQ(image.width, 32);
            return ++uploads;
        },
        [&](std::uint32_t)
        {
            EXPECT_EQ(std::this_thread::get_id(), owner);
            ++released;
        });
    for (int i = 0; i < 70; ++i)
    {
        const auto url = "https://logos.invalid/" + std::to_string(i);
        host::set_network_response(url, file);
        for (int wait = 0; wait < 500 && cache.find(url).id == 0; ++wait)
        {
            cache.update({url});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_NE(cache.find(url).id, 0u);
        cache.update({url, url});
        EXPECT_EQ(uploads, static_cast<unsigned>(i + 1));
    }
    EXPECT_EQ(released, 6u);
    EXPECT_EQ(cache.find("https://logos.invalid/0").id, 0u);
    const auto slow = "https://logos.invalid/slow";
    host::set_network_response(slow, file);
    host::set_network(true, "", 10000);
    cache.update({slow});
    const auto start = std::chrono::steady_clock::now();
    cache.clear();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    EXPECT_EQ(released, uploads);
    host::reset();
    std::filesystem::remove_all(dir);
}
} // namespace
