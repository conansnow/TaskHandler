#include "conan/thread_pool.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

using conan::Blocked;
using conan::Future;
using conan::Queued;
using conan::ThreadPool;
using conan::ThreadPoolOptions;
using conan::ThreadPoolQueueFull;
using conan::ThreadPoolStopped;

namespace {

constexpr auto kTimeout = std::chrono::seconds(10);
constexpr int kNum = 6;

// Parks one worker inside a task so later submissions stay queued. On a
// 1-thread pool that is the whole pool; on a larger pool it is one worker.
class Gate {
public:
  explicit Gate(ThreadPool &pool)
      : release_{std::make_shared<std::promise<void>>()} {
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    std::shared_future<void> release_future = release_->get_future().share();
    pool.add_callable([entered, release_future] {
      entered->set_value();
      release_future.wait();
    });
    if (entered_future.wait_for(kTimeout) != std::future_status::ready) {
      throw std::runtime_error(
          "ThreadPool test gate: worker did not enter within timeout");
    }
  }

  ~Gate() { release(); }

  Gate(const Gate &) = delete;
  Gate &operator=(const Gate &) = delete;

  void release() {
    if (!released_) {
      released_ = true;
      release_->set_value();
    }
  }

private:
  std::shared_ptr<std::promise<void>> release_;
  bool released_{false};
};

struct Payload {
  int a{0x11111111};
  int b{0x22222222};
  std::string s{"payload"};
};

void wait_until_not_running(ThreadPool &pool) {
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (pool.running() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

ThreadPoolOptions pool_options(std::size_t thread_count) {
  ThreadPoolOptions options;
  options.thread_count = thread_count;
  return options;
}

#if defined(__linux__)
std::string current_thread_name() {
  char buffer[32]{};
  pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
  return std::string{buffer};
}
#endif

} // namespace

TEST(thread_pool, default_thread_count_is_at_least_one) {
  ThreadPool pool;
  EXPECT_GE(pool.thread_count(), 1U);
  EXPECT_TRUE(pool.running());
}

TEST(thread_pool, explicit_thread_count_is_honoured) {
  ThreadPool pool{pool_options(2)};
  EXPECT_EQ(2U, pool.thread_count());
}

TEST(thread_pool, blocked_runs_before_returning) {
  ThreadPool pool{pool_options(1)};
  int num_tmp{};
  pool.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(thread_pool, queued_runs_asynchronously) {
  ThreadPool pool{pool_options(1)};
  auto done = std::make_shared<std::promise<int>>();
  auto done_future = done->get_future();
  pool.add_callable([done] { done->set_value(kNum); });
  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  EXPECT_EQ(kNum, done_future.get());
}

TEST(thread_pool, future_returns_the_tasks_own_type) {
  ThreadPool pool{pool_options(1)};

  auto future_void = pool.add_callable<Future>([] { /* no result */ });
  ASSERT_EQ(std::future_status::ready, future_void.wait_for(kTimeout));
  future_void.get();

  std::future<int> future_int = pool.add_callable<Future>([] { return kNum; });
  ASSERT_EQ(std::future_status::ready, future_int.wait_for(kTimeout));
  EXPECT_EQ(kNum, future_int.get());

  std::future<std::unique_ptr<int>> future_owned =
      pool.add_callable<Future>([] { return std::make_unique<int>(kNum); });
  ASSERT_EQ(std::future_status::ready, future_owned.wait_for(kTimeout));
  EXPECT_EQ(kNum, *future_owned.get());
}

TEST(thread_pool, queued_accepts_move_only_callables) {
  ThreadPool pool{pool_options(1)};
  auto done = std::make_shared<std::promise<int>>();
  auto done_future = done->get_future();
  auto owned = std::make_unique<int>(kNum);

  pool.add_callable(
      [done, owned = std::move(owned)] { done->set_value(*owned); });

  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  EXPECT_EQ(kNum, done_future.get());
}

TEST(thread_pool, blocked_and_future_accept_move_only_callables) {
  ThreadPool pool{pool_options(1)};
  auto owned = std::make_unique<int>(kNum);
  int value{};
  pool.add_callable<Blocked>(
      [&value, owned = std::move(owned)] { value = *owned; });
  EXPECT_EQ(kNum, value);

  auto owned_future = std::make_unique<int>(kNum);
  std::future<int> future_tmp = pool.add_callable<Future>(
      [owned_future = std::move(owned_future)] { return *owned_future; });
  ASSERT_EQ(std::future_status::ready, future_tmp.wait_for(kTimeout));
  EXPECT_EQ(kNum, future_tmp.get());
}

TEST(thread_pool, two_workers_run_two_tasks_concurrently) {
  ThreadPool pool{pool_options(2)};

  auto entered0 = std::make_shared<std::promise<void>>();
  auto entered1 = std::make_shared<std::promise<void>>();
  auto release = std::make_shared<std::promise<void>>();
  auto entered0_future = entered0->get_future();
  auto entered1_future = entered1->get_future();
  std::shared_future<void> released = release->get_future().share();

  pool.add_callable([entered0, released] {
    entered0->set_value();
    released.wait();
  });
  pool.add_callable([entered1, released] {
    entered1->set_value();
    released.wait();
  });

  ASSERT_EQ(std::future_status::ready, entered0_future.wait_for(kTimeout))
      << "first worker did not enter; the pool is not running concurrently";
  ASSERT_EQ(std::future_status::ready, entered1_future.wait_for(kTimeout))
      << "second worker did not enter; a serial handler would hang here";

  release->set_value();
  pool.flush();
}

TEST(thread_pool, pending_counts_accepted_but_unstarted_tasks) {
  ThreadPool pool{pool_options(1)};
  EXPECT_EQ(0U, pool.pending());

  Gate gate{pool};
  pool.add_callable([] {});
  pool.add_callable([] {});
  EXPECT_EQ(2U, pool.pending());

  gate.release();
  pool.flush();
  EXPECT_EQ(0U, pool.pending());
}

TEST(thread_pool, flush_waits_for_queued_work) {
  ThreadPool pool{pool_options(1)};
  auto ran = std::make_shared<std::atomic_int>(0);
  {
    Gate gate{pool};
    pool.add_callable([ran] { ++*ran; });
    EXPECT_EQ(0, ran->load());
    gate.release();
    pool.flush();
  }
  EXPECT_EQ(1, ran->load());
}

TEST(thread_pool, flush_from_inside_a_task_returns_instead_of_deadlocking) {
  ThreadPool pool{pool_options(1)};
  pool.add_callable<Blocked>([&pool] { pool.flush(); });
}

TEST(thread_pool, blocked_recursion_runs_inline) {
  ThreadPool pool{pool_options(1)};
  auto inner_ran = std::make_shared<std::atomic_bool>(false);
  pool.add_callable<Blocked>([&pool, inner_ran] {
    EXPECT_TRUE(pool.is_worker_thread());
    pool.add_callable<Blocked>([inner_ran] { *inner_ran = true; });
  });
  EXPECT_TRUE(inner_ran->load());
}

TEST(thread_pool, future_recursion_runs_inline) {
  ThreadPool pool{pool_options(1)};
  auto num_tmp = std::make_shared<std::atomic_int>(0);
  pool.add_callable<Blocked>([&pool, num_tmp] {
    auto future_tmp = pool.add_callable<Future>([] { return kNum; });
    *num_tmp = future_tmp.get();
  });
  EXPECT_EQ(kNum, num_tmp->load());
}

TEST(thread_pool, is_worker_thread_is_false_outside_the_pool) {
  ThreadPool pool{pool_options(1)};
  EXPECT_FALSE(pool.is_worker_thread());
  bool on_worker = false;
  pool.add_callable<Blocked>(
      [&pool, &on_worker] { on_worker = pool.is_worker_thread(); });
  EXPECT_TRUE(on_worker);
}

TEST(thread_pool, queued_exception_does_not_kill_the_worker) {
  ThreadPool pool{pool_options(1)};
  pool.add_callable([] { throw std::runtime_error("queued boom"); });

  int num_tmp{};
  pool.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(thread_pool, blocked_propagates_exception) {
  ThreadPool pool{pool_options(1)};
  EXPECT_THROW(pool.add_callable<Blocked>(
                   [] { throw std::runtime_error("blocked boom"); }),
               std::runtime_error);

  int num_tmp{};
  pool.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(thread_pool, future_propagates_exception) {
  ThreadPool pool{pool_options(1)};
  auto future_tmp = pool.add_callable<Future>(
      []() -> int { throw std::runtime_error("future boom"); });
  ASSERT_EQ(std::future_status::ready, future_tmp.wait_for(kTimeout));
  EXPECT_THROW(future_tmp.get(), std::runtime_error);
}

TEST(thread_pool, exception_hook_sees_queued_failures) {
  auto reported = std::make_shared<std::promise<std::string>>();
  auto reported_future = reported->get_future();

  ThreadPoolOptions options;
  options.thread_count = 1;
  options.on_exception = [reported](std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception &caught) {
      reported->set_value(caught.what());
    }
  };
  ThreadPool pool{std::move(options)};

  pool.add_callable([] { throw std::runtime_error("hooked boom"); });

  ASSERT_EQ(std::future_status::ready, reported_future.wait_for(kTimeout));
  EXPECT_EQ(std::string{"hooked boom"}, reported_future.get());
}

TEST(thread_pool, exception_hook_is_not_used_for_blocked_or_future) {
  auto hook_calls = std::make_shared<std::atomic_int>(0);

  ThreadPoolOptions options;
  options.thread_count = 1;
  options.on_exception = [hook_calls](std::exception_ptr) { ++*hook_calls; };
  ThreadPool pool{std::move(options)};

  EXPECT_THROW(
      pool.add_callable<Blocked>([] { throw std::runtime_error("a"); }),
      std::runtime_error);
  auto future_tmp =
      pool.add_callable<Future>([]() -> int { throw std::runtime_error("b"); });
  EXPECT_THROW(future_tmp.get(), std::runtime_error);

  pool.flush();
  EXPECT_EQ(0, hook_calls->load());
}

TEST(thread_pool, exception_hook_failure_does_not_kill_the_worker) {
  ThreadPoolOptions options;
  options.thread_count = 1;
  options.on_exception = [](std::exception_ptr) {
    throw std::runtime_error("hook boom");
  };
  ThreadPool pool{std::move(options)};

  pool.add_callable([] { throw std::runtime_error("queued boom"); });

  int num_tmp{};
  pool.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(thread_pool, max_pending_refuses_work_instead_of_growing) {
  ThreadPoolOptions options;
  options.thread_count = 1;
  options.max_pending = 2;
  ThreadPool pool{std::move(options)};

  Gate gate{pool};
  pool.add_callable([] {});
  pool.add_callable([] {});
  EXPECT_EQ(2U, pool.pending());
  EXPECT_THROW(pool.add_callable([] {}), ThreadPoolQueueFull);
  EXPECT_THROW(pool.add_callable<Future>([] { return kNum; }),
               ThreadPoolQueueFull);
  gate.release();
  pool.flush();
}

TEST(thread_pool, max_pending_does_not_refuse_recursive_submissions) {
  ThreadPoolOptions options;
  options.thread_count = 1;
  options.max_pending = 1;
  ThreadPool pool{std::move(options)};

  auto inner = std::make_shared<std::atomic_int>(0);
  pool.add_callable<Blocked>([&pool, inner] {
    pool.add_callable<Blocked>([inner] { ++*inner; });
    *inner += pool.add_callable<Future>([] { return kNum; }).get();
  });
  EXPECT_EQ(kNum + 1, inner->load());
}

TEST(thread_pool, submission_failures_share_one_base_type) {
  ThreadPoolOptions options;
  options.thread_count = 1;
  options.max_pending = 1;
  ThreadPool pool{std::move(options)};

  Gate gate{pool};
  pool.add_callable([] {});
  EXPECT_THROW(pool.add_callable([] {}), conan::ThreadPoolError);
  gate.release();

  pool.stop();
  EXPECT_THROW(pool.add_callable([] {}), conan::ThreadPoolError);
  EXPECT_THROW(pool.add_callable([] {}), std::runtime_error);
}

TEST(thread_pool, stopped_and_full_are_catchable_by_their_own_type) {
  ThreadPool pool{pool_options(1)};
  pool.stop();
  try {
    pool.add_callable([] {});
    FAIL() << "expected ThreadPoolStopped";
  } catch (const ThreadPoolStopped &) {
  } catch (...) {
    FAIL() << "ThreadPoolStopped was not catchable by type";
  }

  ThreadPoolOptions options;
  options.thread_count = 1;
  options.max_pending = 1;
  ThreadPool bounded{std::move(options)};
  Gate gate{bounded};
  bounded.add_callable([] {});
  try {
    bounded.add_callable([] {});
    FAIL() << "expected ThreadPoolQueueFull";
  } catch (const ThreadPoolQueueFull &) {
  } catch (...) {
    FAIL() << "ThreadPoolQueueFull was not catchable by type";
  }
}

TEST(thread_pool, submitting_to_a_stopped_pool_throws) {
  ThreadPool pool{pool_options(1)};
  pool.stop();
  EXPECT_FALSE(pool.running());
  EXPECT_THROW(pool.add_callable([] {}), ThreadPoolStopped);
  EXPECT_THROW(pool.add_callable<Blocked>([] {}), ThreadPoolStopped);
  EXPECT_THROW(pool.add_callable<Future>([] {}), ThreadPoolStopped);
}

TEST(thread_pool, stop_and_start_are_idempotent_and_reversible) {
  ThreadPool pool{pool_options(2)};
  pool.stop();
  pool.stop();
  EXPECT_FALSE(pool.running());

  pool.start();
  pool.start();
  EXPECT_TRUE(pool.running());

  int num_tmp{};
  pool.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(thread_pool, stop_from_inside_a_task_does_not_terminate) {
  auto ran = std::make_shared<std::atomic_bool>(false);
  ThreadPool pool{pool_options(2)};
  pool.add_callable<Blocked>([&pool, ran] {
    pool.stop();
    *ran = true;
  });
  EXPECT_TRUE(ran->load());
  EXPECT_THROW(pool.add_callable([] {}), ThreadPoolStopped);
}

TEST(thread_pool, start_revives_a_pool_stopped_from_inside_a_task) {
  ThreadPool pool{pool_options(2)};
  pool.add_callable<Blocked>([&pool] { pool.stop(); });

  wait_until_not_running(pool);
  ASSERT_FALSE(pool.running());

  pool.start();
  EXPECT_TRUE(pool.running());

  int num_tmp{};
  pool.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(thread_pool, start_from_inside_a_task_does_not_deadlock_against_stop) {
  ThreadPool pool{pool_options(2)};
  auto returned = std::make_shared<std::promise<void>>();
  auto returned_future = returned->get_future();

  pool.add_callable([&pool, returned] {
    wait_until_not_running(pool);
    pool.start();
    returned->set_value();
  });

  pool.stop();
  ASSERT_EQ(std::future_status::ready, returned_future.wait_for(kTimeout));
  EXPECT_FALSE(pool.running());
}

TEST(thread_pool, shutdown_drains_queued_work) {
  constexpr int kCount = 32;
  auto ran = std::make_shared<std::atomic_int>(0);

  ThreadPool pool{pool_options(1)};
  Gate gate{pool};
  for (int i = 0; i < kCount; i++)
    pool.add_callable([ran] { ++*ran; });
  ASSERT_EQ(0, ran->load()) << "tasks should still be queued behind the gate";

  gate.release();
  pool.stop();
  EXPECT_EQ(kCount, ran->load());
}

TEST(thread_pool, concurrent_producers_all_complete) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 50;
  auto ran = std::make_shared<std::atomic_int>(0);

  ThreadPool pool{pool_options(2)};
  std::vector<std::thread> producers;
  for (int i = 0; i < kProducers; i++)
    producers.emplace_back([&pool, ran] {
      for (int j = 0; j < kPerProducer; j++)
        pool.add_callable<Blocked>([ran] { ++*ran; });
    });
  for (auto &producer : producers)
    producer.join();

  EXPECT_EQ(kProducers * kPerProducer, ran->load());
}

#if defined(__linux__)
TEST(thread_pool, worker_threads_are_named_from_the_prefix) {
  ThreadPoolOptions options;
  options.thread_count = 2;
  options.thread_name_prefix = "tp-named";
  ThreadPool pool{std::move(options)};

  auto name0 = std::make_shared<std::promise<std::string>>();
  auto name1 = std::make_shared<std::promise<std::string>>();
  auto release = std::make_shared<std::promise<void>>();
  auto name0_future = name0->get_future();
  auto name1_future = name1->get_future();
  std::shared_future<void> released = release->get_future().share();

  // Both workers have to be inside a task at once, otherwise one worker can
  // run both submissions and the names would not prove there are two threads.
  pool.add_callable([name0, released] {
    name0->set_value(current_thread_name());
    released.wait();
  });
  pool.add_callable([name1, released] {
    name1->set_value(current_thread_name());
    released.wait();
  });

  ASSERT_EQ(std::future_status::ready, name0_future.wait_for(kTimeout));
  ASSERT_EQ(std::future_status::ready, name1_future.wait_for(kTimeout));
  release->set_value();

  std::vector<std::string> names{name0_future.get(), name1_future.get()};
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::vector<std::string>({"tp-named-0", "tp-named-1"}), names);
}
#endif
