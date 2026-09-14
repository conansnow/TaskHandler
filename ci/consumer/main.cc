#include "conan/task_handler.h"

#include <cstdio>
#include <cstdlib>

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

  std::printf("consumed TaskHandler successfully\n");
  return EXIT_SUCCESS;
}
