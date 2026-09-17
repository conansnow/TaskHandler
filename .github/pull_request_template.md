<!--
CONTRIBUTING.md has the long version. Delete anything that does not apply.
-->

## What this changes

Why, not just what. If it fixes a bug, describe the bug: what state the handler
was in and what it did wrong.

## How it was verified

- [ ] `ctest --preset debug`
- [ ] `ctest --preset asan`
- [ ] `TSAN_OPTIONS=halt_on_error=1 ctest --preset tsan --repeat until-fail:20`
- [ ] `clang-format-18 --dry-run --Werror $(git ls-files '*.h' '*.cc')`
- [ ] `clang-tidy-18 -p out/build/debug --warnings-as-errors='*' ...`
- [ ] Benchmarks, if this touches the queue: before/after numbers below

## Notes

- [ ] Regression test added for anything that was broken
- [ ] `CHANGELOG.md` updated under `## [Unreleased]`
- [ ] Documentation updated, if the change is visible from outside
- [ ] Breaks API or ABI (new members on `TaskHandler` or
      `TaskHandlerOptions` do), and the changelog says so
