# ThreadPool

基于 pthread 的 C11 线程池，生产者-消费者模型，带管理线程动态扩缩容。

## 目录

- [特性](#特性)
- [快速开始](#快速开始)
- [使用示例](#使用示例)
- [API](#api)
- [设计](#设计)
- [压力测试](#压力测试)
- [项目结构](#项目结构)
- [变更记录](#变更记录)

## 特性

- **环形任务队列**：容量固定，队列满时生产者阻塞（或选择非阻塞提交）
- **动态扩缩容**：管理线程每 2 秒检查负载，每次 ±2 个线程（`ADJUST_STEP`）
- **优雅销毁**：停止接收新任务 → 执行完队列剩余任务 → 回收全部线程 → 释放资源
- **优雅停收**（`threadPoolShutdown`）：拒绝新任务，已提交任务继续执行完
- **非阻塞提交**（`threadPoolTryAdd`）：队列满立即返回 `EBUSY`，不等待
- **同步屏障**（`threadPoolWaitAll`）：阻塞直到所有已提交任务执行完毕
- **线程安全**：除 `threadPoolDestroy` 外所有 API 均可从任意线程调用
- **低锁竞争**：`busyNum` 用原子计数，worker 每个任务只取一次池锁
- **可移植**：Linux（含线程命名）/ macOS / Windows（MinGW/MSYS2），C11

## 快速开始

### Linux / macOS

```bash
cmake -B build
cmake --build build
./build/threadpool          # 演示程序
./build/stress              # 压力测试
ctest --test-dir build      # 或通过 ctest 跑压测
```

### Windows（MSYS2 UCRT64）

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
        mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-gdb
cmake -B build -G Ninja
cmake --build build
./build/threadpool
./build/stress
```

> 本项目的 `prctl` 线程命名等 Linux 专属调用已做 `__linux__` 保护，
> 在 macOS 上用 `pthread_setname_np`，在 Windows 上自动跳过，可直接编译。

带消毒器构建（推荐开发时用，Linux 下效果最佳）：

```bash
cmake -B build-san -DTP_SANITIZE=ON
cmake --build build-san
./build-san/stress
```

## 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include "threadpool.h"

/* 任务函数：参数由任务函数自己释放（线程池不负责释放 arg） */
static void job(void* arg)
{
    int n = *(int*)arg;
    free(arg);
    printf("job %d done\n", n);
}

int main(void)
{
    /* 初始 2 个线程，最多 8 个，任务队列容量 64 */
    ThreadPool* pool = threadPoolCreate(2, 8, 64);
    if (pool == NULL)
        return 1;

    for (int i = 0; i < 1000; ++i) {
        int* n = malloc(sizeof(int));
        *n = i;
        threadPoolAdd(pool, job, n);        /* 阻塞式提交 */
    }

    threadPoolWaitAll(pool);                /* 等所有任务执行完 */
    printf("pending=%d\n", threadPoolPendingCount(pool));

    threadPoolShutdown(pool);               /* 优雅停收：拒绝新任务 */
    threadPoolDestroy(pool);                /* 销毁（再次调用返回 -1） */
    return 0;
}
```

## API

| 函数 | 说明 |
|------|------|
| `threadPoolCreate(min, max, queueSize)` | 创建线程池；参数非法/资源不足返回 `NULL` |
| `threadPoolDestroy(pool)` | 销毁（优雅排空后回收）；重复调用返回 `-1` |
| `threadPoolShutdown(pool)` | 优雅停收：拒绝新任务，存量任务跑完 |
| `threadPoolAdd(pool, func, arg)` | 阻塞式提交；队列满时等待 |
| `threadPoolTryAdd(pool, func, arg)` | 非阻塞提交；满队列返回 `-1`/`EBUSY` |
| `threadPoolWaitAll(pool)` | 阻塞直到所有任务执行完毕 |
| `threadPoolBusyNum(pool)` | 忙线程数 |
| `threadPoolAliveNum(pool)` | 存活线程数 |
| `threadPoolPendingCount(pool)` | 待处理任务数（队列中 + 运行中） |

约定：

- `arg` 指向的内存由任务函数自行管理，线程池不会释放它
- `threadPoolDestroy` 返回后不得再调用任何 API
- `threadPoolTryAdd` 失败时 `errno` 区分原因：`EBUSY`（队列满）、
  `EPERM`（已关闭）、`EINVAL`（参数非法）

## 设计

```
                        +-------------------+
     threadPoolAdd ---> |  环形任务队列       | <--- worker 线程池
     (生产者，可阻塞)     |  (capacity 个槽位)  |      (min ~ max 个)
                        +-------------------+
                              ^   ^
                        notFull  notEmpty  (条件变量)
                        +-------------------+
                        |  管理线程 (manager) | ---> 每 2s 检查负载，
                        +-------------------+      扩/缩容（±2 线程）
```

- `mutexpool` 保护池状态（队列、线程数、pending、closed/shutdown）；
  `busyNum` 用 `atomic_int` 原子计数，任务执行期间不阻塞其他消费者取任务
- `pending` 会计计数 = 队列中 + 运行中，归零时广播 `allDone` 唤醒 `WaitAll`
- `producerCount` 会计计数：`Destroy` 会等待所有正在 `Add/TryAdd` 的
  调用者离开后才释放内存，杜绝 use-after-free
- 管理线程定时等待使用 `CLOCK_MONOTONIC`（Linux），
  避免系统时间调整导致调度抖动
- worker 线程命名（Linux `prctl` / macOS `pthread_setname_np`），
  便于 `top`/`gdb` 中区分 `tp-worker` 与 `tp-manager`
- worker 持有自身槽位指针（`threadIDs + i`），退出时 O(1) 置零，
  无需扫描线程数组
- 缩容标记 `exitNum` 并入 worker 等待条件：空闲线程被标记后自行退出，
  不依赖外部唤醒，避免信号丢失导致缩容失效

## 压力测试

`stress.c` 覆盖 8 个场景：

| 场景 | 验证点 |
|------|--------|
| S1 | 批量任务 + 提前销毁（优雅排空） |
| S2 | 小池大负载（1~3 线程扛 500 任务，扩容极限） |
| S3 | 提交后立即销毁（任务执行中销毁） |
| S4 | 100 次创建/销毁循环（资源泄漏检测） |
| S5 | 生产者并发 Add 与 Destroy（关闭竞态 / UAF） |
| S6 | 4 生产者 + WaitAll（任务数精确校验，不多不少） |
| S7 | TryAdd 溢出（恰 4 个接受、6 个 EBUSY、全部执行） |
| S8 | Shutdown 停收（拒绝新任务 + 存量排空） |

## 项目结构

```
ThreadPool/
├── threadpool.h     # 公开 API 声明与文档
├── threadpool.c     # 核心实现
├── main.c           # 演示程序（4 个场景）
├── stress.c         # 压力测试（8 个场景，ctest 已接入）
├── CMakeLists.txt   # CMake 构建（Threads::Threads、GCC/Clang 专属警告、TP_SANITIZE）
└── README.md
```

## 变更记录

### v2.1（本次优化）
- **修复缩容失效缺陷**：`exitNum` 并入 worker 等待条件，空闲线程被标记
  缩容后主动退出，不再依赖外部 `signal` 唤醒（原实现在广播被新任务打断时，
  线程会带着 `exitNum` 永久阻塞，线程数不减少）
- **低锁竞争**：`busyNum` 改用 `atomic_int` 原子计数，删除独立锁
  `mutexbusy`，worker 每个任务从 4 次锁操作降到 3 次
- **threadExit O(1)**：worker 创建时携带自身槽位指针，退出时直接置零，
  不再 O(maxNum) 扫描线程数组
- **消除 OOM 回退 UB**：`Destroy` 的 join 快照改用定长栈数组，
  删除 malloc 失败时无锁读 `threadIDs` 的回退路径（C11 UB）
- **修复 macOS 编译**：`pthread_condattr_setclock` 为 Linux 专属，
  以 `__linux__` 保护（此前在 macOS 上编译失败）
- **工程化**：CMake `-Wall -Wextra` 以生成器表达式限定 GCC/Clang，
  兼容 MSVC

### v2（本次重写）
- **修复 UAF 竞态**：`Destroy` 现在等待所有正在阻塞提交的生产者离开
  后才释放内存（原实现可能因生产者唤醒延迟而访问已释放的池）
- **修复 join 数据竞争**：`Destroy` 在锁内快照 worker 槽位再 join，
  消除了与 `threadExit` 置零操作的无锁读写竞争（C11 UB）
- **修复时钟抖动**：管理线程定时改用 `CLOCK_MONOTONIC`
- **修复广播丢失**：manager 进入等待前先检查关闭标志，
  避免 destroy 时 join 最多多等一个检测周期（2 秒）
- **修复初始化泄漏**：创建路径分步初始化、失败精确回滚
- **新增 API**：`threadPoolShutdown` / `threadPoolTryAdd` /
  `threadPoolWaitAll` / `threadPoolPendingCount`
- **可移植性**：`prctl` 以 `__linux__` 保护（macOS 用
  `pthread_setname_np`），线程池可在 macOS/MinGW 编译
- **工程化**：stress 接入 CMake + ctest；`-Wall -Wextra`；
  可选 ASan/UBSan 构建；新增 README

### v1（原始版）
- 环形任务队列 + 生产者-消费者模型
- 管理线程动态扩缩容（±2）
- 优雅销毁 4 步流程
- 兼容宏 `threadPoolDestory`
