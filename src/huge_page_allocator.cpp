#include "malloc_new/allocator.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <unistd.h>


#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

namespace {

constexpr std::uint64_t kMagic = 0x4D414C4C4F43504FULL;  // "MALLOCPO"
constexpr std::size_t kHugePageSize = 2ULL * 1024ULL * 1024ULL;
constexpr std::array<std::size_t, 12> kPayloadClasses = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 1024, 2048};

struct AllocationHeader {
    std::uint64_t magic;
    std::uint32_t class_index;
    std::uint32_t flags;
    std::size_t requested_size;
    void* original;
};

struct FreeNode {
    FreeNode* next;
};

struct SizeClassPool {
    std::mutex mu;
    FreeNode* free_list{nullptr};
    std::size_t block_size{0};
    bool initialized{false};
};

std::array<SizeClassPool, kPayloadClasses.size()> g_pools;

inline std::size_t round_up(std::size_t x, std::size_t align) {
    return (x + (align - 1)) & ~(align - 1);
}

int class_index_for(std::size_t total) {
    for (std::size_t i = 0; i < kPayloadClasses.size(); ++i) {
        if (total <= kPayloadClasses[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void initialize_pool(SizeClassPool& pool, std::size_t class_index) {
    if (pool.initialized) {
        return;
    }
    pool.block_size = kPayloadClasses[class_index];
    pool.initialized = true;
}

void refill_pool(SizeClassPool& pool) {
    void* slab = mmap(nullptr,
                      kHugePageSize,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
                      -1,
                      0);

    if (slab == MAP_FAILED) {
        slab = mmap(nullptr,
                    kHugePageSize,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1,
                    0);
        if (slab == MAP_FAILED) {
            return;
        }
        madvise(slab, kHugePageSize, MADV_HUGEPAGE);
    }

    std::size_t capacity = kHugePageSize / pool.block_size;
    auto* base = static_cast<unsigned char*>(slab);
    for (std::size_t i = 0; i < capacity; ++i) {
        auto* node = reinterpret_cast<FreeNode*>(base + i * pool.block_size);
        node->next = pool.free_list;
        pool.free_list = node;
    }
}

void* alloc_small(std::size_t size, std::size_t alignment) {
    std::size_t total = round_up(sizeof(AllocationHeader), alignof(std::max_align_t)) + size;
    total = round_up(total, std::max(alignment, alignof(std::max_align_t)));

    int idx = class_index_for(total);
    if (idx < 0) {
        return nullptr;
    }

    auto& pool = g_pools[static_cast<std::size_t>(idx)];
    std::lock_guard<std::mutex> lock(pool.mu);
    initialize_pool(pool, static_cast<std::size_t>(idx));
    if (!pool.free_list) {
        refill_pool(pool);
        if (!pool.free_list) {
            return nullptr;
        }
    }

    FreeNode* node = pool.free_list;
    pool.free_list = node->next;

    auto* raw = reinterpret_cast<unsigned char*>(node);
    auto* header = reinterpret_cast<AllocationHeader*>(raw);
    header->magic = kMagic;
    header->class_index = static_cast<std::uint32_t>(idx);
    header->flags = 1;
    header->requested_size = size;
    header->original = nullptr;

    void* payload = raw + round_up(sizeof(AllocationHeader), alignof(std::max_align_t));
    if (alignment > alignof(std::max_align_t)) {
        auto p = reinterpret_cast<std::uintptr_t>(payload);
        p = round_up(p, alignment);
        payload = reinterpret_cast<void*>(p);
    }
    return payload;
}

void* alloc_large(std::size_t size, std::size_t alignment, bool zeroed) {
    std::size_t extra = sizeof(AllocationHeader) + alignment;
    if (size > (std::numeric_limits<std::size_t>::max() - extra)) {
        return nullptr;
    }

    void* raw = std::malloc(size + extra);
    if (!raw) {
        return nullptr;
    }

    auto base = reinterpret_cast<std::uintptr_t>(raw) + sizeof(AllocationHeader);
    auto aligned = round_up(base, alignment);
    auto* header = reinterpret_cast<AllocationHeader*>(aligned - sizeof(AllocationHeader));
    header->magic = kMagic;
    header->class_index = 0;
    header->flags = 0;
    header->requested_size = size;
    header->original = raw;

    void* payload = reinterpret_cast<void*>(aligned);
    if (zeroed) {
        std::memset(payload, 0, size);
    }
    return payload;
}

AllocationHeader* get_header(void* ptr) {
    if (!ptr) {
        return nullptr;
    }
    auto* header = reinterpret_cast<AllocationHeader*>(reinterpret_cast<unsigned char*>(ptr) - sizeof(AllocationHeader));
    if (header->magic != kMagic) {
        return nullptr;
    }
    return header;
}

}  // namespace

extern "C" void* hp_malloc(std::size_t size) noexcept {
    if (size == 0) {
        size = 1;
    }
    void* p = alloc_small(size, alignof(std::max_align_t));
    if (p) {
        return p;
    }
    return alloc_large(size, alignof(std::max_align_t), false);
}

extern "C" void* hp_aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return nullptr;
    }
    if (size == 0) {
        size = alignment;
    }
    if (alignment <= alignof(std::max_align_t)) {
        if (void* p = alloc_small(size, alignment)) {
            return p;
        }
    }
    return alloc_large(size, alignment, false);
}

extern "C" void* hp_calloc(std::size_t count, std::size_t size) noexcept {
    if (count == 0 || size == 0) {
        return hp_malloc(1);
    }
    if (count > (std::numeric_limits<std::size_t>::max() / size)) {
        return nullptr;
    }
    std::size_t total = count * size;
    void* ptr = hp_malloc(total);
    if (ptr) {
        std::memset(ptr, 0, total);
    }
    return ptr;
}

extern "C" void hp_free(void* ptr) noexcept {
    if (!ptr) {
        return;
    }

    AllocationHeader* header = get_header(ptr);
    if (!header) {
        std::free(ptr);
        return;
    }

    if (header->flags == 1) {
        std::size_t idx = header->class_index;
        auto& pool = g_pools[idx];
        std::lock_guard<std::mutex> lock(pool.mu);
        auto* node = reinterpret_cast<FreeNode*>(header);
        node->next = pool.free_list;
        pool.free_list = node;
        return;
    }

    std::free(header->original);
}

extern "C" void* hp_realloc(void* ptr, std::size_t size) noexcept {
    if (!ptr) {
        return hp_malloc(size);
    }
    if (size == 0) {
        hp_free(ptr);
        return nullptr;
    }

    AllocationHeader* header = get_header(ptr);
    if (!header) {
        return std::realloc(ptr, size);
    }

    std::size_t old_size = header->requested_size;
    if (size <= old_size) {
        header->requested_size = size;
        return ptr;
    }

    void* new_ptr = hp_malloc(size);
    if (!new_ptr) {
        return nullptr;
    }
    std::memcpy(new_ptr, ptr, old_size);
    hp_free(ptr);
    return new_ptr;
}
