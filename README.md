# malloc_new

一个基于 **普通大页（hugetlb）优先** 的内存池原型，目标是：

- 避免透明大页（THP）带来的运行时抖动。
- 提供 C/C++ 常用分配接口：`malloc/aligned_alloc/calloc/realloc/free`。
- 提供可用于标准容器的 C++ Allocator：`malloc_new::HugePageAllocator<T>`。
- 提供 `new/delete` 风格的包装：`malloc_new::hp_new<T>()` / `malloc_new::hp_delete()`。

> 当前版本定位为“可运行原型”，重点验证接口、普通大页映射路径和基础行为；暂未覆盖 jemalloc/TCMalloc 全量兼容语义与性能目标。

## 构建与测试

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## 接口

头文件：`include/malloc_new/allocator.hpp`

- C 接口：
  - `void* hp_malloc(size_t)`
  - `void* hp_aligned_alloc(size_t alignment, size_t size)`
  - `void* hp_calloc(size_t count, size_t size)`
  - `void* hp_realloc(void*, size_t)`
  - `void hp_free(void*)`
- C++ Allocator：`malloc_new::HugePageAllocator<T>`
- C++ 对象包装：`malloc_new::hp_new<T>()` / `malloc_new::hp_delete()`

## 设计要点

- 小对象走固定 size class（最多 2048B payload class）+ 空闲链表。
- 每次 refill 默认映射 2MB（优先 `MAP_HUGETLB | MAP_HUGE_2MB`，失败后退化到匿名映射并 `madvise(MADV_HUGEPAGE)`）。
- 大对象走 `malloc` 回退路径，统一由头部元数据识别释放方式。

