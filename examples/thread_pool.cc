// A tour of ThreadPool, including bouncing results onto a TaskHandler.
// Build it with -DTASKHANDLER_BUILD_EXAMPLES=ON and run `thread_pool_example`.

#include "conan/thread_pool.h"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <utility>

namespace {

void tour() {
  conan::ThreadPoolOptions options;
  options.thread_count = 2;
  options.thread_name_prefix = "example-pool";
  options.on_exception = [](std::exception_ptr error) {
    try {
      std::rethrow_exception(std::move(error));
    } catch (const std::exception &caught) {
      std::cout << "  [on_exception] " << caught.what() << '\n';
    }
  };

  conan::ThreadPool pool{std::move(options)};
  std::cout << "thread_count=" << pool.thread_count() << '\n';

  std::cout << "Queued: returns immediately, may run in parallel\n";
  auto both_entered = std::make_shared<std::promise<void>>();
  auto remaining = std::make_shared<std::atomic_int>(2);
  auto release = std::make_shared<std::promise<void>>();
  std::shared_future<void> released = release->get_future().share();
  auto mark_entered = [remaining, both_entered, released] {
    if (remaining->fetch_sub(1) == 1)
      both_entered->set_value();
    released.wait();
  };
  pool.add_callable(mark_entered);
  pool.add_callable(mark_entered);
  both_entered->get_future().wait();
  std::cout << "  two workers were inside a task at the same time\n";
  release->set_value();
  pool.flush();

  std::cout << "Blocked and Future use the same policy tags as TaskHandler\n";
  pool.add_callable<conan::Blocked>(
      [] { std::cout << "  ran before add_callable returned\n"; });
  std::future<int> answer = pool.add_callable<conan::Future>([] { return 42; });
  std::cout << "  answer=" << answer.get() << '\n';

  std::cout << "Bounce a result onto a serial handler\n";
  conan::TaskHandler handler;
  auto applied = std::make_shared<std::promise<int>>();
  pool.add_callable([&handler, applied] {
    const int result = 7;
    handler.add_callable([applied, result] { applied->set_value(result); });
  });
  std::cout << "  applied=" << applied->get_future().get() << '\n';

  std::cout << "Backpressure: a bounded pool refuses work\n";
  {
    conan::ThreadPoolOptions bounded_options;
    bounded_options.thread_count = 1;
    bounded_options.max_pending = 1;
    conan::ThreadPool bounded{std::move(bounded_options)};

    auto entered = std::make_shared<std::promise<void>>();
    auto hold = std::make_shared<std::promise<void>>();
    std::future<void> has_entered = entered->get_future();
    std::shared_future<void> held = hold->get_future().share();
    bounded.add_callable([entered, held] {
      entered->set_value();
      held.wait();
    });
    has_entered.wait();

    bounded.add_callable([] {});
    std::cout << "  first accepted\n";
    try {
      bounded.add_callable([] {});
    } catch (const conan::ThreadPoolQueueFull &refused) {
      std::cout << "  refused: " << refused.what() << '\n';
    }
    hold->set_value();
  }
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
