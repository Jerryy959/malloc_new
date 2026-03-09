#pragma once

#include <cstddef>
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
}

namespace malloc_new {

template <typename T>
class HugePageAllocator {
public:
    using value_type = T;

    HugePageAllocator() noexcept = default;
    template <typename U>
    HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > (std::numeric_limits<std::size_t>::max() / sizeof(T))) {
            throw std::bad_array_new_length();
        }
        if (void* p = hp_aligned_alloc(alignof(T), n * sizeof(T)); p != nullptr) {
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
