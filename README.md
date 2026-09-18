# TaskHandler

**中文** | [English](README.en.md)

面向 C++23 的单 worker 任务队列，以及一个允许并发执行的兄弟类型线程池。

每个 handler 恰好拥有一条线程，提交给它的任务都在这条线程上逐个执行。这正是它适合做事件处理的原因：handler 持有的状态不需要加锁，因为只有它的 worker 会碰到这些状态。可以提交后不管、提交后等待，也可以提交后拿走一个 future。

可以重叠的 CPU 工作应放到 `ThreadPool` 上，而不是在 handler 里再开一条 worker。结果若必须碰到 handler 持有的状态，再弹回 handler。

同一份源码既可当 header-only 用，也可编译成库。

```cpp
#include "conan/task_handler.h"

conan::TaskHandler handler;

// Fire and forget.
handler.add_callable([] { std::cout << "on the worker thread\n"; });

// Wait for it.
handler.add_callable<conan::Blocked>([] { std::cout << "done before we move on\n"; });

// Take the result.
std::future<int> answer = handler.add_callable<conan::Future>([] { return 42; });
std::cout << answer.get() << '\n';
```

## 目录

- [安装](#安装)
- [提交任务](#提交任务)
- [优先级](#优先级)
- [延时任务与取消](#延时任务与取消)
- [错误](#错误)
- [背压](#背压)
- [共享 handler](#共享-handler)
- [线程池](#线程池)
- [生命周期](#生命周期)
- [选项](#选项)
- [版本](#版本)
- [构建](#构建)
- [保证与限制](#保证与限制)
- [参与贡献](#参与贡献)
- [许可证](#许可证)

## 安装

### 仅头文件

把 `include/conan/` 拷进工程并包含头文件即可。消费方必须按 C++23 编译。除此之外没有别的依赖，但仍需链接线程库：

```cmake
find_package(Threads REQUIRED)
target_include_directories(my_app PRIVATE third_party/TaskHandler/include)
target_link_libraries(my_app PRIVATE Threads::Threads)
```

### 作为子目录

```cmake
add_subdirectory(third_party/TaskHandler)
target_link_libraries(my_app PRIVATE TaskHandler::header_only)   # or ::task_handler
```

### 用 FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(TaskHandler
    GIT_REPOSITORY https://github.com/conansnow/TaskHandler.git
    GIT_TAG v0.3.0
    )
FetchContent_MakeAvailable(TaskHandler)
target_link_libraries(my_app PRIVATE TaskHandler::task_handler)
```

测试、示例、benchmark 和安装规则只在 TaskHandler 作为顶层工程时默认打开，所以消费方只会编到库本身。

### 作为已安装的包

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

```cmake
find_package(TaskHandler 0.3 REQUIRED)
target_link_libraries(my_app PRIVATE TaskHandler::task_handler)
```

导出两个 target：

| Target | 作用 |
| --- | --- |
| `TaskHandler::header_only` | Interface target。不用编、不用发二进制。 |
| `TaskHandler::task_handler` | 编译后的库。消费方编译更快，只需发一份共享对象。 |

`TaskHandler::task_handler` 会自行向下传播 `TASKHANDLER_COMPILED_LIB`（共享库还会带上 `TASKHANDLER_SHARED_LIB`），不必手写宏。每个二进制选一个 target，不要混用。

## 提交任务

`add_callable` 的第一个模板参数是策略标签，默认是 `Queued`。

```cpp
// Queued: returns immediately, gives back a TaskId you can cancel with.
conan::TaskId id = handler.add_callable([] { work(); });

// Blocked: returns once the task has run, rethrowing whatever it threw.
handler.add_callable<conan::Blocked>([&] { state = compute(); });

// Future: returns std::future<R> for the task's own R, move-only results
// included.
std::future<std::unique_ptr<Reply>> reply =
    handler.add_callable<conan::Future>([] { return fetch(); });
```

可调用对象不必能拷贝，捕获 `unique_ptr` 没问题：

```cpp
handler.add_callable([data = std::move(data)] { consume(*data); });
```

`Blocked` 和 `Future` 若已经在该 handler 自己的 worker 上被调用，会就地执行而不是入队，避免自己等自己。递归使用因此是安全的：

```cpp
handler.add_callable<conan::Blocked>([&] {
  // Already on the worker, so this runs inline rather than queueing behind
  // a task that can never start.
  handler.add_callable<conan::Blocked>([&] { nested(); });
});
```

## 优先级

每次提交都可以带一个可选优先级。**数值越大越先跑。** 同优先级按提交顺序。

```cpp
handler.add_callable([] { normal(); });            // priority 0
handler.add_callable([] { urgent(); }, 10);        // jumps the queue
handler.add_callable([] { whenever(); }, -10);     // sinks to the bottom
```

优先级只决定 worker 接下来取哪一个，不会打断已经在跑的任务。

> 0.2.0 之前比较方向相反，*更小* 的值先跑。如果给旧版本传过非零优先级，把符号反过来。

## 延时任务与取消

```cpp
using namespace std::chrono_literals;

conan::TaskId id = handler.add_callable_after(5s, [] { retry(); });
handler.add_callable_at(deadline, [] { give_up(); });

if (too_late)
  handler.cancel(id);   // true if the task had not started yet

std::future<Reply> reply =
    handler.add_callable_after<conan::Future>(5s, [] { return fetch(); });
```

延时任务在截止时刻变成可运行，之后再按优先级排队，所以繁忙的 handler 可能比预定时间更晚才跑到它。绝不会更早跑。延时任务即使在 worker 上也绝不就地执行：若在某个任务里对这个 future 做 `get()`，会等到一项要等当前任务返回才能开始的工作。

`cancel` 对已经跑完、正在跑、或已经取消过的任务返回 `false`。之后 `TaskId` 仍然 `valid()`：它命名的是一次提交，并不表示任务还在排队。延时 `Future` 返回的是 future 而不是 id，因此靠 `stop()`（或析构）丢弃，future 会变成 broken。

## 错误

`Blocked` 任务抛出的异常会从 `add_callable` 重新抛出。`Future` 任务的异常存在 future 里，由 `get()` 重新抛出。

`Queued` 任务没有地方报告失败，默认把异常丢掉。要看见它们，装一个 hook：

```cpp
conan::TaskHandlerOptions options;
options.on_exception = [](std::exception_ptr error) {
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &caught) {
    LOG_ERROR("task failed: {}", caught.what());
  }
};
conan::TaskHandler handler{std::move(options)};
```

hook 在 worker 线程上运行，而且只对 `Queued` 任务触发。

提交被拒绝时会抛异常。两种拒绝共享一个基类，只关心「没接住」的调用方可以只 catch 一种类型。

| 异常 | 何时抛出 |
| --- | --- |
| `conan::TaskHandlerError` | 两者的基类；同时也是 `std::runtime_error` |
| `conan::TaskHandlerStopped` | handler 已经 stop |
| `conan::TaskHandlerQueueFull` | 已有 `max_pending` 个任务在等 |
| `conan::ThreadPoolError` | 线程池拒绝异常的基类 |
| `conan::ThreadPoolStopped` | 线程池已经 stop |
| `conan::ThreadPoolQueueFull` | 线程池已持有 `max_pending` 个任务 |

## 背压

队列默认无界。当生产者可能跑得比 handler 快时，给它设上限；达到上限后提交被拒绝，而不是把队列撑到把进程内存吃光：

```cpp
conan::TaskHandlerOptions options;
options.max_pending = 1024;
conan::TaskHandler handler{std::move(options)};

try {
  handler.add_callable([] { work(); });
} catch (const conan::TaskHandlerQueueFull &) {
  drop_or_retry_later();
}
```

上限把可运行的和尚未到期的任务算在一起，不含正在跑的那一个。来自 worker 的递归 `Blocked` / `Future` 就地执行、不入队，所以上限不会拒绝它们。

故意选择抛异常而不是阻塞提交：阻塞提交会把一条不相干的线程无限期停住，两个 handler 之间这就等着死锁。

## 共享 handler

不构造也能用到三个进程级 handler，第一次使用时创建。

```cpp
conan::TaskHandler::instance().add_callable([] { work(); });      // index 0
conan::TaskHandler::instance<1>().add_callable([] { other(); });  // index 1
```

`instance(index)` 接受运行期下标，超过 `instance_count()` 抛 `std::out_of_range`。`init()` 提前启动全部三个；`uninit()` 排空并停掉它们。两者都可选、都幂等，进程退出时也会自动关闭。

返回的引用在程序剩余生命周期内一直有效，包括跨越 `uninit()`，因此不会悬空。`uninit()` 之后 handler 只是停掉了，提交会抛异常，直到再次 `init()`。

需要不同数量的 worker，或一个自己拥有的？自己构造：

```cpp
conan::TaskHandler render_thread;
conan::TaskHandler io_thread;
```

## 线程池

`ThreadPool` 是允许并发执行的那一类工作的类型。`TaskHandler` 保持一条 worker：这正是它存在的意义。包含 `conan/thread_pool.h` 并构造一个自己拥有的池。没有进程级的池。

```cpp
#include "conan/thread_pool.h"

conan::ThreadPoolOptions options;
options.thread_count = 4;                 // 0 means max(1, hardware_concurrency())
options.thread_name_prefix = "crunch";    // workers are crunch-0, crunch-1, ...
conan::ThreadPool pool{std::move(options)};

pool.add_callable([] { crunch(); });
pool.add_callable<conan::Blocked>([&] { value = crunch(); });
std::future<int> answer = pool.add_callable<conan::Future>([] { return crunch(); });
```

策略标签与 handler 相同。`Queued` 返回 `void`：池不取消已入队的工作，所以没有 `TaskId`。延时工作留在 handler 上；该由一条线程去睡在截止时刻上。worker 共享一条 FIFO 队列：单个 worker 按提交顺序跑已入队的任务；不同 worker 上的任务可以重叠。

结果必须碰到 handler 持有的状态时，弹回 handler：

```cpp
conan::TaskHandler handler;
conan::ThreadPool pool;

pool.add_callable([&handler] {
  const int result = crunch();
  handler.add_callable([result] { apply(result); });
});
```

```cpp
std::size_t ThreadPool::pending() const;  // accepted, not yet started
void ThreadPool::flush();                 // wait until the workers are idle
bool ThreadPool::is_worker_thread() const;
bool ThreadPool::running() const;
std::size_t ThreadPool::thread_count() const noexcept;
void ThreadPool::start();                 // idempotent
void ThreadPool::stop();                  // drain, stop, join; idempotent
```

`stop()` 会排空已经接受的工作。`flush()` 等到目前已提交的任务都跑完、worker 空闲；从 worker 里调用会立即返回，因为在那里等只会死锁。任务内部的 `start()` / `stop()` 与 handler 相同：`stop()` 只记下请求，`start()` 什么也不做，因为 worker 不能 join 自己。

`max_pending` 以 `ThreadPoolQueueFull` 拒绝，而不是阻塞。来自 worker 的递归 `Blocked` 和 `Future` 就地执行，这样 1 线程的池不会自己等自己。如果每个 worker 都阻塞在等待更多池内工作上，池仍然会死锁：固定大小的池都有这个风险。

`on_exception` 可能被多个 worker 同时调用。hook 必须对此安全，否则由调用方自己同步；库不会替你串行化。

## 生命周期

构造 handler 会启动它的 worker。销毁时先跑完已经入队的工作，再 join。

```cpp
void TaskHandler::start();   // idempotent
void TaskHandler::stop();    // drain, stop, join; idempotent
bool TaskHandler::running() const;
```

`stop()` 会跑已经接受的工作而不是丢掉，这样 `Blocked` 的调用方不会一直等一个不会再跑的任务。尚未到期的延时任务会被丢弃，之后 `start()` 可以把 handler 拉回来。

从 handler 自己的任务里调用 `stop()` 只记下请求然后返回，因为线程不能 join 自己。worker 排空队列后自行退出。从任务里调用 `start()` 什么也不做：这条 worker 按定义正在跑，已经发出的 stop 请求只能由别的线程收回。

```cpp
std::size_t TaskHandler::pending() const;   // accepted, not yet started
void TaskHandler::flush();                  // wait for runnable work only
bool TaskHandler::is_current_thread() const;
```

`flush()` 只等可运行的工作，不等仍在等截止时刻的延时任务。从 worker 线程调用会立即返回，因为在那里等只会死锁。

## 选项

```cpp
struct TaskHandlerOptions {
  std::string thread_name;                             // shown in debuggers
  std::function<void(std::exception_ptr)> on_exception; // Queued failures
  std::size_t max_pending;                             // 0 means unbounded
};
```

`thread_name` 在 Linux、macOS 和 Windows 上都会生效。Linux 会截到 15 个字符。共享 handler 把自己命名为 `conan-task-0` 到 `conan-task-2`。池的 worker 用同样的方式使用 `thread_name_prefix`，形如 `{prefix}-{index}`。

`thread_name` 在 worker 启动时应用。`on_exception` 和 `max_pending` 在 handler 的整个生命周期内有效。事后改它们等于再构造一个 handler。

## 版本

```cpp
#if TASKHANDLER_VERSION < 300          // major * 10000 + minor * 100 + patch
#error TaskHandler 0.3 or newer is required
#endif

std::cout << TASKHANDLER_VERSION_STRING << '\n';   // the header's version
std::cout << conan::runtime_version() << '\n';     // the library's own
```

两者不一致，只会发生在头文件和旁边的编译库已经漂移的时候，这正是 `runtime_version()` 的用途。主版本仍为 0 时，次版本 bump 可能破坏 API 或 ABI；[changelog](CHANGELOG.md) 会写明。

## 构建

依赖由 [vcpkg](https://github.com/microsoft/vcpkg) 管理；把 `VCPKG_ROOT` 指到你的 checkout。GoogleTest 只在 `tests` feature 里拉取，库的消费方不需要它。

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

| Preset | 构建 |
| --- | --- |
| `debug` | Debug，共享库 |
| `release` | Release，共享库 |
| `static` | Release，静态库 |
| `asan` | AddressSanitizer 与 UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |

测试套件会编两遍，分别对着两种消费方式，这样 header-only 和编译库不会悄悄分叉。

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `TASKHANDLER_BUILD_TESTS` | 作为顶层工程时打开 | 编测试套件 |
| `TASKHANDLER_BUILD_EXAMPLES` | 作为顶层工程时打开 | 编 `examples/` |
| `TASKHANDLER_BUILD_BENCHMARKS` | 作为顶层工程时打开 | 编 `benchmarks/` |
| `TASKHANDLER_INSTALL` | 作为顶层工程时打开 | 生成安装规则 |
| `TASKHANDLER_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX` |
| `BUILD_SHARED_LIBS` | `OFF` | 共享库而不是静态库 |

`examples/basic.cc` 是 handler 公开 API 的可运行导览。`examples/thread_pool.cc` 覆盖线程池，包括把结果弹回 handler。`benchmarks/task_handler_benchmark.cc` 测提交、调度和往返；改队列前和改完后在同一台机器上跑。

库需要 C++23（GCC 13、Clang 17、MSVC 17.7 或 AppleClang）以及 CMake 3.28。CI 还会用 C++26 再编一遍，覆盖更新的消费方。Apple 的 libc++ 仍没有 `std::move_only_function`；那些构建用一小段 polyfill 存放入队的可调用对象。

## 保证与限制

可以依赖的：

- 每个 handler 一条 worker，同一 handler 上的任务绝不会并发跑。
- `ThreadPool` 上的任务可以并发；共享状态要保护。单个 worker 仍按提交顺序跑已入队的任务。
- handler 上优先级高的先跑；同优先级按提交顺序。
- `stop()` 和析构会跑已经接受的工作。
- 延时任务绝不会在截止时刻之前跑。
- `instance()` 返回的引用在程序生命周期内一直有效。

需要小心的：

- 除非设置 `max_pending`，队列无界。生产者跑得比 handler 快就会无限增长。
- 两个 handler 互相 `Blocked` 会死锁，和两把互斥锁反序拿是一回事。
- handler 必须活得比它的 worker 久，所以不能从自己的任务里销毁自己。从那里 `stop()` 没问题，但析构无法等待它正在上面跑的那条线程，worker 会继续使用已经销毁的对象。
- 优先级不抢占。一个长时间任务会拖住后面所有工作。
- 同一个二进制里混用 `TaskHandler::header_only` 和 `TaskHandler::task_handler` 会得到两套共享 handler。选一个。
- 如果每个 `ThreadPool` worker 都阻塞在等待更多池内工作上，池会死锁。就地执行救不了这种情况。

## 参与贡献

[CONTRIBUTING.md](CONTRIBUTING.md) 写了构建方式、CI 跑哪些检查，以及一次改动该带上什么。[docs/design.md](docs/design.md) 解释内部为什么长这样，包括试过又丢掉的方案。

## 许可证

本仓库代码采用 MIT License。
