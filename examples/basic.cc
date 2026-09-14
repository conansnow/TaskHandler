// A tour of the TaskHandler API. Build it with -DTASKHANDLER_BUILD_EXAMPLES=ON
// and run `task_handler_example`.

#include "conan/task_handler.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;

int main() {
  conan::TaskHandlerOptions options;
  options.thread_name = "example";
  // Queued tasks have no future to fail through, so this is the only place a
  // failure inside one can be observed.
  options.on_exception = [](std::exception_ptr error) {
    try {
      std::rethrow_exception(std::move(error));
    } catch (const std::exception &caught) {
      std::cout << "  [on_exception] " << caught.what() << '\n';
    }
  };

  conan::TaskHandler handler{std::move(options)};

  std::cout << "Queued: returns immediately\n";
  handler.add_callable([] { std::cout << "  ran on the worker thread\n"; });

  std::cout << "Blocked: returns once the task has run\n";
  handler.add_callable<conan::Blocked>(
      [] { std::cout << "  ran before add_callable returned\n"; });

  std::cout << "Future: typed results, including move-only ones\n";
  std::future<std::string> text =
      handler.add_callable<conan::Future>([] { return std::string{"hello"}; });
  std::future<std::unique_ptr<int>> owned = handler.add_callable<conan::Future>(
      [] { return std::make_unique<int>(7); });
  std::cout << "  text=" << text.get() << " owned=" << *owned.get() << '\n';

  std::cout
      << "Priority: higher values run first, ties keep submission order\n";
  {
    // Nothing below can start until this blocking task lets the worker go, so
    // the output order is decided by priority rather than by timing.
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    handler.add_callable([released] { released.wait(); });

    handler.add_callable([] { std::cout << "  low (-1)\n"; }, -1);
    handler.add_callable([] { std::cout << "  high (10)\n"; }, 10);
    handler.add_callable([] { std::cout << "  normal (0)\n"; });
    release.set_value();
    handler.flush();
  }

  std::cout << "Scheduling and cancellation\n";
  handler.add_callable_after(50ms, [] { std::cout << "  fired after 50ms\n"; });
  const conan::TaskId doomed =
      handler.add_callable_after(50ms, [] { std::cout << "  never runs\n"; });
  std::cout << "  cancelled: " << std::boolalpha << handler.cancel(doomed)
            << '\n';

  std::cout << "Failure in a Queued task\n";
  handler.add_callable(
      [] { throw std::runtime_error("something went wrong"); });

  std::this_thread::sleep_for(150ms);
  handler.flush();
  std::cout << "pending=" << handler.pending() << '\n';

  // The destructor drains whatever is still runnable and joins the worker.
  return 0;
}
