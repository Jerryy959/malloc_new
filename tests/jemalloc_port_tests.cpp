#include "malloc_new/allocator.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>

int main() {
    // Ported core semantics from jemalloc-style API expectations.
    void* p = hp_malloc(0);
    assert(p != nullptr);
    hp_free(p);

    errno = 0;
    void* bad_align = hp_aligned_alloc(3, 64);
    assert(bad_align == nullptr);
    assert(errno == EINVAL);

    void* c = hp_calloc(128, sizeof(std::uint32_t));
    assert(c != nullptr);
    auto* words = static_cast<std::uint32_t*>(c);
    for (int i = 0; i < 128; ++i) {
        assert(words[i] == 0);
    }

    // realloc(nullptr, n) == malloc(n)
    void* r = hp_realloc(nullptr, 128);
    assert(r != nullptr);
    std::memset(r, 0x5A, 128);

    // realloc(p, 0) frees and returns nullptr
    void* r0 = hp_realloc(r, 0);
    assert(r0 == nullptr);

    hp_free(c);
    return 0;
}
