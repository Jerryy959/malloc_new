#include "malloc_new/allocator.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

int main() {
    {
        void* p = hp_malloc(64);
        assert(p != nullptr);
        std::memset(p, 0xAB, 64);
        hp_free(p);
    }

    {
        void* p = hp_aligned_alloc(64, 128);
        assert(p != nullptr);
        assert((reinterpret_cast<std::uintptr_t>(p) % 64) == 0);
        hp_free(p);
    }

    {
        int* a = static_cast<int*>(hp_calloc(16, sizeof(int)));
        assert(a != nullptr);
        for (int i = 0; i < 16; ++i) {
            assert(a[i] == 0);
        }
        hp_free(a);
    }

    {
        char* p = static_cast<char*>(hp_malloc(8));
        std::strcpy(p, "abc");
        p = static_cast<char*>(hp_realloc(p, 32));
        assert(std::string(p) == "abc");
        hp_free(p);
    }

    {
        std::vector<int, malloc_new::HugePageAllocator<int>> v;
        for (int i = 0; i < 1024; ++i) {
            v.push_back(i);
        }
        assert(v[123] == 123);
    }

    {
        auto* s = malloc_new::hp_new<std::string>("hello");
        assert(*s == "hello");
        malloc_new::hp_delete(s);
    }

    return 0;
}
