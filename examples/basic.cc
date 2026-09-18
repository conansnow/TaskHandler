// A tour of the TaskHandler API. Build it with -DTASKHANDLER_BUILD_EXAMPLES=ON
// and run `task_handler_example`.

#include "conan/task_handler.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

void tour() {
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

  std::cout << "Delayed Future: a result once the deadline is due\n";
  std::future<int> later =
      handler.add_callable_after<conan::Future>(10ms, [] { return 42; });
  std::cout << "  later=" << later.get() << '\n';

  std::cout << "Failure in a Queued task\n";
  handler.add_callable(
      [] { throw std::runtime_error("something went wrong"); });

  std::this_thread::sleep_for(150ms);
  handler.flush();
  std::cout << "pending=" << handler.pending() << '\n';

  std::cout << "Backpressure: a bounded handler refuses work\n";
  {
    conan::TaskHandlerOptions bounded_options;
    bounded_options.max_pending = 2;
    conan::TaskHandler bounded{std::move(bounded_options)};

    // Parking the worker means the two tasks below stay queued, so the third
    // submission is the one the limit refuses. Waiting for the parked task to
    // start matters: until it does, it is itself one of the two the queue
    // holds.
    auto entered = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    std::future<void> has_entered = entered->get_future();
    std::shared_future<void> released = release->get_future().share();
    bounded.add_callable([entered, released] {
      entered->set_value();
      released.wait();
    });
    has_entered.wait();

    bounded.add_callable([] {});
    std::cout << "  first accepted\n";
    bounded.add_callable([] {});
    std::cout << "  second accepted\n";
    try {
      bounded.add_callable([] {});
    } catch (const conan::TaskHandlerQueueFull &refused) {
      std::cout << "  refused: " << refused.what() << '\n';
    }
    release->set_value();
  }

  // The destructor drains whatever is still runnable and joins the worker.
}

} // namespace

int main() {
  try {
    tour();
  } catch (const std::exception &caught) {
    std::cerr << "example failed: " << caught.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
