# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Breaking

- The library now requires C++23 and CMake 3.28. Rebuild consumers with a
  toolchain that can do both; GCC 13, Clang 17, MSVC 17.7 and AppleClang with a
  complete C++23 library are the floor CI actually runs. `CMAKE_CXX_STANDARD`
  still defaults to 23 and is still overridable, so CI rebuilds as C++26 to
  catch anything a newer consumer would hit. CMake 3.28 is the floor because
  that is when `CMAKE_CXX_SCAN_FOR_MODULES` exists; 4.0 is not required.
- Submission overloads are constrained with concepts (`QueuedPolicy`,
  `BlockedPolicy`, `FuturePolicy`, `TaskCallable`) rather than
  `std::enable_if`. Call sites that already passed a policy tag and an
  invocable do not change. The `detail::is_*_v` traits are gone.
- Queued work is stored as `std::move_only_function<void()>` instead of
  `std::unique_ptr<detail::Task>`. That is an ABI break for the compiled
  library; rebuild against the matching header. `TaskHandlerOptions::on_exception`
  is still `std::function`, so options stay copyable.
- The version macros remain 0.3.0 until this work is tagged. The tag that
  ships these breaks must be 0.4.0: `SameMinorVersion` would otherwise let a
  0.3 consumer accept an ABI-incompatible install.

### Changed

- `clang-format` and `clang-tidy` are pinned to major version 23. Reformat
  with that binary; 18 will disagree.
- The vcpkg baseline is current, which pulls GoogleTest 1.18 into the test
  feature. The library itself still has no dependencies.
- CMake turns off C++ module scanning (`CMAKE_CXX_SCAN_FOR_MODULES`). The
  library is not modular, and a missing `clang-scan-deps` otherwise breaks
  `find_package(Threads)` under Clang and CMake 3.28 or newer.
- `add_callable_after` takes any `std::chrono::duration` through the
  `detail::ChronoDuration` concept rather than a bare `Rep`/`Period` pair.
  Call sites that already passed a duration do not change.
- Shared-library exception types use `TASKHANDLER_VISIBLE` (on Windows that
  is an alias of `TASKHANDLER_API`) so a `TaskHandlerStopped` thrown inside
  the DLL can be caught by type outside it.
- Task and timer destruction share one internal nothrow helper. No API change.
- Immediate and delayed `Future` paths share one `packaged_task` helper, and
  the leftover `detail::make_task` wrapper is gone. No API change.
- The worker loop names its run and timer-discard phases. No API change.

### Fixed

- `cancel()` destroyed the cancelled callable while still holding `mutex_`. A
  destructor that called back into the handler deadlocked. The node is now
  extracted under the lock and destroyed after releasing it.
- Discarding undue timers on `stop()` cleared `worker_id_` before running
  those destructors, so `is_current_thread()` was already false and a
  destructor that called `start()` or `stop()` deadlocked against the joining
  `stop()`. The worker id stays set until the discarded tasks are gone.
- The shared-handler registry is no longer destroyed at process exit, so a
  static destructor that calls `instance()` cannot use a dead mutex. Workers
  are still joined via `atexit`.
- Promoting a due timer into `ready_` could drop the callable if the insert
  threw after the move. The mapped task is assigned only after `try_emplace`
  has allocated the node.

## [0.3.0]

### Breaking

- `TaskHandlerStopped` now derives from the new `conan::TaskHandlerError` rather
  than directly from `std::runtime_error`. Code catching either
  `TaskHandlerStopped` or `std::runtime_error` is unaffected.
- `TaskHandlerOptions` has a new member and so changes size, which breaks the
  shared-library ABI. Rebuild consumers against the matching header; a
  mismatched pair now shows up through `conan::runtime_version()` instead of
  misbehaving.

### Added

- `add_callable_after<Future>` and `add_callable_at<Future>` return
  `std::future<R>` for delayed work. Delayed tasks are never run inline, even
  from the worker thread. There is still no delayed `Blocked`: that would park
  the caller until the deadline. Cancelling a delayed Future happens by
  dropping it (`stop()` or destruction), which leaves the future broken.
- `TaskId` can be compared with `==` and `!=`. `valid()` still means "this id
  came from a submission", not "the task is still pending".
- `TaskHandlerOptions::max_pending` bounds the queue. Once it is reached,
  submitting throws the new `conan::TaskHandlerQueueFull` instead of letting a
  producer that outruns its worker grow the queue without limit. Zero, the
  default, keeps the queue unbounded. Recursive `Blocked` and `Future`
  submissions run inline and are never refused by it.
- `conan::TaskHandlerError`, the base of `TaskHandlerStopped` and
  `TaskHandlerQueueFull`, so a caller that only wants to know that a submission
  was refused can catch one type.
- `TASKHANDLER_VERSION_MAJOR`, `_MINOR`, `_PATCH`, `_STRING` and a comparable
  `TASKHANDLER_VERSION`, for consumers with no CMake project to ask, plus
  `conan::runtime_version()` for the version the linked library was built from.
  Configuring the project checks the macros against `project()`, so the two
  cannot drift.
- `benchmarks/`, timing submission throughput for one and for several producers,
  the same across sixteen priorities, timer bookkeeping, and the `Blocked` and
  `Future` round trips. No benchmark framework: the library has no dependencies
  and this did not need to be its first. Built when TaskHandler is the top-level
  project, or with `TASKHANDLER_BUILD_BENCHMARKS`.
- `docs/design.md`, on why the internals look the way they do, including the
  alternatives that were tried and rejected. `CONTRIBUTING.md`, `SECURITY.md`,
  issue and pull request templates, an `.editorconfig` and a dependabot
  configuration for the actions.

### Fixed

- `uninit()` deadlocked if a task on a shared handler called `instance()` (or
  `init()` / `uninit()`) while shutdown was joining that worker: the registry
  mutex was held across `stop()`. The pointers are now copied, the lock is
  dropped, then the workers are joined. A second pass catches a handler created
  during the first join.
- `scheduled_tasks_run_in_deadline_order` still slept until "only the first
  timer is due". On a loaded macOS runner that sleep overshot the next slot,
  two timers were promoted together, and the mid-test snapshot saw `{1, 2}`.
  The test now releases the gate immediately and only asserts the final
  deadline order.
- `~TaskHandler()` could `std::terminate` if `std::thread::join` threw, because
  the destructor is implicitly `noexcept`. It now swallows that error. The
  worker also reports, rather than dying on, an exception from a task
  destructor, including discarded delayed tasks.
- GitHub Actions never reached a compile: `lukka/run-vcpkg` still asked for
  the removed `x-gha` binary cache, so every job that needed GoogleTest died
  in configure. The workflow now uses a files cache instead.
- `start()` could not revive a handler that had been stopped from inside one of
  its own tasks. Such a stop leaves the `std::thread` joinable forever, because
  a worker cannot join itself, and `start()` read that as a live worker and
  returned. `running()` is already false once the stop flag is set, so a
  caller that waited on it could still hit the worker on the way out and get
  the same no-op. It now joins the exiting worker first, then starts a new
  one.
- `start()` from inside a task deadlocked against a concurrent `stop()`, which
  holds the lifecycle mutex while waiting to join that very worker. It now
  returns instead, as `stop()` already did from the same position.
- `stop()` documented undue delayed tasks as discarded but kept them, so
  `pending()` went on counting work nothing would run and a later `start()`
  resurrected tasks whose deadline had passed while the handler was down. The
  worker now drops them as it exits.

### Changed

- `cancel()` is `[[nodiscard]]`. Ignoring the result is ignoring whether the
  task had already started.
- The installed package config uses `SameMinorVersion` rather than
  `SameMajorVersion`, so `find_package(TaskHandler 0.2)` will not accept this
  0.3 install. That matches the 0.x policy already documented. From 1.0 this
  can become `SameMajorVersion`.
- The out-of-line definitions move from `include/conan/task_handler-inl.h` to
  `include/conan/detail/task_handler-inl.h`, matching the namespace they are
  already in. Nothing should have been including them directly.
- `CMAKE_CXX_STANDARD` is no longer forced to 17, so the project can be built as
  C++20 or C++23. CI does both, since consumers are free to be newer than the
  library.
- The example is compiled against both consumption modes, like the test suite,
  and it covers `max_pending`. The CI consumer project covers `FetchContent` and
  `add_subdirectory` as well as `find_package`.
- The clang-tidy job runs with `--warnings-as-errors`. clang-tidy exits 0 on
  findings, so the job previously only caught a translation unit that would not
  compile. The twelve findings it then reported are fixed, or carry a NOLINT
  with the reason where the code is deliberate.

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

[Unreleased]: https://github.com/conansnow/TaskHandler/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/conansnow/TaskHandler/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/conansnow/TaskHandler/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/conansnow/TaskHandler/releases/tag/v0.1.0
