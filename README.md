# malloc_new

`malloc_new` 是一个 **普通大页（hugetlb）优先** 的内存分配器原型，目标是降低交易场景中由透明大页（THP）引入的运行时抖动。

## 功能概览

### 已提供接口

头文件：`include/malloc_new/allocator.hpp`

- C 风格接口
  - `hp_malloc / hp_aligned_alloc / hp_calloc / hp_realloc / hp_free`
- C++ 标准库 Allocator
  - `malloc_new::HugePageAllocator<T>`（支持作为 STL 容器 allocator 参数）
- C++ new/delete 包装
  - `malloc_new::hp_new<T>(...)`
  - `malloc_new::hp_delete(ptr)`
- 标准接口兼容层（适配器）
  - `malloc_new::compat::malloc / aligned_alloc / calloc / realloc / free`
  - `malloc_new::compat::new_object / delete_object`

### 大页行为

- 小对象从 size class 池分配。
- refill 时优先 `MAP_HUGETLB | MAP_HUGE_2MB`。
- 若 hugetlb 失败，回退匿名 `mmap + madvise(MADV_HUGEPAGE)`。
- 可通过 `hp_get_allocator_stats()` 查看：
  - hugepage refill 尝试次数
  - hugepage refill 成功次数
  - 回退路径成功次数

## 构建

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DMALLOC_NEW_BUILD_BENCHMARKS=ON
cmake --build build -j
```

> 如果你仅执行过旧 build 目录上的 `cmake --build build -j`，建议先重新配置一次（上面的 `cmake -S ...`），确保 `malloc_new_latency_bench` 目标被生成。

## 测试

```bash
cmake --build build --target test
```

当前测试集：

- `smoke`：基础接口冒烟
- `integrity`：对齐、realloc、多线程压力
- `jemalloc_port`：移植的 jemalloc 风格通用语义测试
- `tcmalloc_port`：移植的 tcmalloc 风格小对象/容器兼容测试

## 延迟测量（纳秒）

```bash
./build/malloc_new_latency_bench
```

输出 `p50/p99`（单位 ns）并同时输出 hugepage 统计。注意微基准对 CPU 频率、绑核、系统负载敏感。

## 详细文档

- 完整 API、实现方式、测试说明与和官方 jemalloc/TCMalloc 测试差异，请见：`docs/API_Implementation_and_Testing.md`。

## 验证“确实实现了大页分配内存池”建议

1. 运行 `malloc_new_latency_bench` 触发 slab refill。
2. 读取 `hugepage_stats`：若 `success>0` 则至少一次走到 `MAP_HUGETLB`。
3. 在目标机器上结合 `/proc/meminfo` 的 `HugePages_*` 与系统策略做交叉验证。
