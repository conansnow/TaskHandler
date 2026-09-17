# Agent instructions

This file is for coding agents. Humans should start at
[CONTRIBUTING.md](CONTRIBUTING.md). The public contract is
[README.md](README.md); why the internals look this way is
[docs/design.md](docs/design.md).

## What this is

TaskHandler is a serial executor for C++17: one worker thread per
handler, one task at a time, in a defined order. State owned by a
handler needs no locking, because only its worker ever touches it.

It is not a thread pool, not a work-stealing scheduler, and not a
coroutine runtime. Those belong in a different type. The library has
no dependencies beyond the standard library and a thread library.

Usable header-only or as a compiled library, from the same source.
Pick one CMake target per binary and do not mix them.

## Layout

- `include/conan/task_handler.h` -- public API. Document the contract here.
- `include/conan/detail/task_handler-inl.h` -- shared out-of-line
  definitions. Document the implementation, not the contract.
- `src/task_handler.cc` -- compiled-library translation unit. It only
  includes the two headers.
- `tests/task_handler_test.cc` -- the suite. Built twice, once against
  each target.
- `examples/basic.cc` -- runnable tour of the public API.
- `benchmarks/` -- submission, scheduling and round-trip timings. No
  extra framework.
- `ci/consumer/` -- downstream smoke test for `find_package` and
  FetchContent.
- `CMakePresets.json` -- `debug`, `release`, `static`, `asan`, `tsan`.

Do not include `detail/task_handler-inl.h` from consumer code. The
public header pulls it in for header-only builds;
`src/task_handler.cc` compiles it once otherwise.

## Commands

Dependencies come from vcpkg. Point `VCPKG_ROOT` at a checkout.
Every preset enables the `tests` feature so GoogleTest is available.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

What CI also runs, for a change to the queue or the lifecycle:

```sh
ctest --preset asan
TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan --repeat until-fail:20
clang-format-18 --dry-run --Werror $(git ls-files '*.h' '*.cc')
clang-tidy-18 -p out/build/debug --warnings-as-errors='*' \
    src/task_handler.cc examples/basic.cc \
    benchmarks/task_handler_benchmark.cc
```

clang-format and clang-tidy are pinned to major version 18 because
their output drifts. A different version may disagree with CI; do
not reformat the tree with an unpinned binary.

`--warnings-as-errors` is required: clang-tidy exits 0 on findings,
so without it the check passes whatever it reports. The test file is
left out on purpose (GoogleTest macro expansion).

`examples/basic.cc` is a runnable tour. `benchmarks/` answers "did
that cost anything" for a queue change; run it before and after, on
the same machine. CI's benchmark step is a smoke run, not a
measurement.

A packaging change (`CMakeLists.txt`, install rules, exported
targets) should also prove the consumer project still builds:

```sh
cmake --preset release && cmake --build --preset release
cmake --install out/build/release
cmake -S ci/consumer -B out/consumer-find-package -G Ninja \
    -DCMAKE_PREFIX_PATH="$PWD/out/install/release"
cmake --build out/consumer-find-package
```

## Invariants

A change has to preserve these. The rationale, including alternatives
that were tried and rejected, lives in
[docs/design.md](docs/design.md).

- One worker per handler. Tasks on the same handler never run
  concurrently.
- A task is in exactly one of `ready_`, `timed_`, or the worker's
  hands.
- `sequence_` is never reused. That is what makes a `TaskId` safe to
  cancel with.
- Promotion from `timed_` to `ready_` keeps the original sequence
  number.
- Nothing accepted into `ready_` is dropped. `stop()` and
  destruction drain it, because a `Blocked` caller is waiting on a
  promise only the task can fulfil. Delayed tasks that are not yet
  due are discarded.
- User code -- a task body, a task destructor, the exception hook --
  never runs with `mutex_` held. It can call back into the handler.
- Lock order is `lifecycle_mutex_` then `mutex_`, never the other
  way round.
- `Blocked` and `Future` run inline when already on the worker.
  Queueing there is a self-deadlock.
- Shared handlers from `instance()` are never freed. `uninit()`
  stops their workers and leaves the objects in place so a held
  reference cannot dangle.
- `Blocked` borrows the caller's callable (the call does not return
  until the task has run). `Queued` and `Future` own it. Do not
  capture a caller temporary by reference on a path that outlives
  the call -- that was a real ASan finding.

## How to change the code

`.clang-format` and `.clang-tidy` are the authority for layout and
naming. Beyond that:

- Comments explain *why*, not what. Record a constraint, a trade-off
  or a bug the next reader would otherwise rediscover.
- Public declarations are documented where they are declared, in
  `include/conan/task_handler.h`.
- Prefer `std::unique_ptr<detail::Task>` over `std::function`. The
  latter requires copy-constructible targets and would reject a
  lambda that captured a `std::unique_ptr`.
- Do not pimpl `TaskHandler`. Adding a member is an ABI break while
  the major version is 0; say so in the changelog rather than paying
  an allocation per handler.
- Do not introduce a lock-free queue, a binary heap for `ready_`, or
  a thread-local "current handler" pointer. Each of those was
  considered and rejected; see
  [docs/design.md](docs/design.md#alternatives-considered).
- Do not add library dependencies. GoogleTest sits behind the vcpkg
  `tests` feature so consumers never see it.
- New members on `TaskHandler` or `TaskHandlerOptions` break the
  shared-library ABI. Allowed before 1.0; the changelog must say so.
- Exceptions thrown across the shared-object boundary need
  `TASKHANDLER_VISIBLE`. Hidden visibility is on for the compiled
  library on purpose.
- Stay on C++17 in the library itself. CI also rebuilds as C++20 and
  C++23, so do not rely on a newer overload set by accident.

## Testing

Threading bugs do not reproduce on demand. Lean on determinism where
you can and on repetition where you cannot.

- Add a named regression test in `tests/task_handler_test.cc` for
  every bug. Prefer the `Gate` helper over a sleep: it parks the
  worker inside a task, so tests about ordering or about what is
  still queued are deterministic.
- State shared with a task must outlive the test frame. Capture
  `shared_ptr` by value, not stack locals by reference.
- The suite is compiled twice. A change that works in one
  consumption mode and breaks the other is not done.
- Sanitizer runs are not optional for a change to the queue or the
  lifecycle. TSan repeats because a bug that shows up one run in
  fifty is the normal case here.
- Do not destroy a `TaskHandler` from inside one of its own tasks,
  even in a test. That is a documented limit, not something the
  library can absorb.

## What a change comes with

- A test, as above.
- A changelog entry under `## [Unreleased]` in
  [CHANGELOG.md](CHANGELOG.md), in the `Added`, `Changed`, `Fixed`
  or `Breaking` group. Write it for someone upgrading.
- Documentation, when the change is visible from outside. The README
  is the reference.
- A benchmark run, when the change touches the queue.

Do not bump the version in a feature pull request. The version
appears in `project()` in `CMakeLists.txt`, in
`TASKHANDLER_VERSION_*` in the public header, and in `vcpkg.json`.
Configuring checks the first two against each other. Entries
accumulate under Unreleased; a release bumps all three once.

While the major version is 0, a minor bump may break API or ABI.

Security-sensitive bugs belong in a private advisory, not a public
issue. See [SECURITY.md](SECURITY.md).

## Cursor Cloud specific instructions

There is no application to launch and no browser flow to click
through. Verify with the CMake presets and `ctest`.

The environment needs CMake 3.22 or newer, Ninja, a C++17 compiler,
and `VCPKG_ROOT` pointing at a vcpkg checkout. Every preset sets
`VCPKG_MANIFEST_FEATURES=tests`. clang-format and clang-tidy, when
used, must be major version 18.

Build trees belong under `out/` and are gitignored. Do not commit
them.

If `VCPKG_ROOT` is unset, locate or bootstrap vcpkg before
configuring; the presets will not work without the toolchain file.
