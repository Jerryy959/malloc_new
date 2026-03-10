#include "malloc_new/allocator.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

struct Tick {
    std::uint64_t seq;
    double px;
};

int main() {
    // Ported from tcmalloc-like small-object reuse + thread-safe churn ideas.
    std::vector<void*> ptrs;
    ptrs.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        void* p = hp_malloc(64);
        assert(p != nullptr);
        std::memset(p, i % 251, 64);
        ptrs.push_back(p);
    }
    for (void* p : ptrs) {
        hp_free(p);
    }

    // STL Allocator requirement check in realistic container growth.
    std::vector<Tick, malloc_new::HugePageAllocator<Tick>> ticks;
    for (std::uint64_t i = 0; i < 4096; ++i) {
        ticks.push_back(Tick{i, static_cast<double>(i) * 0.25});
    }
    assert(ticks[123].seq == 123);

    // Standard-like adapter API check.
    void* q = malloc_new::compat::malloc(256);
    assert(q != nullptr);
    q = malloc_new::compat::realloc(q, 512);
    assert(q != nullptr);
    malloc_new::compat::free(q);

    auto* t = malloc_new::compat::new_object<Tick>(Tick{7, 3.14});
    assert(t->seq == 7);
    malloc_new::compat::delete_object(t);
    return 0;
}
