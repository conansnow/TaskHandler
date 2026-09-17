#include "conan/task_handler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
  conan::TaskHandler handler;

  const int answer =
      handler.add_callable<conan::Future>([] { return 42; }).get();

  int blocked = 0;
  handler.add_callable<conan::Blocked>([&blocked] { blocked = 7; });

  if (answer != 42 || blocked != 7) {
    std::fprintf(stderr, "unexpected results: answer=%d blocked=%d\n", answer,
                 blocked);
    return EXIT_FAILURE;
  }

  // Also a check that the version symbol is exported by the shared build, and
  // that the header the consumer compiled against is the one the library was
  // built from.
  if (std::strcmp(conan::runtime_version(), TASKHANDLER_VERSION_STRING) != 0) {
    std::fprintf(stderr, "version skew: header %s, library %s\n",
                 TASKHANDLER_VERSION_STRING, conan::runtime_version());
    return EXIT_FAILURE;
  }

  std::printf("consumed TaskHandler %s successfully\n",
              conan::runtime_version());
  return EXIT_SUCCESS;
}
