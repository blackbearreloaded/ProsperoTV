/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* SDR NV12 Videodec2-to-AGC presenter extracted from ProsperoLight. */

#include "iptv_native_agc_present.h"

#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BASE_OUTPUT_WIDTH 1920u
#define BASE_OUTPUT_HEIGHT 1080u
#define FOUR_K_OUTPUT_WIDTH 3840u
#define FOUR_K_OUTPUT_HEIGHT 2160u
#define BASE_FRAMEBUFFER_BYTES 0xa00000u
#define FOUR_K_FRAMEBUFFER_BYTES 0x2000000u
#define TV_SAFE_INSET_X 64u
#define TV_SAFE_INSET_Y 36u
#define FRAMEBUFFER_ALIGNMENT 0x200000u
#define SHADER_MEMORY_BYTES 0x10000u
#define DIRECT_MEMORY_TYPE 12
#define MAP_PROTECTION 0x33
#define VIDEO_OUT_PIXEL_FORMAT_SDR UINT64_C(0x8000000000000000)
#define PRESENT_WAIT_VBLANKS 120u

typedef struct agc_register
{
    uint16_t offset;
    uint16_t pad;
    uint32_t value;
} agc_register_t;

typedef struct agc_command_buffer
{
    uint32_t *bottom;
    uint32_t *top;
    uint32_t *up;
    uint32_t *down;
    uintptr_t callback;
    void *user_data;
    uint32_t reserved_dwords;
    uint32_t pad;
} agc_command_buffer_t;

typedef struct agc_submit_description
{
    void *words;
    uint32_t word_count;
    uint8_t flag;
    uint8_t pad[3];
} agc_submit_description_t;

typedef struct video_buffer
{
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
} video_buffer_t;

typedef struct video_attribute
{
    uint8_t reserved[80];
} video_attribute_t;

int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                      size_t alignment, int memory_type,
                                      int64_t *direct_memory_start);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                                 int64_t direct_memory_start, size_t alignment);
int32_t sceKernelMunmap(void *address, size_t length);
int32_t sceKernelReleaseDirectMemory(int64_t direct_memory_start, size_t length);
int sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param);
int sceVideoOutClose(int32_t handle);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
void sceVideoOutSetBufferAttribute2(video_attribute_t *attribute, uint64_t pixel_format,
                                    uint32_t tiling_mode, uint32_t width, uint32_t height,
                                    uint64_t option, uint32_t dcc_control,
                                    uint64_t dcc_clear_color);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t buffer_index_start,
                                video_buffer_t *buffers, int32_t buffer_count,
                                video_attribute_t *attribute, int32_t category, void *option);
int sceVideoOutUnregisterBuffers(int32_t handle, int32_t set_index);
int sceVideoOutIsFlipPending(int32_t handle);
int sceVideoOutWaitVblank(int32_t handle);
int sceVideoOutGetFlipStatus(int32_t handle, void *status);

int32_t sceAgcInit(void *state, uint32_t size);
int32_t sceAgcCreateShader(void **shader, void *header, void *code);
int32_t sceAgcLinkShaders(void *cx, void *uc, void *reserved, void *vertex_shader,
                          void *pixel_shader, uint32_t primitive_type);
void *sceAgcGetRegisterDefaults(void);
uint32_t *sceAgcDcbSetCxRegistersIndirect(void *command, const void *registers, uint32_t count);
uint32_t *sceAgcDcbSetShRegistersIndirect(void *command, const void *registers, uint32_t count);
uint32_t *sceAgcDcbSetUcRegistersIndirect(void *command, const void *registers, uint32_t count);
uint32_t *sceAgcCbSetShRegisterRangeDirect(void *command, uint32_t offset, const uint32_t *values,
                                           uint32_t count);
uint32_t *sceAgcDcbDrawIndexAuto(void *command, uint32_t count, uint64_t modifier);
uint32_t *sceAgcDcbSetFlip(void *command, uint32_t handle, int buffer_index, uint32_t flip_mode,
                           int64_t flip_argument);
int32_t sceAgcDriverSubmitDcb(void *description);
int32_t sceAgcSuspendPoint(void);
uint32_t sceAgcDriverGetWaitRenderingPacketSizeInDwords(void);
uint32_t sceAgcDriverWaitUntilSafeForRendering(uint32_t **command, uint32_t packet_size,
                                               uint32_t reserved, uint32_t handle,
                                               int buffer_index);

#ifndef IPTV_NATIVE_AGC_ASSET_ROOT
#define IPTV_NATIVE_AGC_ASSET_ROOT "assets/private/"
#endif

#define EMBED_ASSET(symbol, path)                                                                  \
    __asm__(".section .rodata\n"                                                                   \
            ".global " #symbol "_start\n" #symbol "_start:\n"                                      \
            ".incbin \"" IPTV_NATIVE_AGC_ASSET_ROOT path "\"\n"                                    \
            ".global " #symbol "_end\n" #symbol "_end:\n"                                          \
            ".text\n");                                                                            \
    extern const uint8_t symbol##_start[];                                                         \
    extern const uint8_t symbol##_end[]

EMBED_ASSET(iptv_agc_geometry_header, "geometry.header.bin");
EMBED_ASSET(iptv_agc_geometry_code, "geometry.text.bin");
EMBED_ASSET(iptv_agc_pixel_header, "pixel.header.bin");
EMBED_ASSET(iptv_agc_pixel_code, "pixel.text.linear-buffer.bin");
EMBED_ASSET(iptv_agc_resources, "netflix-video-resources.bin");

typedef struct iptv_native_agc_presenter
{
    uint8_t *shader_memory;
    int64_t shader_start;
    void *framebuffer;
    int64_t framebuffer_start;
    void *vertex_shader;
    void *pixel_shader;
    int video;
    uint32_t frame_number;
    uint32_t output_width;
    uint32_t output_height;
    size_t framebuffer_bytes;
    size_t framebuffer_pool_bytes;
    uint8_t ready;
} iptv_native_agc_presenter_t;

static iptv_native_agc_presenter_t presenter = {
    .shader_start = -1,
    .framebuffer_start = -1,
    .video = -1,
};
static uint64_t agc_state;
static uint8_t agc_initialized;
static _Atomic int present_cancelled;

typedef struct output_geometry
{
    uint32_t width;
    uint32_t height;
    size_t framebuffer_bytes;
} output_geometry_t;

static output_geometry_t output_geometry_for(uint32_t source_width, uint32_t source_height)
{
    if (source_width > BASE_OUTPUT_WIDTH || source_height > BASE_OUTPUT_HEIGHT)
        return (output_geometry_t){FOUR_K_OUTPUT_WIDTH, FOUR_K_OUTPUT_HEIGHT,
                                   FOUR_K_FRAMEBUFFER_BYTES};
    return (output_geometry_t){BASE_OUTPUT_WIDTH, BASE_OUTPUT_HEIGHT, BASE_FRAMEBUFFER_BYTES};
}

static uint8_t agc_out_of_space(agc_command_buffer_t *buffer, uint32_t words, void *user_data)
{
    (void)buffer;
    (void)words;
    (void)user_data;
    return 0;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void flush_gpu_data(const void *address, size_t bytes)
{
    const uint8_t *at = address;
    const uint8_t *end = at + bytes;

    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static int shader_resource_offset(void *shader, unsigned kind, uint32_t *offset)
{
    uint8_t *layout = *(uint8_t **)((uint8_t *)shader + 8);
    uint16_t *counts;
    uint16_t *entries;

    if (!layout || kind >= 4)
        return -1;
    counts = (uint16_t *)(layout + 46);
    if (!counts[kind])
        return -1;
    entries = *(uint16_t **)(layout + 8 + kind * sizeof(void *));
    *offset = entries[0] & 0x7fffu;
    return 0;
}

static int copy_asset(void *destination, size_t capacity, const uint8_t *start, const uint8_t *end)
{
    size_t bytes = (size_t)(end - start);

    if (bytes > capacity)
        return -1;
    memcpy(destination, start, bytes);
    return 0;
}

static int prepare_resources(uint8_t *resources)
{
    uint32_t *header = (uint32_t *)resources;
    uint32_t table_offsets[2] = {header[1], header[3]};
    static const uint32_t table_counts[2] = {2, 8};
    uint32_t *limited_offset = (uint32_t *)(resources + 0x500);
    uint32_t *limited_scale = (uint32_t *)(resources + 0x600);
    uint32_t *sample_scale = (uint32_t *)(resources + 0x700);

    if (limited_offset[0] != 0x3d802008u || limited_offset[1] != 0x3d802008u ||
        limited_offset[2] != 0x3d802008u || limited_scale[0] != 0x3f957abdu ||
        limited_scale[1] != 0x3f922492u || limited_scale[2] != 0x3f922492u ||
        sample_scale[0] != 0x42801f88u)
        return -1;

    limited_offset[0] = limited_offset[1] = limited_offset[2] = 0x3d808081u;
    limited_scale[0] = 0x3f950a85u;
    limited_scale[1] = limited_scale[2] = 0x3f91b6dbu;
    sample_scale[0] = 0x3f800000u;

    for (uint32_t table = 0; table < 2; ++table)
    {
        uint32_t *entry = (uint32_t *)(resources + table_offsets[table]);
        for (uint32_t index = 0; index < table_counts[table]; ++index, entry += 4)
        {
            uintptr_t address = (uintptr_t)resources + entry[0];
            entry[0] = (uint32_t)address;
            entry[1] = (entry[1] & 0xffff0000u) | (uint32_t)(address >> 32);
        }
    }
    return 0;
}

static void bind_pixel_source(agc_command_buffer_t *command, uint8_t *resources, const void *source,
                              size_t y_bytes, size_t uv_bytes, const void *pixel_cb)
{
    uint32_t descriptor[30] = {0};
    uintptr_t uv = (uintptr_t)source + y_bytes;
    uint32_t *header = (uint32_t *)resources;
    uintptr_t table = (uintptr_t)resources + header[3];

    descriptor[0] = (uint32_t)(uintptr_t)source;
    descriptor[1] = (uint32_t)((uintptr_t)source >> 32);
    descriptor[2] = (uint32_t)y_bytes;
    descriptor[3] = 0x31016facu;
    descriptor[5] = 0x00700000u;
    descriptor[8] = (uint32_t)uv;
    descriptor[9] = (uint32_t)(uv >> 32);
    descriptor[10] = (uint32_t)uv_bytes;
    descriptor[11] = 0x31016facu;
    descriptor[13] = 0x00700000u;
    descriptor[16] = descriptor[20] = 0x00000092u;
    descriptor[17] = descriptor[21] = 0x00fff000u;
    descriptor[18] = descriptor[22] = 0x09500000u;
    descriptor[24] = (uint32_t)(uintptr_t)pixel_cb;
    descriptor[25] = (uint32_t)((uintptr_t)pixel_cb >> 32) | (16u << 16);
    descriptor[26] = 4;
    descriptor[27] = 0x0004dfacu;
    descriptor[28] = (uint32_t)table;
    descriptor[29] = (uint32_t)(table >> 32);
    sceAgcCbSetShRegisterRangeDirect(command, 0x0c, descriptor, 30);
}

static int render_frame(int video, int buffer_index, void *target, uint8_t *memory,
                        void *vertex_shader, void *pixel_shader, const void *source,
                        size_t source_bytes, uint32_t pitch, uint32_t surface_height,
                        uint32_t visible_width, uint32_t visible_height, uint32_t output_width,
                        uint32_t output_height, int64_t render_marker)
{
    static const uint16_t target_offsets[16] = {0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f,
                                                0x321, 0x323, 0x324, 0x325, 0x390, 0x398,
                                                0x3a0, 0x3a8, 0x3b0, 0x3b8};
    static const uint32_t geometry_constants[16] = {
        0x3fa24ce6, 0, 0,          0x3e2f0fdd, 0, 0x3fa21449, 0, 0x3e111049,
        0,          0, 0xbf800000, 0x80000000, 0, 0,          0, 0x3f800000};
    static const uint32_t pixel_constants[16] = {
        0x3f800000, 0x3f800000, 0x3f800000, 0, 0x3fed844d, 0xbe3fd0d0, 0, 0,
        0,          0xbeefad6d, 0x3fc9930c, 0, 0,          0,          0, 0};
    agc_register_t *cx = (agc_register_t *)(memory + 0x7000);
    uint8_t *geometry_cb = memory + 0x7800;
    uint8_t *pixel_cb = memory + 0x7900;
    uint8_t *resources = memory + 0xc000;
    uint32_t *words = (uint32_t *)(memory + 0x8000);
    agc_command_buffer_t command = {0};
    agc_submit_description_t submit = {0};
    uint32_t descriptor[30] = {0};
    void *defaults = sceAgcGetRegisterDefaults();
    agc_register_t **blocks;
    uint32_t default_count;
    uint32_t cx_count = 16;
    uint32_t slot;
    size_t y_bytes = (size_t)pitch * surface_height;
    size_t uv_bytes = (size_t)pitch * ((surface_height + 1u) / 2u);
    uint32_t inset_x = output_width * TV_SAFE_INSET_X / BASE_OUTPUT_WIDTH;
    uint32_t inset_y = output_height * TV_SAFE_INSET_Y / BASE_OUTPUT_HEIGHT;

    if (!defaults || visible_width == 0 || visible_height == 0 || visible_width > pitch ||
        visible_height > surface_height || (pitch & 1u) != 0 || y_bytes > UINT32_MAX ||
        uv_bytes > UINT32_MAX || y_bytes + uv_bytes > source_bytes || output_width == 0 ||
        output_height == 0)
        return -1;

    blocks = *(agc_register_t ***)defaults;
    default_count = *(uint32_t *)((uint8_t *)defaults + 0x20);
    for (uint32_t index = 0; index < 16; ++index)
    {
        cx[index] = (agc_register_t){target_offsets[index], 0, 0};
        for (uint32_t candidate = 0; blocks && blocks[0] && candidate < default_count; ++candidate)
        {
            if (blocks[0][candidate].offset == target_offsets[index])
            {
                cx[index].value = blocks[0][candidate].value;
                break;
            }
        }
    }

    cx[0].value = (uint32_t)((uintptr_t)target >> 8);
    cx[1].value &= 0xfc001fffu;
    cx[2].value = (cx[2].value & ~(0x7cu | 0x700u | 0x1800u | 0x10000000u | 0x10000u | 0x8000u |
                                   0x40000u | 0x4000u)) |
                  0x28u | 0x8000u;
    cx[3].value &= ~(0x7000u | 0x18000u);
    cx[4].value = (cx[4].value & ~(0x60u | 0x0cu | 0x00100200u | 0x80000u)) | 0x48u;
    cx[5].value = cx[6].value = cx[9].value = 0;
    cx[10].value = (cx[10].value & 0xffffff00u) | (uint32_t)((uintptr_t)target >> 40);
    cx[11].value &= 0xffffff00u;
    cx[12].value &= 0xffffff00u;
    cx[13].value &= 0xffffff00u;
    cx[14].value = (output_height - 1u) | ((output_width - 1u) << 14);
    cx[15].value = (cx[15].value & ~(0x1fffu | 0x7c000u | 0x03000000u | 0x44000000u)) | 0x6c000u |
                   0x01000000u | 0x44000000u;

#define ADD_REG(register_offset, register_value)                                                   \
    do                                                                                             \
    {                                                                                              \
        cx[cx_count++] = (agc_register_t){(register_offset), 0, (register_value)};                 \
    } while (0)
    ADD_REG(0x10f, float_bits((output_width - inset_x * 2u) * .5f));
    ADD_REG(0x110, float_bits(output_width * .5f));
    ADD_REG(0x111, float_bits((output_height - inset_y * 2u) * -.5f));
    ADD_REG(0x112, float_bits(output_height * .5f));
    ADD_REG(0x113, float_bits(1));
    ADD_REG(0x114, 0);
    ADD_REG(0x0b4, 0);
    ADD_REG(0x0b5, float_bits(1));
    ADD_REG(0x2fa, float_bits(1));
    ADD_REG(0x2fb, float_bits(1));
    ADD_REG(0x2fc, float_bits(1));
    ADD_REG(0x2fd, float_bits(1));
    ADD_REG(0x090, 0x80000000u);
    ADD_REG(0x091, output_width | (output_height << 16));
    ADD_REG(0x08e, 0x0f);
#undef ADD_REG

    memcpy(geometry_cb, geometry_constants, sizeof(geometry_constants));
    memcpy(pixel_cb, pixel_constants, sizeof(pixel_constants));
    ((uint32_t *)pixel_cb)[12] = float_bits((float)visible_width);
    ((uint32_t *)pixel_cb)[13] = float_bits((float)visible_height);
    ((uint32_t *)pixel_cb)[14] = pitch;
    ((uint32_t *)pixel_cb)[15] = pitch / 2u;

    command.bottom = words;
    command.top = words + 0x4000u / sizeof(*words);
    command.up = words;
    command.down = command.top;
    command.callback = (uintptr_t)agc_out_of_space;
    sceAgcDriverWaitUntilSafeForRendering(&command.up,
                                          sceAgcDriverGetWaitRenderingPacketSizeInDwords(), 0,
                                          (uint32_t)video, buffer_index);

    {
        agc_register_t *link_cx = (agc_register_t *)(memory + 0x5000);
        agc_register_t *combined_sh = (agc_register_t *)(memory + 0x6800);
        agc_register_t *vs_cx = *(agc_register_t **)((uint8_t *)vertex_shader + 24);
        agc_register_t *ps_cx = *(agc_register_t **)((uint8_t *)pixel_shader + 24);
        agc_register_t *vs_sh = *(agc_register_t **)((uint8_t *)vertex_shader + 32);
        agc_register_t *ps_sh = *(agc_register_t **)((uint8_t *)pixel_shader + 32);
        uint32_t vs_cx_count = *((uint8_t *)vertex_shader + 91);
        uint32_t ps_cx_count = *((uint8_t *)pixel_shader + 91);
        uint32_t vs_sh_count = *((uint8_t *)vertex_shader + 92);
        uint32_t ps_sh_count = *((uint8_t *)pixel_shader + 92);

        memcpy(cx + cx_count, link_cx, 34 * sizeof(*cx));
        cx_count += 34;
        memcpy(cx + cx_count, vs_cx, vs_cx_count * sizeof(*cx));
        cx_count += vs_cx_count;
        memcpy(cx + cx_count, ps_cx, ps_cx_count * sizeof(*cx));
        cx_count += ps_cx_count;
        memcpy(combined_sh, vs_sh, vs_sh_count * sizeof(*combined_sh));
        memcpy(combined_sh + vs_sh_count, ps_sh, ps_sh_count * sizeof(*combined_sh));
        sceAgcDcbSetCxRegistersIndirect(&command, cx, cx_count);
        sceAgcDcbSetUcRegistersIndirect(&command, memory + 0x6000, 3);
        sceAgcDcbSetShRegistersIndirect(&command, combined_sh, vs_sh_count + ps_sh_count);
    }

    if (shader_resource_offset(vertex_shader, 3, &slot) != 0)
        return -2;
    descriptor[0] = (uint32_t)(uintptr_t)geometry_cb;
    descriptor[1] = (uint32_t)((uintptr_t)geometry_cb >> 32) | (16u << 16);
    descriptor[2] = 4;
    descriptor[3] = 0x0004dfacu;
    {
        uint32_t *header = (uint32_t *)resources;
        uintptr_t table = (uintptr_t)resources + header[1];
        uintptr_t vertex = (uintptr_t)resources + header[2];
        descriptor[4] = (uint32_t)table;
        descriptor[5] = (uint32_t)(table >> 32);
        descriptor[6] = (uint32_t)vertex;
        descriptor[7] = (uint32_t)(vertex >> 32);
    }
    sceAgcCbSetShRegisterRangeDirect(&command, 0x8c + slot, descriptor, 8);
    bind_pixel_source(&command, resources, source, y_bytes, uv_bytes, pixel_cb);
    sceAgcDcbDrawIndexAuto(&command, 4, 2);
    sceAgcDcbSetFlip(&command, (uint32_t)video, buffer_index, 1, render_marker);

    submit.words = words;
    submit.word_count = (uint32_t)(command.up - words);
    flush_gpu_data(memory, SHADER_MEMORY_BYTES);
    {
        int32_t result = sceAgcDriverSubmitDcb(&submit);
        if (result == 0)
            result = sceAgcSuspendPoint();
        return result;
    }
}

static int32_t teardown_presenter(int drain)
{
    int32_t first_result = drain ? iptv_native_agc_present_drain() : 0;
    int32_t result;

    if (presenter.video >= 0 && presenter.ready)
    {
        result = sceVideoOutUnregisterBuffers(presenter.video, 0);
        if (first_result == 0 && result != 0 && (uint32_t)result != UINT32_C(0x80290009))
            first_result = result;
    }
    if (presenter.video >= 0)
    {
        result = sceVideoOutClose(presenter.video);
        if (first_result == 0 && result != 0)
            first_result = result;
    }
    if (presenter.framebuffer)
    {
        result = sceKernelMunmap(presenter.framebuffer, presenter.framebuffer_pool_bytes);
        if (first_result == 0 && result != 0)
            first_result = result;
    }
    if (presenter.framebuffer_start >= 0)
    {
        result = sceKernelReleaseDirectMemory(presenter.framebuffer_start,
                                              presenter.framebuffer_pool_bytes);
        if (first_result == 0 && result != 0)
            first_result = result;
    }
    if (presenter.shader_memory)
    {
        result = sceKernelMunmap(presenter.shader_memory, SHADER_MEMORY_BYTES);
        if (first_result == 0 && result != 0)
            first_result = result;
    }
    if (presenter.shader_start >= 0)
    {
        result = sceKernelReleaseDirectMemory(presenter.shader_start, SHADER_MEMORY_BYTES);
        if (first_result == 0 && result != 0)
            first_result = result;
    }

    memset(&presenter, 0, sizeof(presenter));
    presenter.shader_start = -1;
    presenter.framebuffer_start = -1;
    presenter.video = -1;
    return first_result;
}

static int32_t initialize_presenter(const void *source, size_t source_bytes, uint32_t visible_width,
                                    uint32_t visible_height)
{
    const output_geometry_t output = output_geometry_for(visible_width, visible_height);
    int64_t direct_limit = sceKernelGetDirectMemorySize();
    int32_t result = 0;
    int32_t link_result = -1;

    if (!source || source_bytes == 0 || direct_limit <= 0)
        return -1;
    presenter.output_width = output.width;
    presenter.output_height = output.height;
    presenter.framebuffer_bytes = output.framebuffer_bytes;
    presenter.framebuffer_pool_bytes = output.framebuffer_bytes * 2u;
    if (!agc_initialized)
    {
        result = sceAgcInit(&agc_state, 8);
        if (result == 0)
            agc_initialized = 1;
    }
    if (result != 0)
    {
        agc_state = 0;
        return result;
    }

    result = sceKernelAllocateDirectMemory(0, direct_limit, SHADER_MEMORY_BYTES, 0x4000,
                                           DIRECT_MEMORY_TYPE, &presenter.shader_start);
    if (result == 0)
        result = sceKernelMapDirectMemory((void **)&presenter.shader_memory, SHADER_MEMORY_BYTES,
                                          MAP_PROTECTION, 0, presenter.shader_start, 0x4000);
    if (result != 0 || !presenter.shader_memory)
    {
        if (result == 0)
            result = -2;
        goto fail;
    }

    memset(presenter.shader_memory, 0, SHADER_MEMORY_BYTES);
    if (copy_asset(presenter.shader_memory, 0x1000, iptv_agc_geometry_header_start,
                   iptv_agc_geometry_header_end) != 0 ||
        copy_asset(presenter.shader_memory + 0x3700, 0x1000, iptv_agc_geometry_code_start,
                   iptv_agc_geometry_code_end) != 0 ||
        copy_asset(presenter.shader_memory + 0x1000, 0x1000, iptv_agc_pixel_header_start,
                   iptv_agc_pixel_header_end) != 0 ||
        copy_asset(presenter.shader_memory + 0x2000, 0x1000, iptv_agc_pixel_code_start,
                   iptv_agc_pixel_code_end) != 0 ||
        copy_asset(presenter.shader_memory + 0xc000, 0x1000, iptv_agc_resources_start,
                   iptv_agc_resources_end) != 0 ||
        prepare_resources(presenter.shader_memory + 0xc000) != 0)
    {
        result = -3;
        goto fail;
    }

    result = sceAgcCreateShader(&presenter.vertex_shader, presenter.shader_memory,
                                presenter.shader_memory + 0x3700);
    if (result == 0)
        result = sceAgcCreateShader(&presenter.pixel_shader, presenter.shader_memory + 0x1000,
                                    presenter.shader_memory + 0x2000);
    if (result == 0)
        link_result =
            sceAgcLinkShaders(presenter.shader_memory + 0x5000, presenter.shader_memory + 0x6000,
                              NULL, presenter.vertex_shader, presenter.pixel_shader, 6);
    if (result != 0 || link_result != 0)
    {
        if (result == 0)
            result = link_result;
        goto fail;
    }

    presenter.video = sceVideoOutOpen(0xff, 0, 0, NULL);
    result = presenter.video >= 0 ? sceVideoOutSetFlipRate(presenter.video, 0) : presenter.video;
    if (result == 0)
        result = sceKernelAllocateDirectMemory(0, direct_limit, presenter.framebuffer_pool_bytes,
                                               FRAMEBUFFER_ALIGNMENT, DIRECT_MEMORY_TYPE,
                                               &presenter.framebuffer_start);
    if (result == 0)
        result = sceKernelMapDirectMemory(&presenter.framebuffer, presenter.framebuffer_pool_bytes,
                                          MAP_PROTECTION, 0, presenter.framebuffer_start,
                                          FRAMEBUFFER_ALIGNMENT);
    if (result != 0 || !presenter.framebuffer)
    {
        if (result == 0)
            result = -4;
        goto fail;
    }

    memset(presenter.framebuffer, 0, presenter.framebuffer_pool_bytes);
    flush_gpu_data(presenter.framebuffer, presenter.framebuffer_pool_bytes);
    {
        video_buffer_t buffers[2] = {
            {presenter.framebuffer, NULL, NULL, NULL},
            {(uint8_t *)presenter.framebuffer + presenter.framebuffer_bytes, NULL, NULL, NULL}};
        video_attribute_t attribute = {0};

        sceVideoOutSetBufferAttribute2(&attribute, VIDEO_OUT_PIXEL_FORMAT_SDR, 0,
                                       presenter.output_width, presenter.output_height, 0, 0, 0);
        result =
            sceVideoOutRegisterBuffers2(presenter.video, 0, 0, buffers, 2, &attribute, 0, NULL);
    }
    if (result != 0)
        goto fail;

    presenter.ready = 1;
    return 0;

fail:
    (void)teardown_presenter(0);
    return result;
}

void iptv_native_agc_present_set_cancelled(int cancelled)
{
    atomic_store_explicit(&present_cancelled, cancelled != 0, memory_order_relaxed);
}

int32_t iptv_native_agc_present_nv12(const void *source, size_t source_bytes, uint32_t pitch,
                                     uint32_t surface_height, uint32_t visible_width,
                                     uint32_t visible_height)
{
    uint64_t status[16] = {0};
    uint32_t frame_number = presenter.frame_number;
    uint32_t buffer_index = frame_number & 1u;
    int64_t render_marker = INT64_C(0x49505456) + ((int64_t)frame_number << 8);
    void *target;
    unsigned waits;
    int32_t result;

    if (atomic_load_explicit(&present_cancelled, memory_order_relaxed))
        return -125;
    {
        const output_geometry_t output = output_geometry_for(visible_width, visible_height);
        if (presenter.ready &&
            (presenter.output_width != output.width || presenter.output_height != output.height))
        {
            result = teardown_presenter(1);
            if (result != 0)
                return result;
        }
    }
    if (!presenter.ready)
    {
        result = initialize_presenter(source, source_bytes, visible_width, visible_height);
        if (result != 0)
            return result;
    }

    target = (uint8_t *)presenter.framebuffer + buffer_index * presenter.framebuffer_bytes;
    result = render_frame(presenter.video, (int)buffer_index, target, presenter.shader_memory,
                          presenter.vertex_shader, presenter.pixel_shader, source, source_bytes,
                          pitch, surface_height, visible_width, visible_height,
                          presenter.output_width, presenter.output_height, render_marker);
    if (result != 0)
        return result;

    for (waits = 0; waits < PRESENT_WAIT_VBLANKS; ++waits)
    {
        if (atomic_load_explicit(&present_cancelled, memory_order_relaxed))
            return -125;
        if (sceVideoOutGetFlipStatus(presenter.video, status) == 0 &&
            status[3] == (uint64_t)render_marker)
            break;
        sceVideoOutWaitVblank(presenter.video);
    }
    if (waits == PRESENT_WAIT_VBLANKS)
        return -5;

    ++presenter.frame_number;
    return 0;
}

int32_t iptv_native_agc_present_drain(void)
{
    unsigned waits = 0;
    int32_t result = 0;

    if (presenter.video < 0)
        return 0;
    while (waits < PRESENT_WAIT_VBLANKS && (result = sceVideoOutIsFlipPending(presenter.video)) > 0)
    {
        sceVideoOutWaitVblank(presenter.video);
        ++waits;
    }
    return result;
}

int32_t iptv_native_agc_present_shutdown(void)
{
    int32_t result = teardown_presenter(1);

    atomic_store_explicit(&present_cancelled, 0, memory_order_relaxed);
    return result;
}
