# TaskHandler

A small single-worker task queue for C++23, plus a sibling thread pool for
work that is allowed to run concurrently.

Every handler owns exactly one thread, and everything submitted to that handler
runs on it, one task at a time. That is what makes it useful as an event
handler: state owned by a handler needs no locking, because only its worker
ever touches it. Submit work and forget about it, submit work and wait for it,
or submit work and take a future for the result.

CPU work that can overlap belongs on `ThreadPool`, not on a second worker
inside the handler. Bounce results back onto a handler when they must touch
handler-owned state.

Usable header-only or as a compiled library, from the same source.

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

## Contents

- [Installing](#installing)
- [Submitting work](#submitting-work)
- [Priority](#priority)
- [Delayed work and cancellation](#delayed-work-and-cancellation)
- [Errors](#errors)
- [Backpressure](#backpressure)
- [Shared handlers](#shared-handlers)
- [Thread pool](#thread-pool)
- [Lifetime](#lifetime)
- [Options](#options)
- [Version](#version)
- [Building](#building)
- [Guarantees and limits](#guarantees-and-limits)
- [Contributing](#contributing)
- [License](#license)

## Installing

### Header-only

Copy `include/conan/` into your project and include the header. The consumer
must compile as C++23. Nothing else is needed, though you still have to link a
thread library:

```cmake
find_package(Threads REQUIRED)
target_include_directories(my_app PRIVATE third_party/TaskHandler/include)
target_link_libraries(my_app PRIVATE Threads::Threads)
```

### As a subdirectory

```cmake
add_subdirectory(third_party/TaskHandler)
target_link_libraries(my_app PRIVATE TaskHandler::header_only)   # or ::task_handler
```

### With FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(TaskHandler
    GIT_REPOSITORY https://github.com/conansnow/TaskHandler.git
    GIT_TAG v0.3.0
    )
FetchContent_MakeAvailable(TaskHandler)
target_link_libraries(my_app PRIVATE TaskHandler::task_handler)
```

Tests, examples, benchmarks and install rules default to on only when
TaskHandler is the top-level project, so a consumer builds the library and
nothing else.

### As an installed package

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

```cmake
find_package(TaskHandler 0.3 REQUIRED)
target_link_libraries(my_app PRIVATE TaskHandler::task_handler)
```

Two targets are exported:

| Target | What it does |
| --- | --- |
| `TaskHandler::header_only` | Interface target. Nothing to build or ship. |
| `TaskHandler::task_handler` | Compiled library. Shorter consumer build times, one shared object to ship. |

`TaskHandler::task_handler` propagates `TASKHANDLER_COMPILED_LIB` (and
`TASKHANDLER_SHARED_LIB` for shared builds) on its own, so there is nothing to
define by hand. Pick one target per binary and do not mix them.

## Submitting work

`add_callable` takes a policy tag as its first template argument. The default
is `Queued`.

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

Callables do not need to be copy-constructible, so captured `unique_ptr` state
is fine:

```cpp
handler.add_callable([data = std::move(data)] { consume(*data); });
```

`Blocked` and `Future` detect being called from the handler's own worker thread
and run the task inline instead of deadlocking, which makes recursive use safe:

```cpp
handler.add_callable<conan::Blocked>([&] {
  // Already on the worker, so this runs inline rather than queueing behind
  // a task that can never start.
  handler.add_callable<conan::Blocked>([&] { nested(); });
});
```

## Priority

Every submission takes an optional priority. **Higher values run first.** Tasks
of equal priority run in submission order.

```cpp
handler.add_callable([] { normal(); });            // priority 0
handler.add_callable([] { urgent(); }, 10);        // jumps the queue
handler.add_callable([] { whenever(); }, -10);     // sinks to the bottom
```

Priority only decides what the worker picks up next. It never interrupts a task
that is already running.

> Before 0.2.0 the comparison ran the other way and *lower* values went first.
> If you passed a non-zero priority to an older version, flip its sign.

## Delayed work and cancellation

```cpp
using namespace std::chrono_literals;

conan::TaskId id = handler.add_callable_after(5s, [] { retry(); });
handler.add_callable_at(deadline, [] { give_up(); });

if (too_late)
  handler.cancel(id);   // true if the task had not started yet

std::future<Reply> reply =
    handler.add_callable_after<conan::Future>(5s, [] { return fetch(); });
```

A delayed task becomes runnable at its deadline and is then ordered by priority
like anything else, so a busy handler may run it later than asked. It is never
run earlier. Delayed work is never run inline, even from the worker thread:
getting that future from inside a task would wait for work that cannot start
until the current task returns.

`cancel` returns `false` for a task that already ran, is running, or was
already cancelled. A `TaskId` stays `valid()` after that; it names a submission,
it does not mean the task is still pending. Delayed `Future` submissions return
the future rather than an id, so they are dropped by `stop()` (or destruction)
instead, which leaves the future broken.

## Errors

An exception from a `Blocked` task is rethrown out of `add_callable`. An
exception from a `Future` task is stored in the future and rethrown by `get()`.

A `Queued` task has nowhere to report a failure, so by default the exception is
discarded. Install a hook to see them:

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

The hook runs on the worker thread and only ever fires for `Queued` tasks.

A submission that is refused throws. Both cases share a base, so a caller that
only wants to know that the handler would not take the work can catch one type.

| Exception | Thrown when |
| --- | --- |
| `conan::TaskHandlerError` | Base of both; also a `std::runtime_error` |
| `conan::TaskHandlerStopped` | The handler has been stopped |
| `conan::TaskHandlerQueueFull` | `max_pending` tasks are already waiting |
| `conan::ThreadPoolError` | Base of the pool's refusal exceptions |
| `conan::ThreadPoolStopped` | The pool has been stopped |
| `conan::ThreadPoolQueueFull` | the pool already holds `max_pending` tasks |

## Backpressure

The queue is unbounded by default. Bound it when the producer can outrun the
handler, and submission is refused rather than growing the queue until the
process runs out of memory:

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

The limit counts runnable and not-yet-due tasks together, and excludes the task
currently running. Recursive `Blocked` and `Future` submissions from the worker
thread run inline without queueing, so the limit never refuses them.

It throws rather than blocking on purpose: a blocking submit is a place where an
unrelated thread can be parked indefinitely, and between two handlers that is a
deadlock waiting to happen.

## Shared handlers

Three process-wide handlers are available without constructing anything. They
are created on first use.

```cpp
conan::TaskHandler::instance().add_callable([] { work(); });      // index 0
conan::TaskHandler::instance<1>().add_callable([] { other(); });  // index 1
```

`instance(index)` takes a runtime index and throws `std::out_of_range` past
`instance_count()`. `init()` starts all three up front; `uninit()` drains and
stops them. Both are optional and idempotent, and shutdown also happens
automatically at program exit.

The returned reference stays valid for the rest of the program, including
across `uninit()`, so it can never be left dangling. After `uninit()` the
handler is simply stopped, and submitting to it throws until `init()`.

Need a different number of workers, or one you own? Construct your own:

```cpp
conan::TaskHandler render_thread;
conan::TaskHandler io_thread;
```

## Thread pool

`ThreadPool` is the type for work that is allowed to run concurrently.
`TaskHandler` stays one worker: that is the whole point of it. Include
`conan/thread_pool.h` and construct a pool you own. There is no process-wide
pool.

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

The policy tags are the same ones the handler uses. `Queued` returns `void`:
a pool does not cancel queued work, so there is no `TaskId`. Delayed work
stays on a handler; one thread should sleep on deadlines.

Bounce a result onto a handler when it must touch handler-owned state:

```cpp
conan::TaskHandler handler;
conan::ThreadPool pool;

pool.add_callable([&handler] {
  const int result = crunch();
  handler.add_callable([result] { apply(result); });
});
```

`stop()` drains accepted work. `max_pending` refuses with
`ThreadPoolQueueFull` rather than blocking. Recursive `Blocked` and `Future`
from a worker run inline, which keeps a 1-thread pool from deadlocking on
itself. If every worker is blocked waiting for more pool work, the pool still
deadlocks: that is the same hazard as any fixed-size pool.

## Lifetime

Constructing a handler starts its worker. Destroying it runs everything already
queued, then joins.

```cpp
void TaskHandler::start();   // idempotent
void TaskHandler::stop();    // drain, stop, join; idempotent
bool TaskHandler::running() const;
```

`stop()` runs the work that was already accepted rather than dropping it, so a
`Blocked` caller is never left waiting on a task that will not run. Delayed
tasks that are not yet due are discarded, and `start()` brings the handler back
afterwards.

Calling `stop()` from inside one of the handler's own tasks only records the
request and returns, because a thread cannot join itself. The worker finishes
draining and exits on its own. `start()` from inside a task does nothing at all:
the worker is running by definition, and a stop request already made can only be
taken back from another thread.

```cpp
std::size_t TaskHandler::pending() const;   // accepted, not yet started
void TaskHandler::flush();                  // wait for runnable work only
bool TaskHandler::is_current_thread() const;
```

`flush()` waits for runnable work only, not for delayed tasks that are still
waiting on a deadline. Called from the worker thread it returns immediately,
since waiting there could only deadlock.

## Options

```cpp
struct TaskHandlerOptions {
  std::string thread_name;                             // shown in debuggers
  std::function<void(std::exception_ptr)> on_exception; // Queued failures
  std::size_t max_pending;                             // 0 means unbounded
};
```

`thread_name` is applied on Linux, macOS and Windows. Linux truncates it to 15
characters. The shared handlers name themselves `conan-task-0` through
`conan-task-2`. Pool workers use `thread_name_prefix` the same way, as
`{prefix}-{index}`.

`thread_name` is applied when the worker starts. `on_exception` and
`max_pending` apply for the life of the handler. Changing any of them afterwards
means constructing another handler.

## Version

```cpp
#if TASKHANDLER_VERSION < 300          // major * 10000 + minor * 100 + patch
#error TaskHandler 0.3 or newer is required
#endif

std::cout << TASKHANDLER_VERSION_STRING << '\n';   // the header's version
std::cout << conan::runtime_version() << '\n';     // the library's own
```

The two differ only if a header has drifted away from the compiled library next
to it, which is what `runtime_version()` is for. While the major version is 0, a
minor bump may break API or ABI; the [changelog](CHANGELOG.md) says what.

## Building

Dependencies are managed with [vcpkg](https://github.com/microsoft/vcpkg);
point `VCPKG_ROOT` at your checkout. GoogleTest is only pulled in for the
`tests` feature, so consumers of the library do not need it.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

| Preset | Build |
| --- | --- |
| `debug` | Debug, shared |
| `release` | Release, shared |
| `static` | Release, static |
| `asan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |

The suite is compiled twice, once against each consumption mode, so the
header-only and compiled builds cannot quietly diverge.

| Option | Default | Effect |
| --- | --- | --- |
| `TASKHANDLER_BUILD_TESTS` | on when top level | Build the test suite |
| `TASKHANDLER_BUILD_EXAMPLES` | on when top level | Build `examples/` |
| `TASKHANDLER_BUILD_BENCHMARKS` | on when top level | Build `benchmarks/` |
| `TASKHANDLER_INSTALL` | on when top level | Generate install rules |
| `TASKHANDLER_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX` |
| `BUILD_SHARED_LIBS` | `OFF` | Shared instead of static |

`examples/basic.cc` is a runnable tour of the handler. `examples/thread_pool.cc`
covers the pool, including bouncing a result onto a handler.
`benchmarks/task_handler_benchmark.cc` times submission, scheduling and the
round trips; run it before and after a change to the queue.

The library needs C++23 (GCC 13, Clang 17, MSVC 17.7, or AppleClang) and
CMake 3.28. CI also rebuilds everything as C++26, so a newer consumer is
covered too. Apple's libc++ still lacks `std::move_only_function`; those
builds use a small polyfill for the queued callable.

## Guarantees and limits

What you can rely on:

- One worker per handler, so tasks on the same handler never run concurrently.
- Tasks on a `ThreadPool` may run concurrently; protect shared state.
- Higher priority first on a handler; equal priority in submission order.
- `stop()` and destruction run the work already accepted.
- A delayed task never runs before its deadline.
- A reference from `instance()` stays valid for the life of the program.

What to watch out for:

- The queue is unbounded unless you set `max_pending`. A producer that outruns
  its handler will otherwise grow it without limit.
- `Blocked` across two handlers that each block on the other deadlocks, exactly
  as two mutexes taken in opposite orders would.
- A handler must outlive its worker, so it cannot be destroyed from inside one
  of its own tasks. `stop()` from there is fine, but destruction cannot wait for
  a thread it is running on, and the worker would go on using a destroyed
  object.
- Priority does not preempt. One long task delays everything behind it.
- Mixing `TaskHandler::header_only` and `TaskHandler::task_handler` in one
  binary gives you two sets of shared handlers. Pick one.
- If every `ThreadPool` worker is blocked waiting for more pool work, the
  pool deadlocks. Inline-on-worker does not fix that.

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) covers the build, the checks CI runs and what
a change is expected to come with. [docs/design.md](docs/design.md) explains why
the internals look the way they do, including the parts that were tried and
rejected.

## License

The code in this repository is licensed under the MIT License.
