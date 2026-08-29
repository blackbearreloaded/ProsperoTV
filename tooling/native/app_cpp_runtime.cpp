/*
 * ps5-native-app-boilerplate - Minimal target C++ allocation runtime.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bridges standard C++ allocation operators to the clean-room libc module
 * without introducing exceptions, RTTI, or the complete libc++ runtime.
 */

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

extern "C"
{
    void *mmap(void *address, std::size_t length, int protection, int flags, int descriptor,
               long offset);
    int munmap(void *address, std::size_t length);
}

namespace
{
// The clean-room libc.prx intentionally supplies the loader-facing heap
// contract, not a general-purpose heap. Keep the module byte-identical and
// give the executable one coherent allocator family backed by anonymous pages.
constexpr std::size_t kPageSize = 0x4000;
constexpr std::size_t kPoolChunkSize = 32 * 1024 * 1024;
constexpr std::size_t kDirectMapThreshold = 4 * 1024 * 1024;
constexpr std::size_t kDefaultAlignment = alignof(std::max_align_t);
constexpr std::uint64_t kAllocatedMagic = UINT64_C(0x49505456414C4C43);
constexpr std::uint64_t kFreeMagic = UINT64_C(0x4950545646524545);
constexpr int kProtectionReadWrite = 3;
constexpr int kMapPrivateAnonymous = 0x1002;

struct alignas(std::max_align_t) AllocationHeader
{
    std::uint64_t magic;
    std::size_t capacity;
    std::size_t requested;
    void *mapping;
    std::size_t mapped_size;
    AllocationHeader *next_free;
};

unsigned char *g_pool_cursor = nullptr;
unsigned char *g_pool_limit = nullptr;
AllocationHeader *g_free_list = nullptr;
int g_allocator_lock = 0;

void lock_allocator() noexcept
{
    while (__atomic_test_and_set(&g_allocator_lock, __ATOMIC_ACQUIRE))
    {
    }
}

void unlock_allocator() noexcept
{
    __atomic_clear(&g_allocator_lock, __ATOMIC_RELEASE);
}

[[nodiscard]] bool valid_alignment(std::size_t alignment) noexcept
{
    return alignment >= sizeof(void *) && (alignment & (alignment - 1)) == 0;
}

[[nodiscard]] std::uintptr_t align_up(std::uintptr_t value, std::size_t alignment) noexcept
{
    return (value + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
}

[[nodiscard]] void *map_allocation(std::size_t size, std::size_t alignment) noexcept
{
    constexpr std::size_t header_size = sizeof(AllocationHeader);
    constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (alignment - 1 > maximum - header_size || size > maximum - header_size - (alignment - 1))
        return nullptr;
    const std::size_t total = header_size + size + alignment - 1;
    if (total > std::numeric_limits<std::size_t>::max() - (kPageSize - 1))
        return nullptr;
    const std::size_t mapped_size = (total + kPageSize - 1) & ~(kPageSize - 1);
    void *mapping = mmap(nullptr, mapped_size, kProtectionReadWrite, kMapPrivateAnonymous, -1, 0);
    if (mapping == reinterpret_cast<void *>(-1))
        return nullptr;

    const std::uintptr_t payload_value =
        align_up(reinterpret_cast<std::uintptr_t>(mapping) + header_size, alignment);
    auto *header = reinterpret_cast<AllocationHeader *>(payload_value - header_size);
    header->magic = kAllocatedMagic;
    header->capacity = size;
    header->requested = size;
    header->mapping = mapping;
    header->mapped_size = mapped_size;
    header->next_free = nullptr;
    return reinterpret_cast<void *>(payload_value);
}

[[nodiscard]] void *allocate_storage(std::size_t size, std::size_t alignment) noexcept
{
    if (size == 0)
        size = 1;
    if (!valid_alignment(alignment))
        return nullptr;
    if (size >= kDirectMapThreshold)
        return map_allocation(size, alignment);

    constexpr std::size_t header_size = sizeof(AllocationHeader);
    if (size > std::numeric_limits<std::size_t>::max() - (kDefaultAlignment - 1))
        return nullptr;
    const std::size_t capacity = static_cast<std::size_t>(align_up(size, kDefaultAlignment));
    constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (alignment - 1 > maximum - header_size || capacity > maximum - header_size - (alignment - 1))
        return nullptr;
    const std::size_t required = header_size + capacity + alignment - 1;
    if (required > kPoolChunkSize)
        return map_allocation(size, alignment);

    lock_allocator();
    AllocationHeader **link = &g_free_list;
    while (*link)
    {
        AllocationHeader *header = *link;
        void *payload = header + 1;
        if (header->magic == kFreeMagic && header->capacity >= size &&
            reinterpret_cast<std::uintptr_t>(payload) % alignment == 0)
        {
            *link = header->next_free;
            header->magic = kAllocatedMagic;
            header->requested = size;
            header->next_free = nullptr;
            unlock_allocator();
            return payload;
        }
        link = &header->next_free;
    }

    std::uintptr_t payload_value = 0;
    if (g_pool_cursor)
        payload_value =
            align_up(reinterpret_cast<std::uintptr_t>(g_pool_cursor) + header_size, alignment);
    if (!g_pool_cursor || payload_value + capacity > reinterpret_cast<std::uintptr_t>(g_pool_limit))
    {
        void *mapping =
            mmap(nullptr, kPoolChunkSize, kProtectionReadWrite, kMapPrivateAnonymous, -1, 0);
        if (mapping == reinterpret_cast<void *>(-1))
        {
            unlock_allocator();
            return nullptr;
        }
        g_pool_cursor = static_cast<unsigned char *>(mapping);
        g_pool_limit = g_pool_cursor + kPoolChunkSize;
        payload_value =
            align_up(reinterpret_cast<std::uintptr_t>(g_pool_cursor) + header_size, alignment);
    }
    if (payload_value + capacity > reinterpret_cast<std::uintptr_t>(g_pool_limit))
    {
        unlock_allocator();
        return nullptr;
    }

    auto *header = reinterpret_cast<AllocationHeader *>(payload_value - header_size);
    header->magic = kAllocatedMagic;
    header->capacity = capacity;
    header->requested = size;
    header->mapping = nullptr;
    header->mapped_size = 0;
    header->next_free = nullptr;
    g_pool_cursor =
        reinterpret_cast<unsigned char *>(align_up(payload_value + capacity, kDefaultAlignment));
    unlock_allocator();
    return reinterpret_cast<void *>(payload_value);
}

void deallocate_storage(void *address) noexcept
{
    if (!address)
        return;
    auto *header = static_cast<AllocationHeader *>(address) - 1;
    if (header->magic != kAllocatedMagic)
        return;
    if (header->mapped_size)
    {
        header->magic = kFreeMagic;
        (void)munmap(header->mapping, header->mapped_size);
        return;
    }

    lock_allocator();
    header->magic = kFreeMagic;
    header->next_free = g_free_list;
    g_free_list = header;
    unlock_allocator();
}

[[nodiscard]] void *allocate(std::size_t size) noexcept
{
    return allocate_storage(size, kDefaultAlignment);
}

[[nodiscard]] void *allocate_aligned(std::size_t size, std::size_t alignment) noexcept
{
    return allocate_storage(size, alignment < kDefaultAlignment ? kDefaultAlignment : alignment);
}

[[noreturn]] void allocation_failure() noexcept
{
    __builtin_trap();
}
} // namespace

extern "C" void *malloc(std::size_t size)
{
    return allocate_storage(size, kDefaultAlignment);
}

extern "C" void free(void *address)
{
    deallocate_storage(address);
}

extern "C" void *calloc(std::size_t count, std::size_t size)
{
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
        return nullptr;
    const std::size_t total = count * size;
    auto *address = static_cast<unsigned char *>(allocate_storage(total, kDefaultAlignment));
    if (!address)
        return nullptr;
    for (std::size_t index = 0; index < total; ++index)
        address[index] = 0;
    return address;
}

extern "C" void *realloc(void *address, std::size_t size)
{
    if (!address)
        return allocate_storage(size, kDefaultAlignment);
    if (size == 0)
    {
        deallocate_storage(address);
        return nullptr;
    }

    auto *header = static_cast<AllocationHeader *>(address) - 1;
    if (header->magic != kAllocatedMagic)
        return nullptr;
    if (size <= header->capacity)
    {
        header->requested = size;
        return address;
    }

    void *replacement = allocate_storage(size, kDefaultAlignment);
    if (!replacement)
        return nullptr;
    const std::size_t copy_size = header->requested < size ? header->requested : size;
    auto *destination = static_cast<unsigned char *>(replacement);
    auto *source = static_cast<const unsigned char *>(address);
    for (std::size_t index = 0; index < copy_size; ++index)
        destination[index] = source[index];
    deallocate_storage(address);
    return replacement;
}

extern "C" int posix_memalign(void **address, std::size_t alignment, std::size_t size)
{
    if (!address || !valid_alignment(alignment))
        return 22;
    void *allocation = allocate_storage(size, alignment);
    if (!allocation)
        return 12;
    *address = allocation;
    return 0;
}

extern "C" __attribute__((noinline, visibility("hidden"))) bool
ps5ObserveOwnedAllocation(const void *address) noexcept
{
    __asm__ volatile("" : : "r"(address) : "memory");
    return address != nullptr;
}

void *operator new(std::size_t size)
{
    if (void *address = allocate(size))
        return address;
    allocation_failure();
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate(size);
}

void *operator new(std::size_t size, std::align_val_t alignment)
{
    if (void *address = allocate_aligned(size, static_cast<std::size_t>(alignment)))
        return address;
    allocation_failure();
}

void *operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *address) noexcept
{
    deallocate_storage(address);
}

void operator delete[](void *address) noexcept
{
    deallocate_storage(address);
}

void operator delete(void *address, std::size_t) noexcept
{
    deallocate_storage(address);
}

void operator delete[](void *address, std::size_t) noexcept
{
    deallocate_storage(address);
}

void operator delete(void *address, std::align_val_t) noexcept
{
    deallocate_storage(address);
}

void operator delete[](void *address, std::align_val_t) noexcept
{
    deallocate_storage(address);
}

void operator delete(void *address, std::size_t, std::align_val_t) noexcept
{
    deallocate_storage(address);
}

void operator delete[](void *address, std::size_t, std::align_val_t) noexcept
{
    deallocate_storage(address);
}

void operator delete(void *address, const std::nothrow_t &) noexcept
{
    deallocate_storage(address);
}

void operator delete[](void *address, const std::nothrow_t &) noexcept
{
    deallocate_storage(address);
}

void operator delete(void *address, std::align_val_t, const std::nothrow_t &) noexcept
{
    deallocate_storage(address);
}

void operator delete[](void *address, std::align_val_t, const std::nothrow_t &) noexcept
{
    deallocate_storage(address);
}
