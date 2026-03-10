#include "malloc_new/allocator.hpp"

namespace malloc_new::compat {

void* malloc(std::size_t size) noexcept {
    return hp_malloc(size);
}

void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
    return hp_aligned_alloc(alignment, size);
}

void* calloc(std::size_t count, std::size_t size) noexcept {
    return hp_calloc(count, size);
}

void* realloc(void* ptr, std::size_t size) noexcept {
    return hp_realloc(ptr, size);
}

void free(void* ptr) noexcept {
    hp_free(ptr);
}

}  // namespace malloc_new::compat
