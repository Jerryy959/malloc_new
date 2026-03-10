#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

extern "C" {
void* hp_malloc(std::size_t size) noexcept;
void* hp_aligned_alloc(std::size_t alignment, std::size_t size) noexcept;
void* hp_calloc(std::size_t count, std::size_t size) noexcept;
void* hp_realloc(void* ptr, std::size_t size) noexcept;
void hp_free(void* ptr) noexcept;

struct hp_allocator_stats {
    std::uint64_t hugepage_refill_attempts;
    std::uint64_t hugepage_refill_success;
    std::uint64_t fallback_refill_success;
};

hp_allocator_stats hp_get_allocator_stats() noexcept;
}

namespace malloc_new {

template <typename T>
class HugePageAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    template <typename U>
    struct rebind {
        using other = HugePageAllocator<U>;
    };

    HugePageAllocator() noexcept = default;
    template <typename U>
    HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > (std::numeric_limits<std::size_t>::max() / sizeof(T))) {
            throw std::bad_array_new_length();
        }
        void* p = hp_aligned_alloc(alignof(T), n * sizeof(T));
        if (p != nullptr) {
            return static_cast<T*>(p);
        }
        throw std::bad_alloc();
    }

    void deallocate(T* p, std::size_t) noexcept {
        hp_free(p);
    }

    template <typename U>
    bool operator==(const HugePageAllocator<U>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const HugePageAllocator<U>&) const noexcept {
        return false;
    }
};

template <typename T, typename... Args>
T* hp_new(Args&&... args) {
    void* raw = hp_aligned_alloc(alignof(T), sizeof(T));
    if (!raw) {
        throw std::bad_alloc();
    }
    return ::new (raw) T(std::forward<Args>(args)...);
}

template <typename T>
void hp_delete(T* ptr) noexcept {
    if (!ptr) {
        return;
    }
    ptr->~T();
    hp_free(ptr);
}

}  // namespace malloc_new

namespace malloc_new::compat {
void* malloc(std::size_t size) noexcept;
void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept;
void* calloc(std::size_t count, std::size_t size) noexcept;
void* realloc(void* ptr, std::size_t size) noexcept;
void free(void* ptr) noexcept;

template <typename T, typename... Args>
T* new_object(Args&&... args) {
    return hp_new<T>(std::forward<Args>(args)...);
}

template <typename T>
void delete_object(T* ptr) noexcept {
    hp_delete(ptr);
}
}  // namespace malloc_new::compat
