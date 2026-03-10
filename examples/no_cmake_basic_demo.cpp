#include "malloc_new/allocator.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int main() {
    void* p = hp_malloc(64);
    assert(p != nullptr);
    std::memset(p, 0x11, 64);
    hp_free(p);

    void* a = hp_aligned_alloc(64, 128);
    assert(a != nullptr);
    assert((reinterpret_cast<std::uintptr_t>(a) % 64) == 0);
    hp_free(a);

    int* z = static_cast<int*>(hp_calloc(8, sizeof(int)));
    assert(z != nullptr);
    for (int i = 0; i < 8; ++i) {
        assert(z[i] == 0);
    }
    hp_free(z);

    char* r = static_cast<char*>(hp_malloc(8));
    std::strcpy(r, "ok");
    r = static_cast<char*>(hp_realloc(r, 32));
    assert(std::string(r) == "ok");
    hp_free(r);

    std::vector<int, malloc_new::HugePageAllocator<int>> v;
    for (int i = 0; i < 32; ++i) {
        v.push_back(i);
    }
    assert(v[31] == 31);

    auto* s = malloc_new::hp_new<std::string>("malloc_new");
    assert(*s == "malloc_new");
    malloc_new::hp_delete(s);

    std::cout << "no-cmake basic demo passed" << std::endl;
    return 0;
}
