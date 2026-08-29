/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <SDL2/SDL.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/RenderInterfaceCompatibility.h>
#include <RmlUi/Core/SystemInterface.h>

#include "iptv_app.h"
#include "iptv_input.h"
#include "iptv_memory.h"
#include "iptv_player.h"
#include "bitmap_font_engine.h"

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <pthread.h>
#include <vector>

extern "C" int sceKernelUsleep(std::uint32_t microseconds);
extern "C" int sceKernelSendNotificationRequest(std::uint32_t device, void *request,
                                                std::size_t size, int blocking);
extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" void *mmap(void *address, std::size_t length, int protection, int flags, int descriptor,
                      long offset);
extern "C" int munmap(void *address, std::size_t length);
extern "C" long write(int descriptor, const void *buffer, std::size_t bytes);
extern "C" void *__dso_handle = nullptr;
extern "C" char __eh_frame_hdr_start[1] = {};
extern "C" char __eh_frame_hdr_end[1] = {};
extern "C" char __eh_frame_start[1] = {};
extern "C" char __eh_frame_end[1] = {};

extern "C" void __assert(const char *, const char *, int, const char *)
{
    std::abort();
}

extern "C" char *strcasestr(const char *haystack, const char *needle)
{
    if (!*needle)
        return const_cast<char *>(haystack);
    for (; *haystack; ++haystack)
    {
        const char *left = haystack;
        const char *right = needle;
        while (*left && *right)
        {
            const char left_char =
                *left >= 'A' && *left <= 'Z' ? static_cast<char>(*left + ('a' - 'A')) : *left;
            const char right_char =
                *right >= 'A' && *right <= 'Z' ? static_cast<char>(*right + ('a' - 'A')) : *right;
            if (left_char != right_char)
                break;
            ++left;
            ++right;
        }
        if (!*right)
            return const_cast<char *>(haystack);
    }
    return nullptr;
}

extern "C" float strtof(const char *value, char **end)
{
    return static_cast<float>(strtod(value, end));
}

extern "C" int fseek(std::FILE *file, long offset, int origin)
{
    return fseeko(file, offset, origin);
}

extern "C" long ftell(std::FILE *file)
{
    return static_cast<long>(ftello(file));
}

namespace
{

struct NotificationRequest
{
    std::uint8_t reserved[45];
    char message[3075];
};

void LauncherFailure(const char *stage)
{
    NotificationRequest request{};
    std::snprintf(request.message, sizeof(request.message), "IPTV launcher failed: %s",
                  stage ? stage : "unknown stage");
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

void WriteStdout(const char *text, std::size_t bytes)
{
    while (text && bytes)
    {
        const long written = write(1, text, bytes);
        if (written <= 0)
            return;
        text += static_cast<std::size_t>(written);
        bytes -= static_cast<std::size_t>(written);
    }
}

constexpr std::size_t kMappedAllocationThreshold = 64 * 1024;
constexpr std::uint64_t kAllocationMagic = UINT64_C(0x495054564d454d);
constexpr std::uint64_t kCatalogArenaMagic = UINT64_C(0x495054564152454e);
constexpr int kProtectionReadWrite = 3;
constexpr int kMapPrivateAnonymous = 0x1002;

struct CatalogArena;

struct alignas(std::max_align_t) AllocationHeader
{
    std::uint64_t magic;
    std::size_t requested_size;
    std::size_t mapped_size;
    CatalogArena *arena;
};

struct alignas(std::max_align_t) CatalogArena
{
    std::uint64_t magic;
    std::size_t mapped_size;
    std::size_t capacity;
    std::size_t offset;
    std::size_t live_allocations;
    int retired;
};

CatalogArena *g_catalog_arena = nullptr;
int g_catalog_arena_lock = 0;

void LockCatalogArena()
{
    while (__atomic_test_and_set(&g_catalog_arena_lock, __ATOMIC_ACQUIRE))
    {
    }
}

void UnlockCatalogArena()
{
    __atomic_clear(&g_catalog_arena_lock, __ATOMIC_RELEASE);
}

std::size_t AlignAllocation(std::size_t value)
{
    constexpr std::size_t alignment = alignof(std::max_align_t);
    return (value + alignment - 1u) & ~(alignment - 1u);
}

AllocationHeader *AllocateFromCatalogArena(std::size_t size)
{
    AllocationHeader *header = nullptr;
    LockCatalogArena();
    CatalogArena *arena = g_catalog_arena;
    if (arena && arena->magic == kCatalogArenaMagic && !arena->retired)
    {
        const std::size_t total = AlignAllocation(sizeof(AllocationHeader) + size);
        if (arena->offset <= arena->capacity && total <= arena->capacity - arena->offset)
        {
            header = reinterpret_cast<AllocationHeader *>(reinterpret_cast<unsigned char *>(arena) +
                                                          arena->offset);
            arena->offset += total;
            ++arena->live_allocations;
            header->arena = arena;
        }
    }
    UnlockCatalogArena();
    return header;
}

void *AllocateTracked(std::size_t size)
{
    if (size == 0)
        size = 1;
    constexpr std::size_t alignment_slack = alignof(std::max_align_t) - 1u;
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) - alignment_slack)
        return nullptr;

    const std::size_t total = sizeof(AllocationHeader) + size;
    AllocationHeader *header = AllocateFromCatalogArena(size);
    const bool arena_allocation = header != nullptr;
    std::size_t mapped_size = 0;
    if (!header && size >= kMappedAllocationThreshold)
    {
        mapped_size = (total + 0x3fff) & ~std::size_t(0x3fff);
        void *mapping =
            mmap(nullptr, mapped_size, kProtectionReadWrite, kMapPrivateAnonymous, -1, 0);
        if (mapping != reinterpret_cast<void *>(-1))
            header = static_cast<AllocationHeader *>(mapping);
    }
    else if (!header)
    {
        header = static_cast<AllocationHeader *>(std::malloc(total));
    }
    if (!header)
        return nullptr;

    header->magic = kAllocationMagic;
    header->requested_size = size;
    header->mapped_size = mapped_size;
    if (!arena_allocation)
        header->arena = nullptr;
    return header + 1;
}

void FreeTracked(void *allocation) noexcept
{
    if (!allocation)
        return;
    auto *header = static_cast<AllocationHeader *>(allocation) - 1;
    if (header->magic != kAllocationMagic)
    {
        std::free(allocation);
        return;
    }
    if (header->arena)
    {
        CatalogArena *arena = header->arena;
        std::size_t mapped_size = 0;
        bool release = false;
        LockCatalogArena();
        if (arena->magic == kCatalogArenaMagic && arena->live_allocations)
        {
            --arena->live_allocations;
            release = arena->retired && arena->live_allocations == 0;
            mapped_size = arena->mapped_size;
        }
        UnlockCatalogArena();
        if (release)
            munmap(arena, mapped_size);
        return;
    }
    if (header->mapped_size != 0)
        munmap(header, header->mapped_size);
    else
        std::free(header);
}

void *CallocTracked(std::size_t count, std::size_t size)
{
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
        return nullptr;
    const std::size_t total = count * size;
    void *allocation = AllocateTracked(total);
    if (allocation)
        std::memset(allocation, 0, total);
    return allocation;
}

void *ReallocTracked(void *allocation, std::size_t size)
{
    if (!allocation)
        return AllocateTracked(size);
    if (size == 0)
    {
        FreeTracked(allocation);
        return nullptr;
    }

    auto *old_header = static_cast<AllocationHeader *>(allocation) - 1;
    if (old_header->magic != kAllocationMagic)
        std::abort();
    void *replacement = AllocateTracked(size);
    if (!replacement)
        return nullptr;
    std::memcpy(replacement, allocation,
                old_header->requested_size < size ? old_header->requested_size : size);
    FreeTracked(allocation);
    return replacement;
}

} // namespace

extern "C" int iptv_catalog_arena_begin(std::size_t capacity)
{
    if (!capacity ||
        capacity > std::numeric_limits<std::size_t>::max() - sizeof(CatalogArena) - 0x3fffu)
        return -1;
    const std::size_t mapped_size =
        (sizeof(CatalogArena) + capacity + 0x3fffu) & ~std::size_t(0x3fff);
    void *mapping = mmap(nullptr, mapped_size, kProtectionReadWrite, kMapPrivateAnonymous, -1, 0);
    if (mapping == reinterpret_cast<void *>(-1))
        return -1;

    auto *arena = static_cast<CatalogArena *>(mapping);
    arena->magic = kCatalogArenaMagic;
    arena->mapped_size = mapped_size;
    arena->capacity = mapped_size;
    arena->offset = AlignAllocation(sizeof(CatalogArena));
    arena->live_allocations = 0;
    arena->retired = 0;

    LockCatalogArena();
    if (g_catalog_arena)
    {
        UnlockCatalogArena();
        munmap(arena, mapped_size);
        return -1;
    }
    g_catalog_arena = arena;
    UnlockCatalogArena();
    return 0;
}

extern "C" void iptv_catalog_arena_end()
{
    CatalogArena *arena = nullptr;
    std::size_t mapped_size = 0;
    bool release = false;
    LockCatalogArena();
    arena = g_catalog_arena;
    g_catalog_arena = nullptr;
    if (arena && arena->magic == kCatalogArenaMagic)
    {
        arena->retired = 1;
        release = arena->live_allocations == 0;
        mapped_size = arena->mapped_size;
    }
    UnlockCatalogArena();
    if (release)
        munmap(arena, mapped_size);
}

extern "C" int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    constexpr int running = 2;
    int state = __atomic_load_n(&once_control->state, __ATOMIC_ACQUIRE);
    if (state == PTHREAD_DONE_INIT)
        return 0;

    int expected = PTHREAD_NEEDS_INIT;
    if (__atomic_compare_exchange_n(&once_control->state, &expected, running, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        init_routine();
        __atomic_store_n(&once_control->state, PTHREAD_DONE_INIT, __ATOMIC_RELEASE);
        return 0;
    }

    while (__atomic_load_n(&once_control->state, __ATOMIC_ACQUIRE) != PTHREAD_DONE_INIT)
        sceKernelUsleep(100);
    return 0;
}

namespace
{

class AppSystemInterface final : public Rml::SystemInterface
{
  public:
    double GetElapsedTime() override
    {
        return static_cast<double>(SDL_GetTicks64() - start_ticks_) / 1000.0;
    }

  private:
    Uint64 start_ticks_ = SDL_GetTicks64();
};

class AppFileInterface final : public Rml::FileInterface
{
  public:
    Rml::FileHandle Open(const Rml::String &path) override
    {
        std::FILE *file = std::fopen(path.c_str(), "rb");
        if (!file)
            file = std::fopen(("/app0/" + path).c_str(), "rb");
        return reinterpret_cast<Rml::FileHandle>(file);
    }

    void Close(Rml::FileHandle file) override
    {
        if (file)
            std::fclose(reinterpret_cast<std::FILE *>(file));
    }

    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override
    {
        return std::fread(buffer, 1, size, reinterpret_cast<std::FILE *>(file));
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override
    {
        return fseeko(reinterpret_cast<std::FILE *>(file), offset, origin) == 0;
    }

    size_t Tell(Rml::FileHandle file) override
    {
        return static_cast<size_t>(ftello(reinterpret_cast<std::FILE *>(file)));
    }
};

class SdlRenderInterface final : public Rml::RenderInterfaceCompatibility
{
  public:
    SdlRenderInterface(SDL_Renderer *renderer, SDL_Surface *surface)
        : renderer_(renderer), surface_(surface)
    {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    }

    void RenderGeometry(Rml::Vertex *rml_vertices, int num_vertices, int *indices, int num_indices,
                        Rml::TextureHandle texture, const Rml::Vector2f &translation) override
    {
        AppTexture *app_texture = reinterpret_cast<AppTexture *>(texture);
        if (app_texture && RenderPixelAlignedQuads(rml_vertices, num_vertices, indices, num_indices,
                                                   app_texture, translation))
        {
            return;
        }

        std::vector<SDL_Vertex> vertices;
        vertices.reserve(static_cast<size_t>(num_vertices));
        for (int i = 0; i < num_vertices; ++i)
        {
            const Rml::Vertex &vertex = rml_vertices[i];
            SDL_Vertex sdl_vertex{};
            sdl_vertex.position = {vertex.position.x + translation.x,
                                   vertex.position.y + translation.y};
            sdl_vertex.color = {vertex.colour.red, vertex.colour.green, vertex.colour.blue,
                                vertex.colour.alpha};
            sdl_vertex.tex_coord = {vertex.tex_coord.x, vertex.tex_coord.y};
            vertices.push_back(sdl_vertex);
        }
        SDL_RenderGeometry(renderer_, app_texture ? app_texture->texture : nullptr, vertices.data(),
                           num_vertices, indices, num_indices);
    }

    bool LoadTexture(Rml::TextureHandle &texture_handle, Rml::Vector2i &texture_dimensions,
                     const Rml::String &source) override
    {
        texture_handle = {};
        texture_dimensions = {};

        std::FILE *file = std::fopen(source.c_str(), "rb");
        if (!file && !source.empty() && source[0] != '/')
        {
            const Rml::String app_path = "/app0/" + source;
            file = std::fopen(app_path.c_str(), "rb");
        }
        if (!file)
            return false;

        unsigned char header[18]{};
        const bool header_read = std::fread(header, 1, sizeof(header), file) == sizeof(header);
        int width = 0;
        int height = 0;
        std::vector<unsigned char> pixels;
        bool decoded = false;

        if (header_read && std::memcmp(header, "RTA1", 4) == 0)
        {
            width = ReadLe16(header + 4);
            height = ReadLe16(header + 6);
            const std::uint32_t pixel_count = ReadLe32(header + 8);
            const std::uint32_t payload_length = ReadLe32(header + 12);
            const std::size_t expected_pixels =
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            if (width > 0 && height > 0 && pixel_count == expected_pixels &&
                expected_pixels <= std::numeric_limits<std::size_t>::max() / 4 &&
                payload_length <= expected_pixels * 2 && fseeko(file, 16, SEEK_SET) == 0)
            {
                std::vector<unsigned char> payload(payload_length);
                if (std::fread(payload.data(), 1, payload.size(), file) == payload.size())
                    decoded = DecodeRadioAtlas(payload, expected_pixels, pixels);
            }
        }
        else
        {
            width = header[12] | (header[13] << 8);
            height = header[14] | (header[15] << 8);
            const bool supported = header_read && header[0] == 0 && header[1] == 0 &&
                                   header[2] == 2 && width > 0 && height > 0 && header[16] == 32 &&
                                   (header[17] & 0x0f) == 8 && (header[17] & 0x30) == 0x20;
            if (supported &&
                static_cast<std::size_t>(width) <= std::numeric_limits<std::size_t>::max() /
                                                       (static_cast<std::size_t>(height) * 4))
            {
                pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                              4);
                decoded = std::fread(pixels.data(), 1, pixels.size(), file) == pixels.size();
            }
        }
        std::fclose(file);
        if (!decoded)
            return false;

        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
            pixels.data(), width, height, 32, width * 4, SDL_PIXELFORMAT_BGRA32);
        if (!surface)
            return false;

        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_FreeSurface(surface);
        if (!texture)
            return false;

        if (SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) != 0 ||
            SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest) != 0)
        {
            SDL_DestroyTexture(texture);
            return false;
        }

        auto *app_texture = new AppTexture;
        app_texture->texture = texture;
        app_texture->width = width;
        app_texture->height = height;
        app_texture->exact_pixels = source.find("lvgl-bitmap") != Rml::String::npos;
        app_texture->rgba.resize(pixels.size());
        for (std::size_t i = 0; i < pixels.size(); i += 4)
        {
            app_texture->rgba[i + 0] = pixels[i + 2];
            app_texture->rgba[i + 1] = pixels[i + 1];
            app_texture->rgba[i + 2] = pixels[i + 0];
            app_texture->rgba[i + 3] = pixels[i + 3];
        }
        if (app_texture->exact_pixels)
        {
            app_texture->surface = SDL_CreateRGBSurfaceWithFormatFrom(
                app_texture->rgba.data(), width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
            if (!app_texture->surface ||
                SDL_SetSurfaceBlendMode(app_texture->surface, SDL_BLENDMODE_BLEND) != 0)
            {
                SDL_FreeSurface(app_texture->surface);
                SDL_DestroyTexture(texture);
                delete app_texture;
                return false;
            }
        }

        texture_dimensions = {width, height};
        texture_handle = reinterpret_cast<Rml::TextureHandle>(app_texture);
        return true;
    }

    bool GenerateTexture(Rml::TextureHandle &texture_handle, const Rml::byte *source,
                         const Rml::Vector2i &dimensions) override
    {
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
            const_cast<Rml::byte *>(source), dimensions.x, dimensions.y, 32, dimensions.x * 4,
            SDL_PIXELFORMAT_RGBA32);
        if (!surface)
            return false;

        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_FreeSurface(surface);
        if (texture)
        {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
        }
        if (!texture)
            return false;

        auto *app_texture = new AppTexture;
        app_texture->texture = texture;
        app_texture->width = dimensions.x;
        app_texture->height = dimensions.y;
        app_texture->rgba.assign(source, source + static_cast<std::size_t>(dimensions.x) *
                                                      static_cast<std::size_t>(dimensions.y) * 4);
        texture_handle = reinterpret_cast<Rml::TextureHandle>(app_texture);
        return true;
    }

    void ReleaseTexture(Rml::TextureHandle texture) override
    {
        auto *app_texture = reinterpret_cast<AppTexture *>(texture);
        if (!app_texture)
            return;
        SDL_FreeSurface(app_texture->surface);
        SDL_DestroyTexture(app_texture->texture);
        delete app_texture;
    }

    void EnableScissorRegion(bool enable) override
    {
        scissor_enabled_ = enable;
        SDL_RenderSetClipRect(renderer_, enable ? &scissor_ : nullptr);
    }

    void SetScissorRegion(int x, int y, int width, int height) override
    {
        scissor_ = {x, y, width, height};
        if (scissor_enabled_)
            SDL_RenderSetClipRect(renderer_, &scissor_);
    }

  private:
    struct AppTexture
    {
        SDL_Texture *texture = nullptr;
        SDL_Surface *surface = nullptr;
        int width = 0;
        int height = 0;
        bool exact_pixels = false;
        std::vector<Rml::byte> rgba;
    };

    static std::uint16_t ReadLe16(const unsigned char *value)
    {
        return static_cast<std::uint16_t>(value[0]) | (static_cast<std::uint16_t>(value[1]) << 8);
    }

    static std::uint32_t ReadLe32(const unsigned char *value)
    {
        return static_cast<std::uint32_t>(value[0]) | (static_cast<std::uint32_t>(value[1]) << 8) |
               (static_cast<std::uint32_t>(value[2]) << 16) |
               (static_cast<std::uint32_t>(value[3]) << 24);
    }

    static bool DecodeRadioAtlas(const std::vector<unsigned char> &payload, std::size_t pixel_count,
                                 std::vector<unsigned char> &pixels)
    {
        pixels.assign(pixel_count * 4, 255);
        std::size_t input = 0;
        std::size_t output = 0;
        while (input < payload.size() && output < pixel_count)
        {
            const unsigned char token = payload[input++];
            const std::size_t length = static_cast<std::size_t>(token & 0x7f) + 1;
            if (length > pixel_count - output)
                return false;
            if ((token & 0x80) == 0)
            {
                for (std::size_t index = 0; index < length; ++index)
                    pixels[(output + index) * 4 + 3] = 0;
            }
            else
            {
                const std::size_t bytes = (length + 1) / 2;
                if (bytes > payload.size() - input)
                    return false;
                for (std::size_t index = 0; index < length; ++index)
                {
                    const unsigned char packed = payload[input + index / 2];
                    const unsigned char alpha =
                        (index & 1) != 0 ? static_cast<unsigned char>((packed & 0x0f) * 17)
                                         : static_cast<unsigned char>((packed >> 4) * 17);
                    pixels[(output + index) * 4 + 3] = alpha;
                }
                input += bytes;
            }
            output += length;
        }
        return input == payload.size() && output == pixel_count;
    }

    struct PixelCopy
    {
        SDL_Rect source;
        SDL_Rect destination;
        Rml::ColourbPremultiplied colour;
    };

    static int RoundPixel(float value)
    {
        return static_cast<int>(value + (value >= 0.0f ? 0.5f : -0.5f));
    }

    static bool SameColor(const Rml::Vertex &lhs, const Rml::Vertex &rhs)
    {
        return lhs.colour.red == rhs.colour.red && lhs.colour.green == rhs.colour.green &&
               lhs.colour.blue == rhs.colour.blue && lhs.colour.alpha == rhs.colour.alpha;
    }

    bool RenderPixelAlignedQuads(Rml::Vertex *vertices, int num_vertices, int *indices,
                                 int num_indices, AppTexture *texture,
                                 const Rml::Vector2f &translation)
    {
        if (num_vertices <= 0 || num_indices <= 0 || num_indices % 6 != 0)
            return false;

        const int texture_width = texture->width;
        const int texture_height = texture->height;
        if (texture_width <= 0 || texture_height <= 0)
            return false;

        const int num_quads = num_indices / 6;
        std::vector<PixelCopy> copies;
        copies.reserve(static_cast<size_t>(num_quads));
        for (int quad = 0; quad < num_quads; ++quad)
        {
            const int index = quad * 6;
            const int i0 = indices[index + 0];
            const int i3 = indices[index + 1];
            const int i1 = indices[index + 2];
            const int i2 = indices[index + 5];
            if (i0 < 0 || i0 >= num_vertices || i1 < 0 || i1 >= num_vertices || i2 < 0 ||
                i2 >= num_vertices || i3 < 0 || i3 >= num_vertices || indices[index + 3] != i1 ||
                indices[index + 4] != i3 || i0 == i1 || i0 == i2 || i0 == i3 || i1 == i2 ||
                i1 == i3 || i2 == i3)
                return false;

            const Rml::Vertex &v0 = vertices[i0];
            const Rml::Vertex &v1 = vertices[i1];
            const Rml::Vertex &v2 = vertices[i2];
            const Rml::Vertex &v3 = vertices[i3];
            if (v0.position.y != v1.position.y || v1.position.x != v2.position.x ||
                v2.position.y != v3.position.y || v3.position.x != v0.position.x ||
                v0.tex_coord.y != v1.tex_coord.y || v1.tex_coord.x != v2.tex_coord.x ||
                v2.tex_coord.y != v3.tex_coord.y || v3.tex_coord.x != v0.tex_coord.x ||
                !SameColor(v0, v1) || !SameColor(v0, v2) || !SameColor(v0, v3))
                return false;

            const int source_width = RoundPixel((v1.tex_coord.x - v0.tex_coord.x) * texture_width);
            const int source_height =
                RoundPixel((v3.tex_coord.y - v0.tex_coord.y) * texture_height);
            const int destination_width = RoundPixel(v1.position.x - v0.position.x);
            const int destination_height = RoundPixel(v3.position.y - v0.position.y);
            if (source_width <= 0 || source_height <= 0 || source_width != destination_width ||
                source_height != destination_height)
                return false;

            SDL_Rect source{RoundPixel(v0.tex_coord.x * texture_width),
                            RoundPixel(v0.tex_coord.y * texture_height), source_width,
                            source_height};
            if (source.x < 0 || source.y < 0 || source.x + source.w > texture_width ||
                source.y + source.h > texture_height)
                return false;
            copies.push_back(
                {source,
                 {RoundPixel(v0.position.x + translation.x),
                  RoundPixel(v0.position.y + translation.y), destination_width, destination_height},
                 v0.colour});
        }

        if (texture->exact_pixels)
            return CompositeExactPixels(*texture, copies);

        for (const PixelCopy &copy : copies)
        {
            SDL_SetTextureColorMod(texture->texture, copy.colour.red, copy.colour.green,
                                   copy.colour.blue);
            SDL_SetTextureAlphaMod(texture->texture, copy.colour.alpha);
            SDL_RenderCopy(renderer_, texture->texture, &copy.source, &copy.destination);
        }

        SDL_SetTextureColorMod(texture->texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture->texture, 255);
        return true;
    }

    bool CompositeExactPixels(const AppTexture &texture, const std::vector<PixelCopy> &copies)
    {
        if (!surface_ || !texture.surface)
            return false;
        SDL_RenderFlush(renderer_);

        SDL_Rect old_clip{};
        SDL_GetClipRect(surface_, &old_clip);
        SDL_SetClipRect(surface_, scissor_enabled_ ? &scissor_ : nullptr);
        for (const PixelCopy &copy : copies)
        {
            SDL_SetSurfaceColorMod(texture.surface, copy.colour.red, copy.colour.green,
                                   copy.colour.blue);
            SDL_SetSurfaceAlphaMod(texture.surface, copy.colour.alpha);
            SDL_Rect destination = copy.destination;
            SDL_BlitSurface(texture.surface, &copy.source, surface_, &destination);
        }
        SDL_SetSurfaceColorMod(texture.surface, 255, 255, 255);
        SDL_SetSurfaceAlphaMod(texture.surface, 255);
        SDL_SetClipRect(surface_, &old_clip);
        return true;
    }

    SDL_Renderer *renderer_;
    SDL_Surface *surface_;
    SDL_Rect scissor_{};
    bool scissor_enabled_ = false;
};

bool ConvertInput(const iptv_input_event_t &source, IptvInputEvent *target)
{
    if (!target)
        return false;
    switch (source.action)
    {
    case IPTV_INPUT_CROSS:
        target->key = IptvInputKey::Cross;
        break;
    case IPTV_INPUT_CIRCLE:
        target->key = IptvInputKey::Circle;
        break;
    case IPTV_INPUT_SQUARE:
        target->key = IptvInputKey::Square;
        break;
    case IPTV_INPUT_TRIANGLE:
        target->key = IptvInputKey::Triangle;
        break;
    case IPTV_INPUT_OPTIONS:
        target->key = IptvInputKey::Options;
        break;
    case IPTV_INPUT_L1:
        target->key = IptvInputKey::L1;
        break;
    case IPTV_INPUT_R1:
        target->key = IptvInputKey::R1;
        break;
    case IPTV_INPUT_UP:
        target->key = IptvInputKey::Up;
        break;
    case IPTV_INPUT_DOWN:
        target->key = IptvInputKey::Down;
        break;
    case IPTV_INPUT_LEFT:
        target->key = IptvInputKey::Left;
        break;
    case IPTV_INPUT_RIGHT:
        target->key = IptvInputKey::Right;
        break;
    case IPTV_INPUT_TOUCHPAD:
    case IPTV_INPUT_COUNT:
        return false;
    }
    target->pressed = source.pressed;
    return true;
}

bool LoadFonts()
{
    static constexpr const char *fonts[] = {
        "ui/fonts/lvgl-bitmap/Montserrat-12.fnt", "ui/fonts/lvgl-bitmap/Montserrat-14.fnt",
        "ui/fonts/lvgl-bitmap/Montserrat-16.fnt", "ui/fonts/lvgl-bitmap/Montserrat-18.fnt",
        "ui/fonts/lvgl-bitmap/Montserrat-20.fnt", "ui/fonts/lvgl-bitmap/Montserrat-22.fnt",
        "ui/fonts/lvgl-bitmap/Montserrat-24.fnt", "ui/fonts/lvgl-bitmap/Montserrat-26.fnt",
        "ui/fonts/lvgl-bitmap/Montserrat-28.fnt", "ui/fonts/lvgl-bitmap/Montserrat-32.fnt",
        "ui/fonts/lvgl-bitmap/Montserrat-34.fnt", "ui/fonts/lvgl-bitmap/Montserrat-36.fnt",
        "ui/fonts/lvgl-bitmap/Montserrat-40.fnt", "ui/fonts/lvgl-bitmap/Montserrat-48.fnt",
    };
    for (const char *font : fonts)
    {
        if (!Rml::LoadFontFace(font))
            return false;
    }
    static constexpr const char *multilingual_fonts[] = {
        "ui/fonts/lvgl-bitmap/multilingual/Radio-20.fnt",
        "ui/fonts/lvgl-bitmap/multilingual/Radio-24.fnt",
        "ui/fonts/lvgl-bitmap/multilingual/Radio-28.fnt",
        "ui/fonts/lvgl-bitmap/multilingual/Radio-32.fnt",
    };
    for (const char *font : multilingual_fonts)
    {
        if (!Rml::LoadFontFace(font))
            return false;
    }
    return true;
}

bool RunLauncher(IptvPlayRequest *play_request, const char *failed_channel_id,
                 const char *failed_channel_name, int playback_result, unsigned playback_attempts)
{
    if (!play_request)
        return false;
    *play_request = {};
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        LauncherFailure("SDL initialization");
        return false;
    }
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "software");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    SDL_Window *window = SDL_CreateWindow("PS5 IPTV Client", SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED, 1920, 1080, SDL_WINDOW_SHOWN);
    SDL_Surface *surface = window ? SDL_GetWindowSurface(window) : nullptr;
    SDL_Renderer *renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    if (!window || !renderer)
    {
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        LauncherFailure("software renderer");
        return false;
    }

    AppSystemInterface system_interface;
    AppFileInterface file_interface;
    SdlRenderInterface render_interface(renderer, surface);
    BitmapFontEngine font_engine;
    Rml::RenderInterface *adapted_render_interface = render_interface.GetAdaptedInterface();
    Rml::SetSystemInterface(&system_interface);
    Rml::SetFileInterface(&file_interface);
    Rml::SetRenderInterface(adapted_render_interface);
    Rml::SetFontEngineInterface(&font_engine);
    if (!Rml::Initialise())
    {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        LauncherFailure("RmlUi initialization");
        return false;
    }
    if (!LoadFonts())
    {
        Rml::Shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        LauncherFailure("bitmap fonts");
        return false;
    }

    Rml::Context *context =
        Rml::CreateContext("iptv-shell", {1920, 1080}, adapted_render_interface);
    Rml::ElementDocument *document = context ? context->LoadDocument("ui/main.rml") : nullptr;
    IptvApp app;
    bool input_ready = false;
    bool running = false;
    if (document)
    {
        document->Show();
        input_ready = iptv_input_init();
        running = input_ready && app.Initialize(document);
        if (running && playback_result < 0)
            app.ReportPlaybackFailure(failed_channel_id, failed_channel_name, playback_result,
                                      playback_attempts);
    }
    if (!running)
        LauncherFailure(document ? "input or app initialization" : "RmlUi document");
    if (running)
        sceSystemServiceHideSplashScreen();

    while (running)
    {
        SDL_Event event{};
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
                running = false;
        }
        iptv_input_poll();
        iptv_input_event_t native_input{};
        while (iptv_input_next(&native_input) && running)
        {
            IptvInputEvent input{};
            if (ConvertInput(native_input, &input))
                running = app.HandleInput(input);
        }
        if (running && app.TakePlayRequest(play_request))
            running = false;
        if (running)
            app.Poll();
        context->Update();
        SDL_SetRenderDrawColor(renderer, 7, 16, 22, 255);
        SDL_RenderClear(renderer);
        context->Render();
        SDL_RenderFlush(renderer);
        SDL_UpdateWindowSurface(window);
        sceKernelUsleep(16667);
    }

    app.Shutdown();
    if (input_ready)
        iptv_input_shutdown();
    if (document)
        document->Close();
    if (context)
        Rml::RemoveContext("iptv-shell");
    Rml::Shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return !play_request->urls.empty();
}

bool RunAutotestIfPresent()
{
    std::FILE *file = std::fopen("/app0/iptv-autotest.txt", "rb");
    if (!file)
        return false;

    constexpr char kArchivePath[] = "/download0/iptv-autotest-receipts.txt";
    constexpr char kLatestReceiptPath[] = "/download0/iptv-last-receipt.txt";
    if (std::FILE *archive = std::fopen(kArchivePath, "wb"))
    {
        std::fputs("IPTV_AUTOTEST_RECEIPTS_V1\n", archive);
        std::fclose(archive);
    }

    char line[4097]{};
    unsigned test_index = 0;
    while (std::fgets(line, sizeof(line), file))
    {
        std::size_t bytes = std::strlen(line);
        while (bytes && (line[bytes - 1u] == '\r' || line[bytes - 1u] == '\n' ||
                         line[bytes - 1u] == ' ' || line[bytes - 1u] == '\t'))
        {
            line[--bytes] = '\0';
        }
        char *url = line;
        while (*url == ' ' || *url == '\t')
            ++url;
        if (!*url || *url == '#')
            continue;
        ++test_index;
        char name[64]{};
        std::snprintf(name, sizeof(name), "controlled acceptance %u", test_index);
        char marker[96]{};
        int marker_bytes =
            std::snprintf(marker, sizeof(marker), "IPTV_AUTOTEST_BEGIN index=%u\n", test_index);
        if (marker_bytes > 0)
        {
            WriteStdout(marker, static_cast<std::size_t>(marker_bytes) < sizeof(marker)
                                    ? static_cast<std::size_t>(marker_bytes)
                                    : sizeof(marker) - 1u);
            if (std::FILE *archive = std::fopen(kArchivePath, "ab"))
            {
                std::fwrite(marker, 1, static_cast<std::size_t>(marker_bytes), archive);
                std::fclose(archive);
            }
        }
        std::remove(kLatestReceiptPath);
        const int result = iptv_player_run(url, name);
        if (std::FILE *archive = std::fopen(kArchivePath, "ab"))
        {
            if (std::FILE *receipt = std::fopen(kLatestReceiptPath, "rb"))
            {
                char receipt_chunk[1024]{};
                std::size_t receipt_bytes = 0;
                while ((receipt_bytes =
                            std::fread(receipt_chunk, 1, sizeof(receipt_chunk), receipt)) != 0)
                {
                    std::fwrite(receipt_chunk, 1, receipt_bytes, archive);
                }
                std::fclose(receipt);
            }
            else
            {
                std::fputs("IPTV_RECEIPT_MISSING\n", archive);
            }
            std::fclose(archive);
        }
        marker_bytes = std::snprintf(marker, sizeof(marker),
                                     "IPTV_AUTOTEST_END index=%u result=%d\n", test_index, result);
        if (marker_bytes > 0)
        {
            WriteStdout(marker, static_cast<std::size_t>(marker_bytes) < sizeof(marker)
                                    ? static_cast<std::size_t>(marker_bytes)
                                    : sizeof(marker) - 1u);
            if (std::FILE *archive = std::fopen(kArchivePath, "ab"))
            {
                std::fwrite(marker, 1, static_cast<std::size_t>(marker_bytes), archive);
                std::fclose(archive);
            }
        }
        sceKernelUsleep(250000);
    }
    std::fclose(file);
    return test_index != 0;
}

} // namespace

[[noreturn]] void KeepProcessAlive()
{
    for (;;)
        sceKernelUsleep(1000000);
}

int main()
{
    if (SDL_SetMemoryFunctions(AllocateTracked, CallocTracked, ReallocTracked, FreeTracked) != 0)
    {
        LauncherFailure("SDL allocator setup");
        KeepProcessAlive();
    }

    (void)RunAutotestIfPresent();

    std::string failed_channel_id;
    std::string failed_channel_name;
    int playback_result = 0;
    unsigned playback_attempts = 0;
    for (;;)
    {
        IptvPlayRequest request;
        if (!RunLauncher(&request, failed_channel_id.c_str(), failed_channel_name.c_str(),
                         playback_result, playback_attempts))
            KeepProcessAlive();

        // SDL and AGC must never own VideoOut at the same time.
        sceKernelUsleep(100000);
        playback_result = -1;
        playback_attempts = 0;
        for (const std::string &url : request.urls)
        {
            ++playback_attempts;
            playback_result = iptv_player_run_with_headers(
                url.c_str(), request.channel_name.c_str(),
                request.user_agent.empty() ? nullptr : request.user_agent.c_str(),
                request.referrer.empty() ? nullptr : request.referrer.c_str());
            if (playback_result >= 0)
                break;
        }
        if (playback_result < 0)
        {
            failed_channel_id = request.channel_id;
            failed_channel_name = request.channel_name;
        }
        else
        {
            failed_channel_id.clear();
            failed_channel_name.clear();
            playback_attempts = 0;
        }
        sceKernelUsleep(100000);
    }
}
