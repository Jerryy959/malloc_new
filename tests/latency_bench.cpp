#include "malloc_new/allocator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

std::uint64_t percentile(const std::vector<std::uint64_t>& v, double p) {
    const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return v[idx];
}

void run_case(const char* name, std::size_t bytes, int iters) {
    std::vector<std::uint64_t> samples;
    samples.reserve(static_cast<std::size_t>(iters));

    for (int i = 0; i < iters; ++i) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        void* p = hp_malloc(bytes);
        hp_free(p);
        const auto t1 = std::chrono::high_resolution_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples.push_back(static_cast<std::uint64_t>(ns));
    }

    std::sort(samples.begin(), samples.end());
    const auto p50 = percentile(samples, 0.50);
    const auto p99 = percentile(samples, 0.99);
    std::printf("latency[%s]: p50=%lluns p99=%lluns (iters=%d)\n",
                name,
                static_cast<unsigned long long>(p50),
                static_cast<unsigned long long>(p99),
                iters);
}

}  // namespace

int main() {
    run_case("small_64B", 64, 200000);
    run_case("small_256B", 256, 200000);

    const hp_allocator_stats stats = hp_get_allocator_stats();
    std::printf("hugepage_stats: attempts=%llu success=%llu fallback=%llu\n",
                static_cast<unsigned long long>(stats.hugepage_refill_attempts),
                static_cast<unsigned long long>(stats.hugepage_refill_success),
                static_cast<unsigned long long>(stats.fallback_refill_success));
    return 0;
}
