// The one translation unit of the compiled-library build. Everything it
// contains lives in detail/task_handler-inl.h, which the header-only build
// inlines instead, so the two modes cannot drift apart.

#include "conan/task_handler.h"

#include "conan/detail/task_handler-inl.h"

namespace {

// Keep the Apple polyfill from rotting on libstdc++/MSVC CI, which would
// otherwise never instantiate it.
[[maybe_unused]] void touch_move_only_task_polyfill() {
  conan::detail::MoveOnlyTask task{[] {}};
  task();
  task = nullptr;
}

} // namespace
