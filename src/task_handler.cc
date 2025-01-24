#include "task_handler.h"

CURRENT_NAMESPACE_START
#ifdef TASKHANDLER_COMPILED_LIB
TaskHandler *TaskHandler::thises_static[TaskHandler::MAX_THREADS]{};

int TaskHandler::init() {
  static std::atomic_bool init{false};
  if (init)
    return 0;
  init = true;
  for (size_t i = 0; i < MAX_THREADS; i++) {
    thises_static[i] = new TaskHandler;
    thises_static[i]->start();
  }
  return 0;
}

int TaskHandler::uninit() {
  static std::atomic_bool uninit{false};
  if (uninit)
    return 0;
  uninit = true;
  for (size_t i = 0; i < MAX_THREADS; i++) {
    thises_static[i]->stop();
    delete thises_static[i];
  }
  return 0;
}
#endif
CURRENT_NAMESPACE_END
