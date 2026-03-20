#include "malloc_new/allocator.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
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
    std::atomic<FreeNode*> free_list{nullptr};
};

struct ThreadCache {
    FreeNode* free_list{nullptr};
    std::size_t count{0};
};

void flush_thread_cache(std::size_t idx, ThreadCache& local_cache);

struct ThreadCaches {
    std::array<ThreadCache, kPayloadClasses.size()> caches{};

    ~ThreadCaches() {
        for (std::size_t i = 0; i < caches.size(); ++i) {
            flush_thread_cache(i, caches[i]);
        }
    }
};

std::array<SizeClassPool, kPayloadClasses.size()> g_pools;
thread_local ThreadCaches g_thread_caches;
std::atomic<std::uint64_t> g_hugepage_refill_attempts{0};
std::atomic<std::uint64_t> g_hugepage_refill_success{0};
std::atomic<std::uint64_t> g_fallback_refill_success{0};
std::atomic<std::uint64_t> g_thread_exit_flush_blocks{0};

constexpr std::uint32_t kSmallAllocFlag = 1;
constexpr std::uint32_t kMmapAllocFlag = 2;
constexpr std::size_t kLocalBatchSize = 64;
constexpr std::size_t kLocalCacheLimit = 256;

struct RefillResult {
    FreeNode* local_head{nullptr};
    std::size_t local_count{0};
    FreeNode* shared_head{nullptr};
    FreeNode* shared_tail{nullptr};
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

void push_list_atomic(std::atomic<FreeNode*>& dst, FreeNode* head, FreeNode* tail) {
    if (!head) {
        return;
    }

    FreeNode* observed = dst.load(std::memory_order_relaxed);
    do {
        tail->next = observed;
    } while (!dst.compare_exchange_weak(observed,
                                        head,
                                        std::memory_order_release,
                                        std::memory_order_relaxed));
}

std::size_t pop_batch_atomic(std::atomic<FreeNode*>& src, FreeNode*& out_head, std::size_t max_count) {
    out_head = nullptr;
    if (max_count == 0) {
        return 0;
    }

    FreeNode* observed = src.load(std::memory_order_acquire);
    while (observed) {
        FreeNode* cursor = observed;
        std::size_t count = 1;
        while (count < max_count && cursor->next) {
            cursor = cursor->next;
            ++count;
        }

        FreeNode* remainder = cursor->next;
        if (src.compare_exchange_weak(observed,
                                      remainder,
                                      std::memory_order_acquire,
                                      std::memory_order_relaxed)) {
            cursor->next = nullptr;
            out_head = observed;
            return count;
        }
    }
    return 0;
}

RefillResult refill_local_cache(std::size_t block_size) {
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

    const std::size_t capacity = kHugePageSize / block_size;
    const std::size_t local_target = std::min(capacity, kLocalCacheLimit);
    auto* base = static_cast<unsigned char*>(slab);

    RefillResult result{};
    FreeNode* local_tail = nullptr;
    for (std::size_t i = 0; i < capacity; ++i) {
        auto* node = reinterpret_cast<FreeNode*>(base + i * block_size);
        if (i < local_target) {
            node->next = result.local_head;
            result.local_head = node;
            if (!local_tail) {
                local_tail = node;
            }
            ++result.local_count;
            continue;
        }

        node->next = result.shared_head;
        result.shared_head = node;
        if (!result.shared_tail) {
            result.shared_tail = node;
        }
    }
    return result;
}

void flush_thread_cache(std::size_t idx, ThreadCache& local_cache) {
    if (!local_cache.free_list) {
        local_cache.count = 0;
        return;
    }

    FreeNode* flush_head = local_cache.free_list;
    const std::size_t flushed_count = local_cache.count;
    FreeNode* flush_tail = flush_head;
    while (flush_tail->next) {
        flush_tail = flush_tail->next;
    }

    local_cache.free_list = nullptr;
    local_cache.count = 0;
    g_thread_exit_flush_blocks.fetch_add(flushed_count, std::memory_order_relaxed);
    push_list_atomic(g_pools[idx].free_list, flush_head, flush_tail);
}

void* alloc_small(std::size_t size, std::size_t alignment) {
    std::size_t total = round_up(sizeof(AllocationHeader), alignof(std::max_align_t)) + size;
    total = round_up(total, std::max(alignment, alignof(std::max_align_t)));

    int idx = class_index_for(total);
    if (idx < 0) {
        return nullptr;
    }

    auto& pool = g_pools[static_cast<std::size_t>(idx)];
    auto& local_cache = g_thread_caches.caches[static_cast<std::size_t>(idx)];

    if (!local_cache.free_list) {
        FreeNode* batch = nullptr;
        local_cache.count = pop_batch_atomic(pool.free_list, batch, kLocalBatchSize);
        local_cache.free_list = batch;

        if (!local_cache.free_list) {
            RefillResult refill = refill_local_cache(kPayloadClasses[static_cast<std::size_t>(idx)]);
            if (!refill.local_head) {
                return nullptr;
            }
            local_cache.free_list = refill.local_head;
            local_cache.count = refill.local_count;
            push_list_atomic(pool.free_list, refill.shared_head, refill.shared_tail);
        }
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
        auto& local_cache = g_thread_caches.caches[idx];
        auto* node = reinterpret_cast<FreeNode*>(header);

        node->next = local_cache.free_list;
        local_cache.free_list = node;
        ++local_cache.count;

        if (local_cache.count > kLocalCacheLimit) {
            FreeNode* retained_tail = local_cache.free_list;
            for (std::size_t i = 1; i < kLocalBatchSize && retained_tail; ++i) {
                retained_tail = retained_tail->next;
            }

            if (retained_tail) {
                ThreadCache flush_cache{};
                flush_cache.free_list = retained_tail->next;
                flush_cache.count = local_cache.count - kLocalBatchSize;
                retained_tail->next = nullptr;
                local_cache.count = kLocalBatchSize;
                flush_thread_cache(idx, flush_cache);
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


extern "C" std::uint64_t hp_debug_thread_exit_flush_blocks() noexcept {
    return g_thread_exit_flush_blocks.load(std::memory_order_relaxed);
}

extern "C" hp_allocator_stats hp_get_allocator_stats() noexcept {
    hp_allocator_stats stats{};
    stats.hugepage_refill_attempts = g_hugepage_refill_attempts.load(std::memory_order_relaxed);
    stats.hugepage_refill_success = g_hugepage_refill_success.load(std::memory_order_relaxed);
    stats.fallback_refill_success = g_fallback_refill_success.load(std::memory_order_relaxed);
    return stats;
}
