#include "malloc_new/allocator.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <atomic>
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
    std::size_t mapping_size;
    std::size_t reserved;
};

static_assert(sizeof(AllocationHeader) % alignof(std::max_align_t) == 0,
              "AllocationHeader must preserve default alignment");

struct FreeNode {
    FreeNode* next;
};

struct SizeClassPool {
    std::mutex mu;
    FreeNode* free_list{nullptr};
    std::size_t block_size{0};
    bool initialized{false};
};

struct ThreadCache {
    FreeNode* free_list{nullptr};
    std::size_t count{0};
};

std::array<SizeClassPool, kPayloadClasses.size()> g_pools;
thread_local std::array<ThreadCache, kPayloadClasses.size()> g_thread_caches;
std::atomic<std::uint64_t> g_hugepage_refill_attempts{0};
std::atomic<std::uint64_t> g_hugepage_refill_success{0};
std::atomic<std::uint64_t> g_fallback_refill_success{0};

constexpr std::uint32_t kSmallAllocFlag = 1;
constexpr std::uint32_t kMmapAllocFlag = 2;
constexpr std::size_t kLocalBatchSize = 64;
constexpr std::size_t kLocalCacheLimit = 256;

struct RefillResult {
    FreeNode* head{nullptr};
    FreeNode* tail{nullptr};
};

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

RefillResult refill_pool(std::size_t block_size) {
    g_hugepage_refill_attempts.fetch_add(1, std::memory_order_relaxed);
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
            return {};
        }
        g_fallback_refill_success.fetch_add(1, std::memory_order_relaxed);
        madvise(slab, kHugePageSize, MADV_HUGEPAGE);
    } else {
        g_hugepage_refill_success.fetch_add(1, std::memory_order_relaxed);
    }

    std::size_t capacity = kHugePageSize / block_size;
    auto* base = static_cast<unsigned char*>(slab);
    RefillResult result{};
    for (std::size_t i = 0; i < capacity; ++i) {
        auto* node = reinterpret_cast<FreeNode*>(base + i * block_size);
        node->next = result.head;
        result.head = node;
        if (!result.tail) {
            result.tail = node;
        }
    }
    return result;
}

void push_list(FreeNode*& dst, FreeNode* head, FreeNode* tail) {
    if (!head) {
        return;
    }
    tail->next = dst;
    dst = head;
}

void* alloc_small(std::size_t size, std::size_t alignment) {
    std::size_t total = round_up(sizeof(AllocationHeader), alignof(std::max_align_t)) + size;
    total = round_up(total, std::max(alignment, alignof(std::max_align_t)));

    int idx = class_index_for(total);
    if (idx < 0) {
        return nullptr;
    }

    auto& pool = g_pools[static_cast<std::size_t>(idx)];
    auto& local_cache = g_thread_caches[static_cast<std::size_t>(idx)];

    while (!local_cache.free_list) {
        {
            std::lock_guard<std::mutex> lock(pool.mu);
            initialize_pool(pool, static_cast<std::size_t>(idx));

            for (std::size_t i = 0; i < kLocalBatchSize && pool.free_list; ++i) {
                FreeNode* node = pool.free_list;
                pool.free_list = node->next;
                node->next = local_cache.free_list;
                local_cache.free_list = node;
                ++local_cache.count;
            }
        }

        if (local_cache.free_list) {
            break;
        }

        RefillResult refill = refill_pool(kPayloadClasses[static_cast<std::size_t>(idx)]);
        if (!refill.head) {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(pool.mu);
        push_list(pool.free_list, refill.head, refill.tail);
    }

    FreeNode* node = local_cache.free_list;
    local_cache.free_list = node->next;
    if (local_cache.count > 0) {
        --local_cache.count;
    }

    auto* raw = reinterpret_cast<unsigned char*>(node);
    auto* header = reinterpret_cast<AllocationHeader*>(raw);
    header->magic = kMagic;
    header->class_index = static_cast<std::uint32_t>(idx);
    header->flags = kSmallAllocFlag;
    header->requested_size = size;
    header->original = nullptr;
    header->mapping_size = 0;
    header->reserved = 0;

    void* payload = raw + sizeof(AllocationHeader);
    if (alignment > alignof(std::max_align_t)) {
        auto p = reinterpret_cast<std::uintptr_t>(payload);
        p = round_up(p, alignment);
        payload = reinterpret_cast<void*>(p);
    }
    return payload;
}

void* alloc_large_fallback(std::size_t size, std::size_t alignment, bool zeroed) {
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
    header->mapping_size = 0;
    header->reserved = 0;

    void* payload = reinterpret_cast<void*>(aligned);
    if (zeroed) {
        std::memset(payload, 0, size);
    }
    return payload;
}

void* alloc_large(std::size_t size, std::size_t alignment, bool zeroed) {
    if (alignment > alignof(std::max_align_t)) {
        return alloc_large_fallback(size, alignment, zeroed);
    }

    std::size_t payload_offset = sizeof(AllocationHeader);
    std::size_t requested = payload_offset + size;
    if (requested < size) {
        return nullptr;
    }

    std::size_t map_size = round_up(requested, kHugePageSize);
    void* raw = mmap(nullptr,
                     map_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
                     -1,
                     0);
    if (raw == MAP_FAILED) {
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) {
            page_size = 4096;
        }
        map_size = round_up(requested, static_cast<std::size_t>(page_size));
        raw = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            return alloc_large_fallback(size, alignment, zeroed);
        }
        madvise(raw, map_size, MADV_HUGEPAGE);
    }

    auto* raw_bytes = static_cast<unsigned char*>(raw);
    auto* header = reinterpret_cast<AllocationHeader*>(raw_bytes);
    header->magic = kMagic;
    header->class_index = 0;
    header->flags = kMmapAllocFlag;
    header->requested_size = size;
    header->original = raw;
    header->mapping_size = map_size;
    header->reserved = 0;

    void* payload = raw_bytes + payload_offset;
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

    if (header->flags == kSmallAllocFlag) {
        std::size_t idx = header->class_index;
        auto& pool = g_pools[idx];
        auto& local_cache = g_thread_caches[idx];
        auto* node = reinterpret_cast<FreeNode*>(header);

        node->next = local_cache.free_list;
        local_cache.free_list = node;
        ++local_cache.count;

        if (local_cache.count > kLocalCacheLimit) {
            FreeNode* flush_head = nullptr;
            FreeNode* flush_tail = nullptr;
            std::size_t flushed = 0;
            while (local_cache.free_list && local_cache.count > kLocalBatchSize) {
                FreeNode* current = local_cache.free_list;
                local_cache.free_list = current->next;
                --local_cache.count;

                current->next = flush_head;
                flush_head = current;
                if (!flush_tail) {
                    flush_tail = current;
                }
                ++flushed;
            }

            if (flushed != 0) {
                std::lock_guard<std::mutex> lock(pool.mu);
                push_list(pool.free_list, flush_head, flush_tail);
            }
        }
        return;
    }

    if (header->flags & kMmapAllocFlag) {
        munmap(header->original, header->mapping_size);
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


extern "C" hp_allocator_stats hp_get_allocator_stats() noexcept {
    hp_allocator_stats stats{};
    stats.hugepage_refill_attempts = g_hugepage_refill_attempts.load(std::memory_order_relaxed);
    stats.hugepage_refill_success = g_hugepage_refill_success.load(std::memory_order_relaxed);
    stats.fallback_refill_success = g_fallback_refill_success.load(std::memory_order_relaxed);
    return stats;
}
