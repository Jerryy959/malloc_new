cmake_minimum_required(VERSION 3.16)
project(malloc_new LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(malloc_new STATIC
    src/huge_page_allocator.cpp
)

target_include_directories(malloc_new PUBLIC include)

enable_testing()
add_executable(malloc_new_tests tests/smoke_tests.cpp)
target_link_libraries(malloc_new_tests PRIVATE malloc_new)
add_test(NAME smoke COMMAND malloc_new_tests)
