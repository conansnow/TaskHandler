# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0]

### Breaking

- `add_callable<Future>` returns `std::future<R>` for the task's own `R`
  instead of `std::future<std::any>`. Drop the `std::any_cast` at the call
  site. Move-only results now work, which they could not before.
- **Priority now reads the conventional way round: higher values run first.**
  Previously lower values did. If you passed a non-zero priority, flip its
  sign. Equal priorities still run in submission order.
- `get_instance()` is replaced by `instance()`, which returns a reference
  rather than a pointer and can never be null.
- `init()` and `uninit()` return `void` rather than an `int` that was always
  `0`, and they exist in both header-only and compiled builds instead of only
  the compiled one. They are also now optional, since `instance()` creates a
  started handler on first use.
- The header moves from `inc/task_handler.h` to
  `include/conan/task_handler.h`.
- The `CURRENT_NAMESPACE_START`, `CURRENT_NAMESPACE_END` and
  `CURRENT_NAMESPACE` macros are gone. Use `namespace conan` directly.
- The `is_queued_v`, `is_blocked_v` and `is_future_v` traits move into
  `conan::detail`. The `Queued`, `Blocked` and `Future` tags are unchanged.
- Submitting to a stopped handler throws `conan::TaskHandlerStopped` instead of
  a bare `std::runtime_error`.

### Added

- `TaskHandler` can be constructed directly, so a program is no longer limited
  to the three shared handlers.
- `add_callable_after(delay, ...)` and `add_callable_at(deadline, ...)` for
  delayed work.
- `cancel(TaskId)` for tasks that have not started yet. `Queued` and delayed
  submissions return a `TaskId`.
- `pending()`, `flush()`, `running()` and `is_current_thread()`.
- `TaskHandlerOptions::on_exception`, a hook for failures in `Queued` tasks,
  which previously had nowhere to go and were discarded in silence.
- `TaskHandlerOptions::thread_name` for named worker threads on Linux and
  macOS. Shared handlers name themselves `conan-task-0` through
  `conan-task-2`.
- Move-only callables can be submitted. A lambda capturing a `std::unique_ptr`
  previously failed to compile against `std::function`'s copy-constructible
  requirement.
- Install rules and a CMake package config, so `find_package(TaskHandler)`
  works, exporting `TaskHandler::task_handler` and `TaskHandler::header_only`.
- `asan`, `tsan` and `static` CMake presets, and a CI matrix covering GCC,
  Clang and MSVC across both consumption modes.
- `examples/basic.cc`, a runnable tour of the API.

### Fixed

- Calling `stop()` or `uninit()` from inside a task terminated the process:
  `stop()` called `std::thread::join()` unconditionally, and a thread joining
  itself throws `std::system_error("Resource deadlock avoided")`. The worker
  now records the request and unwinds instead.
- `uninit()` deleted the handlers while other threads could still be holding a
  pointer from `get_instance()`. Shared handlers are now never freed, so
  `uninit()` only stops their workers and a held reference cannot dangle.
- `get_instance()` called `std::abort()` when no handler was present, which in
  header-only builds could be reached through static initialization order.
  Handlers are now created lazily on first use, so the case cannot arise.
- `~TaskHandler()` was defaulted: it neither stopped the worker nor freed
  queued tasks. It now drains and joins.
- Nothing in the build ever asked for pthread. It only linked because glibc
  2.34 folded libpthread into libc; `Threads::Threads` is now linked properly.
- The shared object exported 429 symbols, including internal template
  instantiations, because the visibility attribute in the header had no
  matching `-fvisibility=hidden`. It now exports 27.
- Consumers had to define `TASKHANDLER_COMPILED_LIB` and
  `TASKHANDLER_SHARED_LIB` by hand, because the library declared them
  `PRIVATE` even though they select the header's mode. They are `PUBLIC` now.
- `.clang-format` used the clang-format 6 `RawStringFormats` schema, so every
  clang-format from 9 onwards rejected the file outright and the project's
  style was never applied.
- `CONAN_DISABLE_MOVE` deleted `TYPE(const TYPE &&)`, which is not the move
  constructor. The macro is gone along with the type it guarded.

### Changed

- Tasks are stored as a move-only type-erased `detail::Task` rather than a
  `std::variant` over a `Callable` helper, removing the dependency on
  `<any>`, `<variant>` and `<algorithm>`.
- The queue is a `std::map` keyed on `(priority, sequence)`, so insertion is
  O(log n) rather than the O(n) `std::upper_bound` walk over a `std::list`,
  and the FIFO tiebreak within a priority is explicit rather than incidental.
- Out-of-line definitions live in `include/conan/task_handler-inl.h`, shared
  by the header-only and compiled builds so the two cannot diverge.
- Tests move from `src/googletest_nomain.cc` to `tests/task_handler_test.cc`,
  grow from 15 cases to 37, and run against both consumption modes under
  ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer.
- GoogleTest moves behind a vcpkg `tests` feature, so it is no longer a hard
  dependency of the library.
- Tests, examples and install rules are options, defaulting to on only when
  TaskHandler is the top-level project.

## [0.1.0]

- Initial implementation: a priority task queue with `Queued`, `Blocked` and
  `Future` submission, three shared handlers, and header-only or compiled use.

[Unreleased]: https://github.com/conansnow/TaskHandler/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/conansnow/TaskHandler/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/conansnow/TaskHandler/releases/tag/v0.1.0
