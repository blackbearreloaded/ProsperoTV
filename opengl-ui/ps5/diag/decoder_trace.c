/* ProsperoTV - What the decoders ask of the system, for the diagnostic log.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Linked with --wrap for these calls (ps5/patch_tree.py). While the diagnostic
 * log is on (Settings, a debug build), every decoder call is written with its
 * result, and every memory call that fails, with what was left at the time.
 * While it is off, each wrapper only passes the call on. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int tv_diag_enabled(void);
void tv_diag_line(const char *line);

static void tv_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void tv_trace(const char *format, ...)
{
    char line[400];
    va_list arguments;
    size_t length;

    if (!tv_diag_enabled())
        return;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    length = strlen(line);
    if (length != 0 && line[length - 1] == '\n')
        line[length - 1] = '\0';
    tv_diag_line(line);
    fprintf(stderr, "%s\n", line);
}

int32_t sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                           size_t alignment, int64_t *start, size_t *size);
int32_t sceKernelAvailableFlexibleMemorySize(size_t *size);
int64_t sceKernelGetDirectMemorySize(void);

int32_t __real_sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                             size_t length, size_t alignment, int type,
                                             int64_t *start);
int32_t __real_sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                                        int64_t start, size_t alignment);
int32_t __real_sceKernelMapNamedFlexibleMemory(void **address, size_t length, int protection,
                                               int flags, const char *name);
int32_t __real_sceVideodec2QueryComputeMemoryInfo(void *memory);
int32_t __real_sceVideodec2AllocateComputeQueue(const void *config, const void *memory,
                                                void **queue);
int32_t __real_sceVideodec2QueryDecoderMemoryInfo(const void *config, void *memory);
int32_t __real_sceVideodec2CreateDecoder(const void *config, const void *memory, void **decoder);

static void left(const char *when)
{
    int64_t start = 0;
    size_t direct = 0;
    size_t flexible = 0;
    const int32_t direct_rc =
        sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x4000, &start,
                                           &direct);
    const int32_t flexible_rc = sceKernelAvailableFlexibleMemorySize(&flexible);
    tv_trace(
            "[TVDIAG] %s: direct total=%lld largest-free=%zu (rc=0x%08x) flexible free=%zu "
            "(rc=0x%08x)\n",
            when, (long long)sceKernelGetDirectMemorySize(), direct, (unsigned)direct_rc, flexible,
            (unsigned)flexible_rc);
}

int32_t __wrap_sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                             size_t length, size_t alignment, int type,
                                             int64_t *start)
{
    const int32_t rc = __real_sceKernelAllocateDirectMemory(search_start, search_end, length,
                                                            alignment, type, start);
    if (rc != 0)
    {
        tv_trace(
                "[TVDIAG] AllocateDirectMemory length=%zu align=0x%zx type=%d end=0x%llx -> "
                "0x%08x\n",
                length, alignment, type, (long long)search_end, (unsigned)rc);
        left("after the failed allocation");
    }
    return rc;
}

int32_t __wrap_sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                                        int64_t start, size_t alignment)
{
    const int32_t rc =
        __real_sceKernelMapDirectMemory(address, length, protection, flags, start, alignment);
    if (rc != 0)
        tv_trace(
                "[TVDIAG] MapDirectMemory length=%zu prot=0x%x flags=0x%x start=0x%llx "
                "align=0x%zx -> 0x%08x\n",
                length, (unsigned)protection, (unsigned)flags, (long long)start, alignment,
                (unsigned)rc);
    return rc;
}

int32_t __wrap_sceKernelMapNamedFlexibleMemory(void **address, size_t length, int protection,
                                               int flags, const char *name)
{
    const int32_t rc =
        __real_sceKernelMapNamedFlexibleMemory(address, length, protection, flags, name);
    if (rc != 0)
    {
        tv_trace( "[TVDIAG] MapNamedFlexibleMemory %s length=%zu -> 0x%08x\n",
                name != NULL ? name : "?", length, (unsigned)rc);
        left("after the failed flexible mapping");
    }
    return rc;
}

int32_t __real_sceAgcInit(void *state, uint32_t size);
int32_t __real_sceAgcCreateShader(void **shader, void *header, void *code);
int32_t __real_sceAgcLinkShaders(void *cx, void *uc, void *reserved, void *vertex_shader,
                                 void *pixel_shader, uint32_t type);
int32_t __real_sceAgcDriverSubmitDcb(void *description);
int32_t __real_sceAgcSuspendPoint(void);
int __real_sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param);
int __real_sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t buffer_index,
                                       const void *buffers, int32_t count, const void *attribute,
                                       int32_t category, void *option);

int32_t __wrap_sceAgcInit(void *state, uint32_t size)
{
    const int32_t rc = __real_sceAgcInit(state, size);
    tv_trace( "[TVDIAG] AgcInit size=%u -> 0x%08x\n", (unsigned)size, (unsigned)rc);
    return rc;
}

int32_t __wrap_sceAgcCreateShader(void **shader, void *header, void *code)
{
    const int32_t rc = __real_sceAgcCreateShader(shader, header, code);
    if (rc != 0)
        tv_trace( "[TVDIAG] AgcCreateShader -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int32_t __wrap_sceAgcLinkShaders(void *cx, void *uc, void *reserved, void *vertex_shader,
                                 void *pixel_shader, uint32_t type)
{
    const int32_t rc =
        __real_sceAgcLinkShaders(cx, uc, reserved, vertex_shader, pixel_shader, type);
    if (rc != 0)
        tv_trace( "[TVDIAG] AgcLinkShaders -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int32_t __wrap_sceAgcDriverSubmitDcb(void *description)
{
    const int32_t rc = __real_sceAgcDriverSubmitDcb(description);
    if (rc != 0)
        tv_trace( "[TVDIAG] AgcDriverSubmitDcb -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int32_t __wrap_sceAgcSuspendPoint(void)
{
    const int32_t rc = __real_sceAgcSuspendPoint();
    if (rc != 0)
        tv_trace( "[TVDIAG] AgcSuspendPoint -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int __wrap_sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param)
{
    const int rc = __real_sceVideoOutOpen(user_id, bus_type, index, param);
    tv_trace( "[TVDIAG] VideoOutOpen -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int __wrap_sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t buffer_index,
                                       const void *buffers, int32_t count, const void *attribute,
                                       int32_t category, void *option)
{
    const int rc = __real_sceVideoOutRegisterBuffers2(handle, set_index, buffer_index, buffers,
                                                      count, attribute, category, option);
    tv_trace( "[TVDIAG] VideoOutRegisterBuffers2 -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int32_t __real_sceAudiodecInitLibrary(uint32_t codec);
int32_t __real_sceAudiodecCreateDecoder(void *control, uint32_t codec);
int __real_sceAudioOutInit(void);
int __real_sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate,
                           unsigned format);

int32_t __wrap_sceAudiodecInitLibrary(uint32_t codec)
{
    const int32_t rc = __real_sceAudiodecInitLibrary(codec);
    tv_trace( "[TVDIAG] AudiodecInitLibrary codec=%u -> 0x%08x\n", (unsigned)codec,
            (unsigned)rc);
    return rc;
}

int32_t __wrap_sceAudiodecCreateDecoder(void *control, uint32_t codec)
{
    const int32_t rc = __real_sceAudiodecCreateDecoder(control, codec);
    tv_trace( "[TVDIAG] AudiodecCreateDecoder codec=%u -> 0x%08x\n", (unsigned)codec,
            (unsigned)rc);
    return rc;
}

int __wrap_sceAudioOutInit(void)
{
    const int rc = __real_sceAudioOutInit();
    tv_trace( "[TVDIAG] AudioOutInit -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int __wrap_sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate,
                           unsigned format)
{
    const int rc = __real_sceAudioOutOpen(user, type, index, grain, rate, format);
    tv_trace( "[TVDIAG] AudioOutOpen user=0x%x type=%d grain=%u rate=%u format=%u -> 0x%08x\n",
            (unsigned)user, type, grain, rate, format, (unsigned)rc);
    return rc;
}

int32_t __real_sceSysmoduleLoadModule(uint32_t id);
int32_t __real_sceSysmoduleUnloadModule(uint32_t id);

int32_t __wrap_sceSysmoduleLoadModule(uint32_t id)
{
    const int32_t rc = __real_sceSysmoduleLoadModule(id);
    tv_trace( "[TVDIAG] SysmoduleLoadModule 0x%04x -> 0x%08x\n", (unsigned)id,
            (unsigned)rc);
    if (rc != 0)
        left("after the failed module load");
    return rc;
}

int32_t __wrap_sceSysmoduleUnloadModule(uint32_t id)
{
    const int32_t rc = __real_sceSysmoduleUnloadModule(id);
    tv_trace( "[TVDIAG] SysmoduleUnloadModule 0x%04x -> 0x%08x\n", (unsigned)id,
            (unsigned)rc);
    return rc;
}

int32_t __wrap_sceVideodec2QueryComputeMemoryInfo(void *memory)
{
    left("decoder open");
    const int32_t rc = __real_sceVideodec2QueryComputeMemoryInfo(memory);
    tv_trace( "[TVDIAG] Videodec2QueryComputeMemoryInfo -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int32_t __wrap_sceVideodec2AllocateComputeQueue(const void *config, const void *memory,
                                                void **queue)
{
    const int32_t rc = __real_sceVideodec2AllocateComputeQueue(config, memory, queue);
    tv_trace( "[TVDIAG] Videodec2AllocateComputeQueue -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int32_t __wrap_sceVideodec2QueryDecoderMemoryInfo(const void *config, void *memory)
{
    const int32_t rc = __real_sceVideodec2QueryDecoderMemoryInfo(config, memory);
    tv_trace( "[TVDIAG] Videodec2QueryDecoderMemoryInfo -> 0x%08x\n", (unsigned)rc);
    return rc;
}

int32_t __wrap_sceVideodec2CreateDecoder(const void *config, const void *memory, void **decoder)
{
    const int32_t rc = __real_sceVideodec2CreateDecoder(config, memory, decoder);
    tv_trace( "[TVDIAG] Videodec2CreateDecoder -> 0x%08x\n", (unsigned)rc);
    if (rc != 0)
        left("after the failed decoder creation");
    return rc;
}
