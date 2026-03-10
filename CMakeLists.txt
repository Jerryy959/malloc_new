cmake_minimum_required(VERSION 3.16)
project(malloc_new LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MALLOC_NEW_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MALLOC_NEW_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

add_library(malloc_new STATIC
    src/huge_page_allocator.cpp
    src/compat.cpp
)

target_include_directories(malloc_new PUBLIC include)

if (MALLOC_NEW_ENABLE_ASAN)
    target_compile_options(malloc_new PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(malloc_new PUBLIC -fsanitize=address)
endif()

if (MALLOC_NEW_ENABLE_UBSAN)
    target_compile_options(malloc_new PUBLIC -fsanitize=undefined)
    target_link_options(malloc_new PUBLIC -fsanitize=undefined)
endif()

include(CTest)

if (BUILD_TESTING)

add_executable(malloc_new_smoke_tests tests/smoke_tests.cpp)
target_link_libraries(malloc_new_smoke_tests PRIVATE malloc_new Threads::Threads)
add_test(NAME smoke COMMAND malloc_new_smoke_tests)

add_executable(malloc_new_integrity_tests tests/integrity_tests.cpp)
target_link_libraries(malloc_new_integrity_tests PRIVATE malloc_new Threads::Threads)
add_test(NAME integrity COMMAND malloc_new_integrity_tests)

add_executable(malloc_new_jemalloc_port_tests tests/jemalloc_port_tests.cpp)
target_link_libraries(malloc_new_jemalloc_port_tests PRIVATE malloc_new Threads::Threads)
add_test(NAME jemalloc_port COMMAND malloc_new_jemalloc_port_tests)

add_executable(malloc_new_tcmalloc_port_tests tests/tcmalloc_port_tests.cpp)
target_link_libraries(malloc_new_tcmalloc_port_tests PRIVATE malloc_new Threads::Threads)
add_test(NAME tcmalloc_port COMMAND malloc_new_tcmalloc_port_tests)

add_executable(malloc_new_latency_bench tests/latency_bench.cpp)
target_link_libraries(malloc_new_latency_bench PRIVATE malloc_new Threads::Threads)

endif()
