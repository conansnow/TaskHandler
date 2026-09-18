// Throughput and round-trip timings for conan::TaskHandler.
//
// Run it with no arguments, or pass a scale factor to trade runtime for
// stability: `task_handler_benchmark 4` does four times the work per case.
//
// The numbers are only meaningful against other numbers from the same machine.
// What they are for is answering "did that change to the queue cost anything",
// which is a question a single-worker queue otherwise invites guessing about.

#include "conan/task_handler.h"
#include "conan/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Case {
  std::string name;
  std::size_t operations{0};
  double seconds{0.0};
};

// `body` performs `operations` submissions and does not return until they have
// all run, so the timing covers dispatch as well as submission.
template <typename Body>
Case measure(std::string name, std::size_t operations, Body &&body) {
  const auto started = Clock::now();
  std::forward<Body>(body)();
  const auto elapsed = Clock::now() - started;
  return Case{std::move(name), operations,
              std::chrono::duration<double>(elapsed).count()};
}

void report(const std::vector<Case> &results) {
  std::printf("%-42s %12s %12s %14s\n", "case", "operations", "ns/op", "ops/s");
  std::printf("%-42s %12s %12s %14s\n", "------------------------------------",
              "----------", "-----", "-----");
  for (const Case &result : results) {
    const double ns_per_op = result.seconds * 1e9 / double(result.operations);
    const double per_second = double(result.operations) / result.seconds;
    std::printf("%-42s %12zu %12.1f %14.0f\n", result.name.c_str(),
                result.operations, ns_per_op, per_second);
  }
}

Case queued_throughput(std::size_t operations) {
  return measure("queued submit, one producer", operations, [operations] {
    conan::TaskHandler handler;
    for (std::size_t i = 0; i < operations; i++)
      handler.add_callable([] {});
    handler.flush();
  });
}

Case queued_throughput_contended(std::size_t operations, unsigned producers) {
  const std::size_t per_producer = operations / producers;
  return measure("queued submit, " + std::to_string(producers) + " producers",
                 per_producer * producers, [per_producer, producers] {
                   conan::TaskHandler handler;
                   std::vector<std::thread> threads;
                   threads.reserve(producers);
                   for (unsigned p = 0; p < producers; p++)
                     threads.emplace_back([&handler, per_producer] {
                       for (std::size_t i = 0; i < per_producer; i++)
                         handler.add_callable([] {});
                     });
                   for (std::thread &thread : threads)
                     thread.join();
                   handler.flush();
                 });
}

// Every submission here is a full hand-off to the worker and back, so this is
// latency rather than throughput: the worker is idle most of the time.
Case blocked_round_trip(std::size_t operations) {
  return measure("blocked round trip", operations, [operations] {
    conan::TaskHandler handler;
    std::atomic_int sink{0};
    for (std::size_t i = 0; i < operations; i++)
      handler.add_callable<conan::Blocked>([&sink] { ++sink; });
  });
}

Case future_round_trip(std::size_t operations) {
  return measure("future round trip", operations, [operations] {
    conan::TaskHandler handler;
    std::size_t sink = 0;
    for (std::size_t i = 0; i < operations; i++)
      sink += std::size_t(
          handler.add_callable<conan::Future>([] { return 1; }).get());
    if (sink != operations)
      std::fputs("benchmark lost a task\n", stderr);
  });
}

// The runnable queue is ordered by (priority, sequence), so a spread of
// priorities exercises the comparisons a single-priority workload never does.
Case mixed_priority_throughput(std::size_t operations) {
  return measure("queued submit, 16 priorities", operations, [operations] {
    conan::TaskHandler handler;
    for (std::size_t i = 0; i < operations; i++)
      handler.add_callable([] {}, int(i % 16) - 8);
    handler.flush();
  });
}

// Scheduling and cancelling without ever running the task, which is the timer
// bookkeeping on its own.
Case schedule_and_cancel(std::size_t operations) {
  return measure("schedule far out, then cancel", operations, [operations] {
    conan::TaskHandler handler;
    std::vector<conan::TaskId> ids;
    ids.reserve(operations);
    for (std::size_t i = 0; i < operations; i++)
      ids.push_back(handler.add_callable_after(std::chrono::hours(1), [] {}));
    for (const conan::TaskId &id : ids)
      if (!handler.cancel(id))
        std::fputs("benchmark failed to cancel a task\n", stderr);
  });
}

Case queued_pool_throughput(std::size_t operations) {
  return measure("pool queued submit, 4 workers", operations, [operations] {
    conan::ThreadPoolOptions options;
    options.thread_count = 4;
    conan::ThreadPool pool{std::move(options)};
    for (std::size_t i = 0; i < operations; i++)
      pool.add_callable([] {});
    pool.flush();
  });
}

Case pool_future_round_trip(std::size_t operations) {
  return measure("pool future round trip", operations, [operations] {
    conan::ThreadPoolOptions options;
    options.thread_count = 4;
    conan::ThreadPool pool{std::move(options)};
    std::size_t sink = 0;
    for (std::size_t i = 0; i < operations; i++)
      sink +=
          std::size_t(pool.add_callable<conan::Future>([] { return 1; }).get());
    if (sink != operations)
      std::fputs("benchmark lost a pool task\n", stderr);
  });
}

} // namespace

int main(int argc, char **argv) {
  std::size_t scale = 1;
  if (argc > 1) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const long parsed = std::strtol(argv[1], nullptr, 10);
    if (parsed <= 0) {
      std::fputs("usage: task_handler_benchmark [scale]\n", stderr);
      return EXIT_FAILURE;
    }
    scale = std::size_t(parsed);
  }

  std::printf("TaskHandler %s, %u hardware threads, scale %zu\n\n",
              conan::runtime_version(), std::thread::hardware_concurrency(),
              scale);

  // Once through to page everything in, so the first case measured is not also
  // paying for the first thread the process ever creates.
  static_cast<void>(queued_throughput(10000));

  const std::vector<Case> results{
      queued_throughput(200000 * scale),
      queued_throughput_contended(200000 * scale, 4),
      mixed_priority_throughput(200000 * scale),
      schedule_and_cancel(200000 * scale),
      blocked_round_trip(20000 * scale),
      future_round_trip(20000 * scale),
      queued_pool_throughput(200000 * scale),
      pool_future_round_trip(20000 * scale),
  };
  report(results);
  return EXIT_SUCCESS;
}
