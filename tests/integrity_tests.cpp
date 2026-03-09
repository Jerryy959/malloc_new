#include "malloc_new/allocator.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

void test_alignment_matrix() {
    const std::size_t alignments[] = {8, 16, 32, 64, 128, 256};
    const std::size_t sizes[] = {1, 7, 15, 31, 63, 127, 255, 511, 1023, 2048, 4096};
    for (std::size_t a : alignments) {
        for (std::size_t s : sizes) {
            void* p = hp_aligned_alloc(a, s);
            assert(p != nullptr);
            assert((reinterpret_cast<std::uintptr_t>(p) % a) == 0);
            std::memset(p, 0xCD, s);
            hp_free(p);
        }
    }
}

void test_realloc_growth_and_shrink() {
    std::vector<std::uint8_t> golden(4096);
    for (std::size_t i = 0; i < golden.size(); ++i) {
        golden[i] = static_cast<std::uint8_t>(i % 251);
    }

    void* p = hp_malloc(256);
    assert(p != nullptr);
    std::memcpy(p, golden.data(), 256);

    p = hp_realloc(p, 4096);
    assert(p != nullptr);
    assert(std::memcmp(p, golden.data(), 256) == 0);

    p = hp_realloc(p, 128);
    assert(p != nullptr);
    assert(std::memcmp(p, golden.data(), 128) == 0);

    hp_free(p);
}

void worker(std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> size_dist(1, 8192);

    std::vector<void*> allocated;
    allocated.reserve(4000);

    for (int i = 0; i < 4000; ++i) {
        std::size_t size = static_cast<std::size_t>(size_dist(rng));
        void* p = hp_malloc(size);
        assert(p != nullptr);
        std::memset(p, i % 255, size);
        allocated.push_back(p);
    }

    std::shuffle(allocated.begin(), allocated.end(), rng);
    for (void* p : allocated) {
        hp_free(p);
    }
}

void test_multithreaded_stress() {
    constexpr int kThreads = 4;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker, static_cast<std::uint32_t>(0xC0FFEE + i));
    }
    for (auto& t : threads) {
        t.join();
    }
}

}  // namespace

int main() {
    test_alignment_matrix();
    test_realloc_growth_and_shrink();
    test_multithreaded_stress();
    return 0;
}
