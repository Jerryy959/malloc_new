# malloc_new

`malloc_new` 是一个 **普通大页（hugetlb）优先** 的内存分配器原型，目标是降低交易场景中由透明大页（THP）引入的运行时抖动。

> 当前仓库是“工程化原型”阶段：
> - 已提供 C/C++ 接口、可用于 STL 的 Allocator、基础测试框架；
> - 还不是 jemalloc/TCMalloc 的完整替代品；
> - 尚未达到生产级完整语义兼容与严格性能 SLA（例如 p50 < 10ns, p99 < 100ns）。

---

## 1. 功能概览

### 1.1 已提供接口

头文件：`include/malloc_new/allocator.hpp`

- C 风格接口
  - `void* hp_malloc(size_t size)`
  - `void* hp_aligned_alloc(size_t alignment, size_t size)`
  - `void* hp_calloc(size_t count, size_t size)`
  - `void* hp_realloc(void* ptr, size_t size)`
  - `void hp_free(void* ptr)`
- C++ Allocator
  - `malloc_new::HugePageAllocator<T>`（可作为 `std::vector/std::map` 等容器 allocator 参数）
- C++ 对象包装
  - `malloc_new::hp_new<T>(...)`
  - `malloc_new::hp_delete(ptr)`

### 1.2 当前分配策略

- 小对象：固定 size class + freelist。
- refill：优先尝试 `MAP_HUGETLB | MAP_HUGE_2MB`。
- 如果 hugetlb 失败：回退到匿名 `mmap`，并调用 `madvise(MADV_HUGEPAGE)`。
- 大对象：当前回退到 `malloc` 路径。

---

## 2. 如何使用这个库

## 2.1 构建

```bash
cmake -S . -B build
cmake --build build -j
```

产物：`build/libmalloc_new.a`。

### 2.2 在你的 C++ 工程中链接

假设你的项目使用 CMake：

```cmake
add_subdirectory(path/to/malloc_new)
target_link_libraries(your_target PRIVATE malloc_new)
```


### 2.6 非 CMake 环境：手工编译 `libmalloc_new.a` 并调用

如果你的主工程不用 CMake，也可以直接用 `g++/ar` 生成静态库并链接：

```bash
# 1) 编译 allocator 实现为对象文件
g++ -O2 -Iinclude -c src/huge_page_allocator.cpp -o huge_page_allocator.o

# 2) 打包为静态库
ar rcs libmalloc_new.a huge_page_allocator.o

# 3) 编译并链接示例程序（不依赖 CMake）
g++ -O2 -Iinclude examples/no_cmake_basic_demo.cpp ./libmalloc_new.a -lpthread -o no_cmake_basic_demo

# 4) 运行验证
./no_cmake_basic_demo
```

示例程序位于 `examples/no_cmake_basic_demo.cpp`，覆盖了基础 API（`hp_malloc/hp_aligned_alloc/hp_calloc/hp_realloc/hp_free`）、
STL allocator 用法和 `hp_new/hp_delete`。

> 说明：当前代码避免了 C++17 专属语法；一般情况下直接 `g++ -O2` 即可编译。若你的工具链默认标准过老，可显式加 `-std=c++11` 或更高。

### 2.3 直接调用 C 接口

```cpp
#include "malloc_new/allocator.hpp"

void demo_c_api() {
    void* p = hp_malloc(128);
    p = hp_realloc(p, 256);
    hp_free(p);
}
```

### 2.4 作为 STL Allocator 使用

```cpp
#include "malloc_new/allocator.hpp"
#include <vector>

void demo_stl_allocator() {
    std::vector<int, malloc_new::HugePageAllocator<int>> v;
    v.push_back(1);
    v.push_back(2);
}
```

### 2.5 对象 new/delete 风格包装

```cpp
#include "malloc_new/allocator.hpp"
#include <string>

void demo_new_delete_wrapper() {
    auto* s = malloc_new::hp_new<std::string>("hello");
    malloc_new::hp_delete(s);
}
```

---

## 3. 如何测试

### 3.1 基础功能测试（默认）

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
cmake --build build --target test
# 或者： (cd build && ctest --output-on-failure)
```

也可以使用仓库脚本（默认 build 目录）：

```bash
./tools/run_tests.sh
```

当前包含两类测试：

- `smoke`：基础接口冒烟
  - malloc/free
  - aligned_alloc
  - calloc
  - realloc
  - STL allocator 兼容性
  - hp_new/hp_delete
- `integrity`：完整性/稳健性测试
  - 多组对齐矩阵校验
  - realloc 扩容/缩容数据保持
  - 多线程分配释放压力

### 3.2 Sanitizer 测试（建议 CI 必跑）

AddressSanitizer：

```bash
cmake -S . -B build-asan -DMALLOC_NEW_ENABLE_ASAN=ON
cmake --build build-asan -j
(cd build-asan && ctest --output-on-failure)
```

UBSan：

```bash
cmake -S . -B build-ubsan -DMALLOC_NEW_ENABLE_UBSAN=ON
cmake --build build-ubsan -j
(cd build-ubsan && ctest --output-on-failure)
```

### 3.4 非 CMake 场景下的基本功能验证

你可以直接复用上面的手工构建命令，作为“脱离 CMake 的最小验收”：

```bash
g++ -O2 -Iinclude -c src/huge_page_allocator.cpp -o huge_page_allocator.o
ar rcs libmalloc_new.a huge_page_allocator.o
g++ -O2 -Iinclude examples/no_cmake_basic_demo.cpp ./libmalloc_new.a -lpthread -o no_cmake_basic_demo
./no_cmake_basic_demo
```

只要输出 `no-cmake basic demo passed` 且进程退出码为 0，即表示在非 CMake 环境下可成功链接并完成基本功能验证。

### 3.5 排查：`No tests were found!!!`

如果你看到了 `No tests were found!!!`，最常见原因是 **ctest 没有在构建目录中执行**（或你的 ctest 版本对 `--test-dir` 支持不一致）。

建议使用最稳妥的方式（优先 `cmake --build build --target test`）：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
cmake --build build --target test
# 或者：cd build && ctest --output-on-failure
```

快速自检：

```bash
# 构建目录下应存在 CTestTestfile.cmake
ls build/CTestTestfile.cmake
```

如果该文件存在但依旧无测试，请确认配置阶段没有关闭测试：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
```

### 3.3 完整性测试建议（补充）

若你要把它用于交易场景，建议把“完整性”拆为 4 层：

1. **API 语义完整性**
   - 与 libc 语义对齐（0-size、对齐非法参数、realloc 边界等）。
2. **并发安全完整性**
   - 多线程高并发下无崩溃、无明显数据破坏。
3. **内存安全完整性**
   - ASAN/UBSAN 下长期压力无报错。
4. **系统行为完整性**
   - 在配置了 hugetlb 的机器验证是否真实走到 hugetlb 映射；
   - 在未配置 hugetlb 的机器验证回退路径稳定。

---

## 4. 能否复用 jemalloc 的测试？

可以“部分复用”，但需要做适配，不能直接无改动跑全套。

原因：

1. 本仓库当前导出的是 `hp_*` 接口，不是全局替换 `malloc/free`。
2. jemalloc 测试里有不少依赖 jemalloc 私有行为、统计接口、配置开关。
3. 还没有实现 jemalloc 全量语义（arena、tcache、profiling、extent hooks 等）。

### 推荐落地方式

- **第一步（低成本）**：挑选 jemalloc 中与通用 malloc 语义强相关、且不依赖私有 API 的 case，移植为本项目测试。
- **第二步（中成本）**：增加一个 `compat shim`（把 `malloc/free` 转发到 `hp_*`），用于接入更多通用测试。
- **第三步（高成本）**：若目标是“通过 jemalloc 大部分测试集”，需要先补齐行为语义，再分批接入。

---

## 5. 测试环境建议

至少准备两类环境：

1. **功能环境（开发机/CI）**
   - Linux x86_64
   - GCC 13+ / Clang 16+
   - CMake 3.16+
2. **性能/大页环境（基准机）**
   - 明确 THP 配置（建议关闭）
   - 配置 hugetlb 页池（如 2MB hugepages）
   - 固定 CPU 频率、绑定核、隔离噪声进程

建议记录：内核版本、hugetlb 配置、THP 状态、NUMA 拓扑、编译器版本、编译选项。

---

## 6. 后续路线（建议）

- 增加 `malloc/free` 兼容导出层（可选）用于外部测试集接入。
- 增加基准测试（p50/p99 延迟、不同对象尺寸分布）。
- 增加分配器统计与可观测性（命中率、回退率、失败率）。
- 引入更细粒度并发结构（降低全局锁竞争）。
