# Overview

TaskHandler is a simple and easy-to-use event handler written in c++, supporting both header-only and lib use cases.

# Usage

Tasks are submitted to one of `MAX_THREADS` handlers, each of which owns a single
worker thread and runs its tasks in ascending priority order (lower value first,
submission order preserved within a priority).

```cpp
using namespace conan;

// Fire and forget.
TaskHandler::get_instance()->add_callable([] { /* ... */ });

// Return only once the task has run.
TaskHandler::get_instance()->add_callable<Blocked>([] { /* ... */ });

// Return a future. Non-void results are boxed in std::any.
auto result = TaskHandler::get_instance()->add_callable<Future>([] { return 42; });
int value = std::any_cast<int>(result.get());

// Pick a handler, and give a task a priority.
TaskHandler::get_instance<1>()->add_callable([] { /* ... */ }, -1);
```

## Header-only vs. compiled lib

Without `TASKHANDLER_COMPILED_LIB` the header is self-contained and the handlers
are started and stopped automatically.

With `TASKHANDLER_COMPILED_LIB`, `TaskHandler::init()` must be called before the
first `get_instance()` and `TaskHandler::uninit()` on shutdown. Both are
idempotent and safe to call from multiple threads. Calling `get_instance()`
before `init()` or after `uninit()` reports the mistake and aborts rather than
returning a null or dangling handler.

Because the two modes give `TaskHandler` different storage and lifetime, every
translation unit that includes this header must agree on `TASKHANDLER_COMPILED_LIB`
and `TASKHANDLER_SHARED_LIB`.

## Errors and shutdown

- An exception thrown by a `Blocked` or `Future` task is delivered to the
  caller, from `add_callable` and `future::get()` respectively. A `Queued` task
  has no such channel, so its exception is discarded and the worker survives.
- Shutdown drains whatever has already been accepted, so a task that was queued
  before shutdown still runs and a `Blocked` caller is never left waiting.
- Once shutdown has begun, `add_callable` throws `std::runtime_error` instead of
  accepting work that could never run.

# Build

Requires a C++17 compiler and GTest for the tests.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

# License

The code in this repository is licensed under the MIT License.
