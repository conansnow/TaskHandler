#include "task_handler.h"

CURRENT_NAMESPACE_START
#ifdef TASKHANDLER_COMPILED_LIB
TaskHandler *TaskHandler::thises_static[TaskHandler::MAX_THREADS]{};

namespace {
// Function-local so that the mutex is alive for any caller, whatever the
// initialization order of other translation units.
std::mutex &lifecycle_mutex() {
  static std::mutex mutex;
  return mutex;
}
bool initialized{false};
} // namespace

int TaskHandler::init() {
  // A plain load-then-store would let two concurrent callers both get past the
  // guard and start MAX_THREADS worth of handlers each.
  std::lock_guard<std::mutex> guard(lifecycle_mutex());
  if (initialized)
    return 0;
  for (size_t i = 0; i < MAX_THREADS; i++) {
    // Publish only once the worker is running, so that a concurrent
    // get_instance() cannot hand out a handler with no thread behind it.
    auto *handler = new TaskHandler;
    handler->start();
    thises_static[i] = handler;
  }
  initialized = true;
  return 0;
}

int TaskHandler::uninit() {
  std::lock_guard<std::mutex> guard(lifecycle_mutex());
  if (!initialized)
    return 0;
  for (size_t i = 0; i < MAX_THREADS; i++) {
    thises_static[i]->stop();
    delete thises_static[i];
    // Leaving the pointer dangling would turn a later get_instance() into a
    // use-after-free rather than a diagnosed error.
    thises_static[i] = nullptr;
  }
  initialized = false;
  return 0;
}
#endif
CURRENT_NAMESPACE_END
