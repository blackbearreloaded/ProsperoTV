// ProsperoTV - Process-lifetime heap for the OpenGL runtime.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-opengl native-app/app_heap.c. tools/build.sh links with
// --wrap for the malloc family whenever this file exists, routing allocations
// into a fixed sceLibcMspace that is never unmapped. The menu's renderer, the
// catalog and the player all allocate here; when the space is full an
// allocation falls back to the C library's own heap instead of failing.

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *address, size_t size);
void __real_free(void *address);
int __real_posix_memalign(void **address, size_t alignment, size_t size);
size_t __real_malloc_usable_size(const void *address);

void *sceLibcMspaceCreate(const char *name, void *base, size_t size, unsigned flags);
void *sceLibcMspaceMalloc(void *mspace, size_t size);
void *sceLibcMspaceCalloc(void *mspace, size_t count, size_t size);
void *sceLibcMspaceRealloc(void *mspace, void *address, size_t size);
void sceLibcMspaceFree(void *mspace, void *address);
int sceLibcMspacePosixMemalign(void *mspace, void **address, size_t alignment, size_t size);
size_t sceLibcMspaceMallocUsableSize(const void *address);

/* Catalogs and multilingual MP4 sample indexes share this bounded heap.
 * Keep it for the process lifetime; late C++ destructors still use it. */
#define HUI_HEAP_SIZE (512u * 1024u * 1024u)

static atomic_int hui_heap_state;
static void *hui_heap_base;
static void *hui_heap_mspace;
/* Owned-heap usable bytes only; relaxed counters are observations, not fences.
 * `failures` counts the allocations the space could not hold (they went to
 * the C library's heap). */
static atomic_size_t hui_heap_live_bytes, hui_heap_peak_bytes, hui_heap_blocks;
static atomic_size_t hui_heap_failures;

static void hui_heap_resize_stats(size_t before, size_t after)
{
    size_t live = after >= before ? atomic_fetch_add_explicit(&hui_heap_live_bytes, after - before,
                                                              memory_order_relaxed) +
                                        after - before
                                  : atomic_fetch_sub_explicit(&hui_heap_live_bytes, before - after,
                                                              memory_order_relaxed) -
                                        (before - after);
    size_t peak = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    while (live > peak &&
           !atomic_compare_exchange_weak_explicit(&hui_heap_peak_bytes, &peak, live,
                                                  memory_order_relaxed, memory_order_relaxed))
    {
    }
}

static void hui_heap_record_allocation(void *address)
{
    hui_heap_resize_stats(0, sceLibcMspaceMallocUsableSize(address));
    atomic_fetch_add_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
}

static void hui_heap_record_overflow(void)
{
    atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
}

static int hui_heap_ready(void)
{
    int state = atomic_load_explicit(&hui_heap_state, memory_order_acquire);
    if (state == 2)
        return 1;
    if (state != 0)
        return 0;

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&hui_heap_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire))
        return expected == 2;

    void *base = mmap(NULL, HUI_HEAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED)
    {
        atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
        return 0;
    }

    hui_heap_base = base;
    hui_heap_mspace = sceLibcMspaceCreate("prosperotv", base, HUI_HEAP_SIZE, 0);
    if (hui_heap_mspace == NULL)
    {
        hui_heap_base = NULL;
        munmap(base, HUI_HEAP_SIZE);
        atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
        return 0;
    }

    atomic_store_explicit(&hui_heap_state, 2, memory_order_release);
    return 1;
}

static int hui_heap_owns(const void *address)
{
    /* Acquire publication before reading non-atomic heap metadata. */
    if (atomic_load_explicit(&hui_heap_state, memory_order_acquire) != 2)
        return 0;
    uintptr_t value = (uintptr_t)address;
    uintptr_t base = (uintptr_t)hui_heap_base;
    return value >= base && value - base < HUI_HEAP_SIZE;
}

void *__wrap_malloc(size_t size)
{
    if (hui_heap_ready())
    {
        void *address = sceLibcMspaceMalloc(hui_heap_mspace, size);
        if (address != NULL)
        {
            hui_heap_record_allocation(address);
            return address;
        }
        if (size == 0)
            return NULL;
        hui_heap_record_overflow();
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (hui_heap_ready())
    {
        void *address = sceLibcMspaceCalloc(hui_heap_mspace, count, size);
        if (address != NULL)
        {
            hui_heap_record_allocation(address);
            return address;
        }
        if (count == 0 || size == 0)
            return NULL;
        hui_heap_record_overflow();
    }
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *address, size_t size)
{
    if (address == NULL)
        return __wrap_malloc(size);
    if (!hui_heap_owns(address))
        return __real_realloc(address, size);
    size_t before = sceLibcMspaceMallocUsableSize(address);
    void *result = sceLibcMspaceRealloc(hui_heap_mspace, address, size);
    if (result != NULL)
    {
        hui_heap_resize_stats(before, sceLibcMspaceMallocUsableSize(result));
        return result;
    }
    if (size == 0)
        return NULL;
    /* The space is full: the block moves to the C library's heap. */
    hui_heap_record_overflow();
    result = __real_malloc(size);
    if (result == NULL)
        return NULL;
    memcpy(result, address, before < size ? before : size);
    hui_heap_resize_stats(before, 0);
    atomic_fetch_sub_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
    sceLibcMspaceFree(hui_heap_mspace, address);
    return result;
}

void __wrap_free(void *address)
{
    if (hui_heap_owns(address))
    {
        hui_heap_resize_stats(sceLibcMspaceMallocUsableSize(address), 0);
        atomic_fetch_sub_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
        sceLibcMspaceFree(hui_heap_mspace, address);
    }
    else
    {
        __real_free(address);
    }
}

int __wrap_posix_memalign(void **address, size_t alignment, size_t size)
{
    if (hui_heap_ready())
    {
        int result = sceLibcMspacePosixMemalign(hui_heap_mspace, address, alignment, size);
        if (result == 0)
        {
            if (*address != NULL)
                hui_heap_record_allocation(*address);
            return 0;
        }
        if (result != ENOMEM)
            return result;
        hui_heap_record_overflow();
    }
    return __real_posix_memalign(address, alignment, size);
}

size_t __wrap_malloc_usable_size(const void *address)
{
    return hui_heap_owns(address) ? sceLibcMspaceMallocUsableSize(address)
                                  : __real_malloc_usable_size(address);
}

/* Diagnostics: current heap counters for the log. */
void hui_heap_stats(size_t *live_bytes, size_t *peak_bytes, size_t *blocks, size_t *failures)
{
    if (live_bytes)
        *live_bytes = atomic_load_explicit(&hui_heap_live_bytes, memory_order_relaxed);
    if (peak_bytes)
        *peak_bytes = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    if (blocks)
        *blocks = atomic_load_explicit(&hui_heap_blocks, memory_order_relaxed);
    if (failures)
        *failures = atomic_load_explicit(&hui_heap_failures, memory_order_relaxed);
}
