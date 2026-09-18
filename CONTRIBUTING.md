# Contributing

Bug reports and patches are welcome. This is a small library and the bar is
mostly about the same things CI checks, so this page is short.

## Getting a build

Dependencies come from [vcpkg](https://github.com/microsoft/vcpkg); point
`VCPKG_ROOT` at your checkout. Only the test suite needs anything at all
(GoogleTest, behind the `tests` feature), and the library itself has no
dependencies beyond the standard library and a thread library.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `static`, `asan`, `tsan`. All of them build the
test suite twice, once against `TaskHandler::header_only` and once against
`TaskHandler::task_handler`, so a change cannot work in one mode and break the
other.

## Before opening a pull request

Run what CI runs, or as much of it as your platform can:

```sh
ctest --preset debug
ctest --preset asan
TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan --repeat until-fail:20
clang-format-18 --dry-run --Werror $(git ls-files '*.h' '*.cc')
clang-tidy-18 -p out/build/debug --warnings-as-errors='*' \
    src/task_handler.cc examples/basic.cc benchmarks/task_handler_benchmark.cc
```

`--warnings-as-errors` is not decoration: clang-tidy exits 0 on findings, so
without it the check passes whatever it reports. The test file is left out on
purpose, since its findings are almost entirely GoogleTest macro expansion.

The sanitizer runs are not optional for a change to the queue or to the
lifecycle. A threading bug that only shows up one run in fifty is the normal
case here, which is why the TSan job repeats.

A packaging change (`CMakeLists.txt`, install rules, exported targets) should
also prove the consumer project still builds:

```sh
cmake --preset release && cmake --build --preset release
cmake --install out/build/release
cmake -S ci/consumer -B out/consumer-find-package -G Ninja \
    -DCMAKE_PREFIX_PATH="$PWD/out/install/release"
cmake --build out/consumer-find-package
```

`clang-format` and `clang-tidy` are pinned to major version 18 because their
output drifts between releases; a different version may disagree with CI.

## What a change comes with

- **A test.** Every bug this library has had is a named regression test in
  `tests/task_handler_test.cc`, because each of them looked impossible until it
  happened. Prefer the `Gate` helper over a sleep: it parks the worker inside a
  task, so a test about ordering or about what is still queued becomes
  deterministic instead of timing-dependent.
- **A changelog entry** under `## [Unreleased]` in
  [CHANGELOG.md](CHANGELOG.md), in the `Added`, `Changed`, `Fixed` or
  `Breaking` group. Write it for someone upgrading: what changed for them, and
  what they have to do about it.
- **Documentation**, when the change is visible from outside. The README is the
  reference; [docs/design.md](docs/design.md) is for why the internals look the
  way they do, including alternatives that were tried and rejected.
- **A benchmark run**, when the change touches the queue. `benchmarks/` exists
  so that "did that cost anything" has an answer.

## Style

`.clang-format` and `.clang-tidy` are the authority; both are checked in, and
between them they cover layout and naming. Beyond that:

- Comments explain *why*, not what. The code already says what it does; a
  comment earns its place by recording a constraint, a trade-off or a bug that
  the next reader would otherwise have to rediscover.
- Public declarations are documented where they are declared, in
  `include/conan/task_handler.h`. Definitions in
  `include/conan/detail/task_handler-inl.h` document their implementation, not
  their contract.
- User code -- a task body, a task destructor, the exception hook -- must never
  run with `mutex_` held. It can call back into the handler, and one that does
  must not deadlock.
- New members on `TaskHandler` or `TaskHandlerOptions` break the shared-library
  ABI. That is allowed while the major version is 0, but say so in the
  changelog.

## Versions

The version appears in `project()` in `CMakeLists.txt`, in
`TASKHANDLER_VERSION_*` in the public header, and in `vcpkg.json`. Configuring
the project checks the first two against each other, so a bump that misses one
fails the build rather than shipping.

Semantic versioning, with the usual pre-1.0 caveat: while the major version is
0, a minor bump may break API or ABI. Do not bump the version in a feature pull
request; entries accumulate under `## [Unreleased]` and the release bumps it
once. The installed package uses `SameMinorVersion` for that reason.
