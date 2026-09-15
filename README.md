# TCMalloc 内存池（C++ 实现）

仿 Google [TCMalloc](https://github.com/google/tcmalloc) 的**三级缓存**结构内存池：**ThreadCache + CentralCache + PageCache**。基于 Windows API（`VirtualAlloc`）实现，Visual Studio / MSVC 工程，代码注释完整，适合用来理解现代 `malloc` 的核心设计。

对外只暴露两个接口（见 `src/ConcurrentAlloc.h`）：

```cpp
void* ConcurrentAlloc(size_t size);   // 申请内存
void  ConcurrentFree(void* ptr);      // 释放内存
```

## 目录结构

```
.
├── tcmalloc1.sln                # Visual Studio 解决方案
├── tcmalloc1.vcxproj            # 工程文件
├── tcmalloc1.vcxproj.filters    # 工程文件过滤器
├── .gitignore
├── README.md
└── src/
    ├── Common.h                 # 常量、FreeList、SizeClass、Span/SpanList、SystemAlloc/SystemFree
    ├── ConcurrentAlloc.h        # 对外接口 ConcurrentAlloc / ConcurrentFree
    ├── ThreadCache.h/.cpp       # 线程缓存：每个线程一份，208 个自由链表桶，无锁
    ├── CentralCache.h/.cpp      # 中心缓存：全局单例，208 个 span 桶，每桶一把锁
    ├── PageCache.h/.cpp         # 页缓存：全局单例，1~128 页的 span 桶，负责向系统申请与页合并
    ├── PageMap.h                # 页号 → span 的映射表（单层数组 / 二级、三级基数树三种实现）
    ├── ObjectPool.h             # 定长内存池，为 ThreadCache / Span 等控制结构本身提供内存
    ├── UnitTest.cpp             # 功能测试场景（main 默认被注释）
    └── Benchmark.cpp            # 与 malloc/free 的性能对比（工程默认入口）
```

## 整体架构

```
        ConcurrentAlloc(size) / ConcurrentFree(ptr)          <- 对外接口
                        │
        ┌───────────────┴────────────────┐
   size <= 256KB                   size > 256KB
        │                                └──────────────────────┐
        ▼                                                       ▼
┌──────────────────┐   批量下发   ┌──────────────────┐   按页   ┌──────────────────┐
│   ThreadCache    │ ──────────► │  CentralCache    │ ──────► │    PageCache     │
│   线程私有(TLS)   │ ◄────────── │  全局单例         │ ◄────── │   全局单例        │
│  208 个 FreeList │   批量归还   │  208 个 SpanList │   回收   │ 129 个 SpanList  │
│   无锁           │             │  每桶一把桶锁      │         │ 一把页锁 + 页合并  │
└──────────────────┘             └──────────────────┘         └────────┬─────────┘
                                                                       │ 1~128 页
                                                                       ▼
                                                        SystemAlloc / SystemFree
                                                        (VirtualAlloc, 一页 = 8KB)
```

| 层级 | 作用域 | 职责 | 并发策略 |
| --- | --- | --- | --- |
| ThreadCache | 每线程一份 | 直接服务小对象的申请 / 释放 | 无锁（TLS） |
| CentralCache | 全局一份 | 在线程之间调度 span，批量下发 / 回收对象 | 每个桶一把锁 |
| PageCache | 全局一份 | 以页为单位管理内存，向系统申请 / 归还，合并相邻空闲页 | 一把大锁 |

## 核心设计

### 大小类映射

按申请大小分 5 个区间，不同区间用不同粒度对齐，共 **208 个桶**，把内碎片控制在 10% 量级（见 `src/Common.h` 的 `SizeClass`）：

| 申请大小 | 对齐粒度 | 对应桶下标 | 桶数 |
| --- | --- | --- | --- |
| `[1, 128]` | 8 B | `[0, 16)` | 16 |
| `[129, 1024]` | 16 B | `[16, 72)` | 56 |
| `[1025, 8K]` | 128 B | `[72, 128)` | 56 |
| `[8K+1, 64K]` | 1 KB | `[128, 184)` | 56 |
| `[64K+1, 256K]` | 8 KB | `[184, 208)` | 24 |

### 核心数据结构

- **FreeList**：切好的小对象自由链表（头插头删），额外记录 `_size`（当前长度）和 `_maxSize`（下次批量申请的个数，用于慢启动）。
- **Span**：管理一段连续页的内存块，记录 `_pageId`（起始页号）、`_n`（页数）、`_objSize`（切出来的小对象大小）、`_useCount`（已分配给线程缓存的对象数）、`_freeList`（未分配出去的对象链表）、`_isUse`（是否正在被使用）。
- **SpanList**：带哨兵的双向循环链表，每桶自带一把 `std::mutex`。
- **ObjectPool**：定长内存池，避免用 `new` 反复申请 `ThreadCache`、`Span` 这类控制结构（128KB 一批，切分 + 回收复用）。
- **PageMap**：页号到 `Span*` 的映射。文件里给了三种实现——单层数组 `TCMalloc_PageMap1`、二级基数树 `TCMalloc_PageMap2`、三级基数树 `TCMalloc_PageMap3`，本工程用的是**三级基数树**：中间节点和叶子都按需分配（所以 `set()` 会先 `Ensure()` 建好路径上的节点），`PAGE_MAP_BITS` 覆盖整个进程地址空间。

### 关键参数

| 参数 | 值 | 说明 |
| --- | --- | --- |
| `MAX_BYTES` | 256 KB | 小对象上限，超过则走大对象路径 |
| `NFREELIST` | 208 | 线程缓存 / 中心缓存的桶数 |
| `NPAGES` | 129 | 页缓存桶数（下标 1~128） |
| `PAGE_SHIFT` | 13 | 一页 8 KB |
| `PAGE_ID` | x64 下为 `unsigned long long` | 页号 |
| `PAGE_MAP_BITS` | x64 下 `47 - PAGE_SHIFT` = 34，Win32 下 `32 - PAGE_SHIFT` = 19 | 页号映射表需要覆盖的位数 |

## 关键流程

### 申请（size <= 256KB）

1. `ConcurrentAlloc` 判断 `size <= MAX_BYTES`，取当前线程的 TLS 缓存 `pTLSThreadCache`；为空则用 `ObjectPool` 造一个。
2. `ThreadCache::Allocate`：`RoundUp` 对齐后算出桶下标，桶里有就直接 `Pop` 返回——**全程无锁**。
3. 桶空则 `FetchFromCentralCache`：批量个数 `batchNum = min(MaxSize, NumMoveSize(size))`，并把 `MaxSize += 1`（慢启动，越常要就给得越多）。
4. `CentralCache::FetchRangeObj`：加桶锁，`GetOneSpan` 找一个还有空闲对象的 span。
5. 桶内没有可用 span 时：**先解掉桶锁**，再加页锁向 `PageCache::NewSpan(NumMovePage(size))` 要若干页，避免持锁等待。
6. `PageCache::NewSpan`：先看第 k 号桶 → 没有就在更大的桶里找 span 切一块出来 → 都没有则 `SystemAlloc(128)` 要 128 页挂到 128 号桶，再递归切分。
7. 拿到 span 后切成等长小对象串成自由链表，挂到桶头；摘出 `actualNum` 个，`_useCount += actualNum`。
8. 第 1 个返回给调用方，其余 `actualNum - 1` 个挂进线程缓存的桶里备用。

### 释放

1. `ConcurrentFree` → `PageCache::MapObjectToSpan`：用 `ptr >> PAGE_SHIFT` 得到页号，在映射表里反查所属 span，拿到 `_objSize`。
2. 小对象 → `ThreadCache::Deallocate`：挂回本线程对应的桶。
3. 桶长度 `Size >= MaxSize` 时触发 `ListTooLong`，摘一批对象批量还给中心缓存。
4. `CentralCache::ReleaseListToSpans`：加桶锁，逐个挂回 `span->_freeList` 并 `_useCount--`。
5. `_useCount == 0` 说明该 span 切出去的对象全部归还，从桶里摘除，**解掉桶锁**后交给 `PageCache::ReleaseSpanToPageCache`。
6. 页缓存尝试与前后相邻的空闲 span 合并（缓解外部碎片），再挂回对应页数的桶中复用。

### 大对象（size > 256KB）

不走线程缓存和中心缓存，直接向上取整到整页数，加页锁调用 `PageCache::NewSpan(k)`；`k > 128` 页时页缓存也管不了，直接 `SystemAlloc`。释放同理：`span->_n > 128` 直接 `SystemFree`，否则走合并回收。

### 一个真实的申请轨迹

第一次 `ConcurrentAlloc(6)`（经过上面第 8 步之后）：

```
[ThreadCache] Allocate(size=6): 向上取整=8 -> 桶 index=0, 桶内 Size=0 MaxSize=1
    [ThreadCache] 本地桶空了 -> 向 CentralCache 批量要: index=0 batchNum=1 (MaxSize 已自增到 2)
    [CentralCache] 桶里所有 span 都空了 -> 先解开桶锁, 再向 PageCache 要 1 页
    [PageCache] 128 号桶也空了 -> 一口气向系统要 128 页 = 1048576 字节, 挂到 128 号桶, 再递归切分
    [PageCache] 第 128 号桶里有 128 页的大 span(pageId=5648): 头部切 1 页给请求方, 剩下 127 页挂回第 127 号桶
    [CentralCache] 拿到 span(pageId=5648, 1 页=8192 字节), 切成 1024 个 8 字节小对象并串成自由链表
    [CentralCache] 摘出 1 个对象给 ThreadCache, span->_useCount: 0 -> 1
```

## 加锁顺序

- `ThreadCache` 完全无锁（线程私有，TLS）。
- `CentralCache` 每个桶一把锁；`PageCache` 一把全局大锁。
- 统一规则：**持桶锁 → 先释放桶锁 → 再获取页锁**，因此不会出现「持有桶锁等待页锁」的死锁场景。

## 编译与运行

### Visual Studio 2022（推荐）

1. 用 VS 2022 打开 `tcmalloc1.sln`（工具集 `v143`）。
2. 平台选 `x64` 或 `x86` 都能跑，直接 F5 运行，默认执行 `src/Benchmark.cpp` 里的 `main`，它会跑两组性能对比。
3. 想看功能测试：把 `src/Benchmark.cpp` 的 `main` 注释掉，取消 `src/UnitTest.cpp` 末尾 `main` 的注释（`UnitTest.cpp:139`）。

### 非 MSVC 编译器（MinGW / g++，可选）

核心逻辑与编译器无关，但源码里有几处 MSVC 特有写法，用 g++ / clang 编译需要小改：

```bash
g++ -std=c++17 -O2 src/Benchmark.cpp src/ThreadCache.cpp \
    src/CentralCache.cpp src/PageCache.cpp -o tcmalloc1.exe
```

- `src/ThreadCache.h`：`_declspec(thread)` → `thread_local`（`_declspec` 是 MSVC 拼写，GCC/Clang 下会被忽略，导致多线程共享同一个缓存而崩溃）。
- `src/ThreadCache.cpp`：`min(...)` → `std::min(...)`（MSVC 下 `min` 来自 `windows.h` 的宏）。

另外 `SystemAlloc` / `SystemFree` 基于 `VirtualAlloc` / `VirtualFree` 实现，换到 Linux 需要替换为 `brk` / `mmap`。

## 测试场景

`src/UnitTest.cpp` 里的测试函数，配合断点可以观察整个三级缓存的流转：

| 测试函数 | 验证内容 |
| --- | --- |
| `TLSTest` | 两个线程分别申请，验证每个线程持有独立的 ThreadCache |
| `TestConcurrentAlloc1` | 连续申请 6/8/1/7/8 字节再依次释放，观察 span 切分与页合并 |
| `TestConcurrentAlloc2` | 1024 次 8 字节申请刚好切完一页，第 1025 次触发向页缓存要新 span |
| `TestAddressShift` | 打印地址与页号的移位换算关系 |
| `TestMultiThread` | 两个线程并发申请 / 释放 |
| `BigAlloc` | 257KB（页缓存切分路径）与 1056KB（直接向系统申请路径） |

## 性能参考

工程默认入口 `src/Benchmark.cpp` 会对比 4 个线程、10 轮、每轮 10 万次 `malloc/free` 与 `ConcurrentAlloc/ConcurrentFree` 的耗时（共计 400 万次申请 + 400 万次释放）。本机 VS 2022 `Release|x64` 跑 3 轮的合计耗时（波动较大，仅供参考）：

| 实现 | 3 轮合计耗时 | 平均 |
| --- | --- | --- |
| `ConcurrentAlloc / Free` | 209 / 259 / 294 ms | **254 ms** |
| `malloc / free` | 420 / 467 / 494 ms | 460 ms |

`Debug` 配置下两者都慢一个数量级（实测内存池约 1.5 s、`malloc` 约 7.6 s），内存池的领先幅度更明显。

## 已知限制

- 仅支持 Windows：`SystemAlloc/SystemFree` 依赖 `VirtualAlloc/VirtualFree`。
- 暂未实现 tcmalloc 的后台回收线程（scavenger），空闲页不会被主动归还系统。
- 未提供 `aligned_alloc`、`realloc` 等接口，仅支持固定大小的小对象与整页大对象。
- 大小类硬编码为 208 个桶，未做成可配置。

## 参考

- [Google TCMalloc](https://github.com/google/tcmalloc)
- [TCMalloc: Thread-Caching Malloc](https://google.github.io/tcmalloc/design.html)

> 本项目为学习性质的实现，欢迎提出 issue 交流。

## 许可证

仓库暂未添加 LICENSE 文件，如需开源请自行补充（例如 MIT）。
