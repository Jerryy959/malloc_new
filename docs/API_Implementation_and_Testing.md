# malloc_new 文档：API、实现方式、测试体系与特性说明

本文档基于仓库当前 `main`（已合并）代码，系统说明：

1. 对外 API（C / C++ / compat）的能力与语义。
2. 内部实现（size class、大页 refill、回退路径、元数据管理）。
3. 测试代码覆盖点与目标。
4. 与官方 jemalloc / TCMalloc 测试代码的差异。
5. 本仓库的关键特性与边界。

---

## 1. 仓库特性（先看结论）

`malloc_new` 的定位是：**普通大页（hugetlb）优先** 的内存池原型，用于交易类低抖动场景的实验与工程化落地，而不是 jemalloc/TCMalloc 的“全量替代实现”。

核心特性：

- **普通大页优先**：小对象池 refill 时优先 `MAP_HUGETLB | MAP_HUGE_2MB`；失败后回退匿名 `mmap + madvise(MADV_HUGEPAGE)`。  
- **固定 size class + freelist**：小对象按 class 分配，降低每次系统调用开销。  
- **C/C++ 双 API**：提供 `hp_*` C 接口、STL Allocator、对象构造/析构包装。  
- **兼容适配层**：`malloc_new::compat::*` 提供常见 `malloc/realloc/free` 风格调用入口。  
- **可观测性**：通过 `hp_get_allocator_stats()` 获取 hugetlb 尝试/成功/回退计数。  
- **纳秒基准路径**：内置 `malloc_new_latency_bench` 输出 p50/p99（ns）。

---

## 2. API 说明

### 2.1 C 风格接口（`extern "C"`）

头文件：`include/malloc_new/allocator.hpp`

- `void* hp_malloc(std::size_t size) noexcept`
  - `size==0` 时内部按 1 字节处理。
  - 优先走小对象池；不满足 class 时走大对象分配路径。
- `void* hp_aligned_alloc(std::size_t alignment, std::size_t size) noexcept`
  - 要求 `alignment` 是 2 的幂，否则返回 `nullptr` 并置 `errno=EINVAL`。
  - `size==0` 时按 `alignment` 处理。
- `void* hp_calloc(std::size_t count, std::size_t size) noexcept`
  - 做乘法溢出检查；成功后清零。
- `void* hp_realloc(void* ptr, std::size_t size) noexcept`
  - `ptr==nullptr` 等价 `hp_malloc(size)`。
  - `size==0` 时释放并返回 `nullptr`。
  - 对已识别的本分配器块做“扩容拷贝”逻辑。
- `void hp_free(void* ptr) noexcept`
  - 支持空指针。
  - 若能识别到本分配器 header，按池或大对象路径释放；否则走 `std::free`。

另有统计接口：

- `hp_allocator_stats hp_get_allocator_stats() noexcept`
  - `hugepage_refill_attempts`：尝试 hugetlb refill 次数。
  - `hugepage_refill_success`：hugetlb refill 成功次数。
  - `fallback_refill_success`：回退 mmap 成功次数。

---

### 2.2 C++ 标准库 Allocator

类型：`malloc_new::HugePageAllocator<T>`

已提供：

- `value_type / size_type / difference_type`
- `rebind<U>::other`
- `propagate_on_container_move_assignment`
- `is_always_equal`
- `allocate(n)` / `deallocate(p, n)`
- 跨类型构造、`==`/`!=`

这使其可作为 `std::vector<T, HugePageAllocator<T>>` 等容器的 allocator 参数使用。

---

### 2.3 C++ 对象包装

- `malloc_new::hp_new<T>(Args&&...)`
  - 从 `hp_aligned_alloc(alignof(T), sizeof(T))` 取内存并 placement new。
- `malloc_new::hp_delete(T*)`
  - 手动析构并 `hp_free`。

---

### 2.4 compat 适配层

命名空间：`malloc_new::compat`

- `malloc / aligned_alloc / calloc / realloc / free`
  - 纯转发到 `hp_*`。
- `new_object<T>(...) / delete_object(T*)`
  - 转发到 `hp_new / hp_delete`。

设计目的：方便“期望标准形态函数”的 demo/适配测试调用，不直接覆盖进程全局 `::malloc` 符号。

---

## 3. 实现方式说明

核心文件：`src/huge_page_allocator.cpp`

### 3.1 小对象路径：size class + freelist

- 预定义 class：`16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 1024, 2048`。
- 每个 class 一个 `SizeClassPool`：
  - `std::mutex mu`
  - `FreeNode* free_list`
  - `block_size` 与初始化标志
- 分配时根据 header+payload+alignment 计算 `total`，选择可容纳的最小 class。

### 3.2 refill：hugetlb 优先

当 class freelist 为空时：

1. `mmap(..., MAP_HUGETLB | MAP_HUGE_2MB)` 尝试普通大页。
2. 失败则 fallback：`mmap(MAP_ANONYMOUS)` + `madvise(MADV_HUGEPAGE)`。
3. 把 2MB slab 切成 `block_size` 的节点压入 freelist。

同时更新统计计数，供外部验证行为。

### 3.3 元数据与合法性识别

- 每个分配块写入 `AllocationHeader`，包含 magic、class、flags、requested_size、original。
- 通过 magic 判断是否为本分配器块。
- 小对象 `flags==1`：释放时回收到对应池。
- 大对象 `flags==0`：释放 `header->original`。

### 3.4 大对象路径

不适配 class 时走 `alloc_large`：

- `std::malloc(size + header + alignment)`
- 手动做对齐与 header 回填
- 支持可选清零

### 3.5 realloc 语义

- 识别到本分配器块时：
  - 若新大小 <= 旧大小：原地更新 `requested_size`。
  - 若扩容：重新分配、拷贝旧数据、释放旧块。
- 未识别块：回退 `std::realloc`。

---

## 4. 测试代码说明

### 4.1 `tests/smoke_tests.cpp`

目标：基础 API 能跑通。

覆盖：

- `hp_malloc/hp_free`
- `hp_aligned_alloc`
- `hp_calloc`
- `hp_realloc`
- `HugePageAllocator` 用于 `std::vector`
- `hp_new/hp_delete`

### 4.2 `tests/integrity_tests.cpp`

目标：更高强度的正确性/稳健性。

覆盖：

- 多对齐-多尺寸矩阵校验
- realloc 扩容/缩容后数据完整性
- 多线程压力分配释放

### 4.3 `tests/jemalloc_port_tests.cpp`

目标：移植“通用 malloc 语义”层面的 case（而不是 jemalloc 私有能力）。

覆盖：

- `malloc(0)` 可分配
- 非法对齐返回 `EINVAL`
- `calloc` 清零
- `realloc(nullptr, n)`
- `realloc(p, 0)`

### 4.4 `tests/tcmalloc_port_tests.cpp`

目标：移植“高频小对象/容器兼容”风格 case。

覆盖：

- 大量小对象分配释放
- `HugePageAllocator` 在容器扩容下可用
- `compat` API 与 `new_object/delete_object`

### 4.5 `tests/latency_bench.cpp`

目标：提供纳秒级指标采样路径。

行为：

- 对 `hp_malloc + hp_free` 单次调用配对计时
- 统计 p50 / p99（ns）
- 同时打印 `hp_get_allocator_stats()`，关联是否走 hugetlb/fallback

---

## 5. 与官方 jemalloc / TCMalloc 测试代码的不同

这是非常关键的一点：当前仓库是“**可移植语义子集 + 原型验证**”，不是完整替代，因此测试策略与官方项目有显著差异。

### 5.1 覆盖范围差异

官方 jemalloc / TCMalloc 测试通常包含：

- 私有控制接口（如 mallctl/统计树）
- arena / tcache / central freelist / span/pageheap 等内部策略行为
- profile / sampling / background thread / decay / scavenger
- 大量平台特定与回归 case

本仓库当前测试只覆盖：

- 标准分配语义基础子集
- 本实现支持的 size-class 池逻辑
- 对齐、realloc、并发基本正确性
- 简化版性能采样路径

### 5.2 接口层差异

官方测试大多围绕其自身导出的全局或私有 API。

本仓库主要导出 `hp_*` 和 `malloc_new::compat::*`，没有实现官方完整私有接口；因此“移植测试”必须筛选为**与实现无关的通用语义部分**。

### 5.3 性能测试方法差异

官方基准通常有更完整的：

- CPU 绑核、频率固定、NUMA 控制
- 多维对象分布/线程模型
- 长时间稳态采样

本仓库内置 `latency_bench` 是轻量微基准，用于快速检查 p50/p99 与 hugepage 路径状态，不等价于官方全套性能评测。

### 5.4 结论

当前“jemalloc/tcmalloc port tests 通过”应理解为：

- **通过了移植后的通用语义子集测试**，
- **不代表通过官方完整测试矩阵**。

---

## 6. 如何验证“确实实现了大页分配内存池”

推荐步骤：

1. 运行 `malloc_new_latency_bench` 或高频小对象测试，触发多次 refill。
2. 读取 `hp_get_allocator_stats()` 输出：
   - `attempts > 0` 表示发生了 refill 尝试；
   - `success > 0` 表示至少一次命中 hugetlb；
   - `fallback > 0` 表示发生了回退。
3. 在目标环境结合系统状态验证：
   - `/proc/meminfo` 的 `HugePages_Total/Free/Rsvd/Surp`
   - THP 配置是否关闭
   - 内核与容器权限是否允许 `MAP_HUGETLB`

> 说明：如果环境未配置 hugetlb 池，`success` 可能为 0，但这不表示逻辑不存在，而是运行条件不满足。

---

## 7. 当前边界与后续建议

### 7.1 当前边界

- 还不是 jemalloc/TCMalloc 的功能等价替代。
- 大对象路径仍是 `malloc` 风格分配，不是完整 hugepage 策略。
- 统计接口是轻量计数，不是完整可观测平台。

### 7.2 后续建议

- 增加更贴近生产的 benchmark harness（绑核、预热、稳态窗口、CSV 导出）。
- 扩展语义测试矩阵（更多 `realloc` 边界、错误注入、长期并发）。
- 引入可选全局符号接管模式（`LD_PRELOAD`/linker wrap）以接更多第三方通用测试。
- 把大对象路径也逐步纳入普通大页策略（按阈值/分层）。

---

## 8. 文档对应源码索引

- API 声明：`include/malloc_new/allocator.hpp`
- 核心实现：`src/huge_page_allocator.cpp`
- compat 实现：`src/compat.cpp`
- 示例：`examples/no_cmake_basic_demo.cpp`
- 测试：`tests/*.cpp`
- 构建：`CMakeLists.txt`

