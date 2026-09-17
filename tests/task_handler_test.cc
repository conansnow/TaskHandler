#include "conan/task_handler.h"

#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
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
using conan::TaskHandler;
using conan::TaskHandlerOptions;
using conan::TaskHandlerStopped;
using conan::TaskId;

namespace {

// Generous enough that a loaded machine will not trip it, small enough that a
// genuine hang still fails instead of blocking the suite forever.
constexpr auto kTimeout = std::chrono::seconds(10);

constexpr int kNum = 6;

// Parks the worker thread inside a task so that everything submitted
// afterwards is provably still queued. Without this, ordering and scheduling
// tests have to guess at a sleep duration.
class Gate {
public:
  explicit Gate(TaskHandler &handler)
      : release_{std::make_shared<std::promise<void>>()} {
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    std::shared_future<void> release_future = release_->get_future().share();
    handler.add_callable([entered, release_future] {
      entered->set_value();
      release_future.wait();
    });
    entered_future.wait_for(kTimeout);
  }

  // Releasing from the destructor keeps a failed ASSERT from leaving the
  // worker parked forever and hanging every later test.
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

class Recorder {
public:
  void record(int value) {
    std::lock_guard<std::mutex> guard{mutex_};
    values_.push_back(value);
  }

  std::vector<int> snapshot() const {
    std::lock_guard<std::mutex> guard{mutex_};
    return values_;
  }

private:
  mutable std::mutex mutex_{};
  std::vector<int> values_{};
};

struct Payload {
  int a{0x11111111};
  int b{0x22222222};
  std::string s{"payload"};
};

// The returned task outlives this frame, so it must own its callable. A Future
// task that captured the caller's lambda by reference would read a temporary
// that has already gone away.
std::future<int> submit_from_dead_frame() {
  Payload payload;
  return TaskHandler::instance().add_callable<Future>(
      [payload] { return payload.a + payload.b + int(payload.s.size()); });
}

void clobber_stack() {
  volatile char junk[2048];
  for (std::size_t i = 0; i < sizeof(junk); i++)
    junk[i] = char(0x7F);
}

// Restores the shared handlers even if an assertion returns early, so that a
// failure in a shutdown test cannot cascade into every later test.
struct ReinitGuard {
  ~ReinitGuard() { TaskHandler::init(); }
};

} // namespace

TEST(task_handler, blocked_runs_before_returning) {
  int num_tmp{};
  TaskHandler::instance().add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(task_handler, queued_runs_asynchronously) {
  auto done = std::make_shared<std::promise<int>>();
  auto done_future = done->get_future();
  TaskHandler::instance().add_callable([done] { done->set_value(kNum); });
  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  EXPECT_EQ(kNum, done_future.get());
}

TEST(task_handler, future_returns_the_tasks_own_type) {
  auto future_void =
      TaskHandler::instance().add_callable<Future>([] { /* no result */ });
  ASSERT_EQ(std::future_status::ready, future_void.wait_for(kTimeout));
  future_void.get();

  std::future<int> future_int =
      TaskHandler::instance().add_callable<Future>([] { return kNum; });
  ASSERT_EQ(std::future_status::ready, future_int.wait_for(kTimeout));
  EXPECT_EQ(kNum, future_int.get());

  std::future<std::string> future_string =
      TaskHandler::instance().add_callable<Future>(
          [] { return std::string{"Conan Snow"}; });
  ASSERT_EQ(std::future_status::ready, future_string.wait_for(kTimeout));
  EXPECT_EQ(std::string{"Conan Snow"}, future_string.get());
}

TEST(task_handler, future_supports_move_only_results) {
  std::future<std::unique_ptr<int>> future_tmp =
      TaskHandler::instance().add_callable<Future>(
          [] { return std::make_unique<int>(kNum); });
  ASSERT_EQ(std::future_status::ready, future_tmp.wait_for(kTimeout));
  const std::unique_ptr<int> result = future_tmp.get();
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(kNum, *result);
}

TEST(task_handler, accepts_move_only_callables) {
  auto done = std::make_shared<std::promise<int>>();
  auto done_future = done->get_future();
  auto owned = std::make_unique<int>(kNum);

  TaskHandler::instance().add_callable(
      [done, owned = std::move(owned)] { done->set_value(*owned); });

  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  EXPECT_EQ(kNum, done_future.get());
}

TEST(task_handler, future_callable_outlives_caller_frame) {
  Gate gate{TaskHandler::instance()};
  auto future_tmp = submit_from_dead_frame();
  clobber_stack();
  gate.release();

  ASSERT_EQ(std::future_status::ready, future_tmp.wait_for(kTimeout));
  EXPECT_EQ(0x11111111 + 0x22222222 + 7, future_tmp.get());
}

TEST(task_handler, higher_priority_runs_first_and_ties_keep_fifo_order) {
  auto recorder = std::make_shared<Recorder>();
  Gate gate{TaskHandler::instance()};

  // Every task below is queued while the worker sits in the gate, so execution
  // order is decided purely by priority rather than by timing.
  const std::vector<int> priorities{0, -1, -2, 5, -1, 3, 0, -5};
  for (int priority : priorities)
    TaskHandler::instance().add_callable(
        [recorder, priority] { recorder->record(priority); }, priority);

  auto done = std::make_shared<std::promise<void>>();
  auto done_future = done->get_future();
  TaskHandler::instance().add_callable([done] { done->set_value(); },
                                       std::numeric_limits<int>::min());
  gate.release();

  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  // Descending priority, and stable within a priority: the two -1s and the two
  // 0s keep their submission order.
  EXPECT_EQ(std::vector<int>({5, 3, 0, 0, -1, -1, -2, -5}),
            recorder->snapshot());
}

TEST(task_handler, shared_instances_are_independent) {
  auto future0 = TaskHandler::instance<0>().add_callable<Future>(
      [] { return std::this_thread::get_id(); });
  auto future1 = TaskHandler::instance<1>().add_callable<Future>(
      [] { return std::this_thread::get_id(); });
  auto future2 = TaskHandler::instance<2>().add_callable<Future>(
      [] { return std::this_thread::get_id(); });

  ASSERT_EQ(std::future_status::ready, future0.wait_for(kTimeout));
  ASSERT_EQ(std::future_status::ready, future1.wait_for(kTimeout));
  ASSERT_EQ(std::future_status::ready, future2.wait_for(kTimeout));

  const auto id0 = future0.get();
  const auto id1 = future1.get();
  const auto id2 = future2.get();
  EXPECT_NE(id0, id1);
  EXPECT_NE(id1, id2);
  EXPECT_NE(id0, id2);
}

TEST(task_handler, instance_index_is_checked) {
  EXPECT_EQ(3U, TaskHandler::instance_count());
  EXPECT_THROW(
      static_cast<void>(TaskHandler::instance(TaskHandler::instance_count())),
      std::out_of_range);
}

TEST(task_handler, blocked_recursion_runs_inline) {
  auto inner_ran = std::make_shared<std::atomic_bool>(false);
  TaskHandler::instance().add_callable<Blocked>([inner_ran] {
    EXPECT_TRUE(TaskHandler::instance().is_current_thread());
    TaskHandler::instance().add_callable<Blocked>(
        [inner_ran] { *inner_ran = true; });
  });
  EXPECT_TRUE(inner_ran->load());
}

TEST(task_handler, future_recursion_runs_inline) {
  auto num_tmp = std::make_shared<std::atomic_int>(0);
  TaskHandler::instance().add_callable<Blocked>([num_tmp] {
    auto future_tmp =
        TaskHandler::instance().add_callable<Future>([] { return kNum; });
    *num_tmp = future_tmp.get();
  });
  EXPECT_EQ(kNum, num_tmp->load());
}

TEST(task_handler, blocked_across_two_handlers_does_not_deadlock) {
  auto ran = std::make_shared<std::atomic_bool>(false);
  TaskHandler::instance<0>().add_callable<Blocked>([ran] {
    // A different handler means a different worker, so this genuinely queues
    // rather than running inline.
    TaskHandler::instance<1>().add_callable<Blocked>([ran] { *ran = true; });
  });
  EXPECT_TRUE(ran->load());
}

TEST(task_handler, queued_exception_does_not_kill_the_worker) {
  TaskHandler::instance().add_callable(
      [] { throw std::runtime_error("queued boom"); });

  auto done = std::make_shared<std::promise<void>>();
  auto done_future = done->get_future();
  TaskHandler::instance().add_callable([done] { done->set_value(); });
  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
}

TEST(task_handler, blocked_propagates_exception) {
  EXPECT_THROW(TaskHandler::instance().add_callable<Blocked>(
                   [] { throw std::runtime_error("blocked boom"); }),
               std::runtime_error);

  // The handler must still be usable afterwards.
  int num_tmp{};
  TaskHandler::instance().add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(task_handler, future_propagates_exception) {
  auto future_tmp = TaskHandler::instance().add_callable<Future>(
      []() -> int { throw std::runtime_error("future boom"); });
  ASSERT_EQ(std::future_status::ready, future_tmp.wait_for(kTimeout));
  EXPECT_THROW(future_tmp.get(), std::runtime_error);
}

TEST(task_handler, concurrent_producers_all_complete) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 100;
  auto ran = std::make_shared<std::atomic_int>(0);

  std::vector<std::thread> producers;
  for (int i = 0; i < kProducers; i++)
    producers.emplace_back([ran] {
      for (int j = 0; j < kPerProducer; j++)
        TaskHandler::instance().add_callable<Blocked>([ran] { ++*ran; });
    });
  for (auto &producer : producers)
    producer.join();

  // Blocked returns only once the task has run, so every submission must have
  // completed by the time the producers are joined.
  EXPECT_EQ(kProducers * kPerProducer, ran->load());
}

TEST(task_handler, own_instance_is_usable_and_drains_on_destruction) {
  auto ran = std::make_shared<std::atomic_int>(0);
  {
    TaskHandler handler;
    EXPECT_TRUE(handler.running());
    Gate gate{handler};
    for (int i = 0; i < 16; i++)
      handler.add_callable([ran] { ++*ran; });
    ASSERT_EQ(0, ran->load()) << "tasks should still be queued behind the gate";
    gate.release();
  }
  EXPECT_EQ(16, ran->load());
}

TEST(task_handler, pending_counts_accepted_but_unstarted_tasks) {
  TaskHandler handler;
  Gate gate{handler};

  EXPECT_EQ(0U, handler.pending());
  for (int i = 0; i < 5; i++)
    handler.add_callable([] {});
  EXPECT_EQ(5U, handler.pending());

  gate.release();
  handler.flush();
  EXPECT_EQ(0U, handler.pending());
}

TEST(task_handler, flush_waits_for_queued_work) {
  TaskHandler handler;
  auto ran = std::make_shared<std::atomic_int>(0);

  {
    Gate gate{handler};
    for (int i = 0; i < 32; i++)
      handler.add_callable([ran] { ++*ran; });
  }

  handler.flush();
  EXPECT_EQ(32, ran->load());
}

TEST(task_handler, flush_from_inside_a_task_returns_instead_of_deadlocking) {
  TaskHandler handler;
  auto ran = std::make_shared<std::atomic_bool>(false);
  handler.add_callable<Blocked>([&handler, ran] {
    handler.flush();
    *ran = true;
  });
  EXPECT_TRUE(ran->load());
}

TEST(task_handler, cancel_removes_a_queued_task) {
  TaskHandler handler;
  auto recorder = std::make_shared<Recorder>();
  Gate gate{handler};

  const TaskId first =
      handler.add_callable([recorder] { recorder->record(1); });
  const TaskId second =
      handler.add_callable([recorder] { recorder->record(2); });
  const TaskId third =
      handler.add_callable([recorder] { recorder->record(3); });

  EXPECT_TRUE(handler.cancel(second));
  // Cancelling twice must not remove an unrelated task.
  EXPECT_FALSE(handler.cancel(second));
  EXPECT_TRUE(first.valid());
  EXPECT_TRUE(third.valid());

  gate.release();
  handler.flush();
  EXPECT_EQ(std::vector<int>({1, 3}), recorder->snapshot());
}

TEST(task_handler, cancel_of_a_finished_or_invalid_task_is_false) {
  TaskHandler handler;
  const TaskId id = handler.add_callable([] {});
  handler.flush();
  EXPECT_FALSE(handler.cancel(id));
  EXPECT_FALSE(handler.cancel(TaskId{}));
  EXPECT_FALSE(TaskId{}.valid());
}

TEST(task_handler, scheduled_task_runs_no_earlier_than_its_delay) {
  TaskHandler handler;
  constexpr auto kDelay = std::chrono::milliseconds(60);

  auto done =
      std::make_shared<std::promise<std::chrono::steady_clock::time_point>>();
  auto done_future = done->get_future();
  const auto submitted_at = std::chrono::steady_clock::now();
  handler.add_callable_after(
      kDelay, [done] { done->set_value(std::chrono::steady_clock::now()); });

  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  EXPECT_GE(done_future.get() - submitted_at, kDelay);
}

TEST(task_handler, scheduled_tasks_run_in_deadline_order) {
  TaskHandler handler;
  auto recorder = std::make_shared<Recorder>();
  auto first = std::make_shared<std::promise<void>>();
  auto first_future = first->get_future();
  auto done = std::make_shared<std::promise<void>>();
  auto done_future = done->get_future();

  // Park the worker so every timer is queued before any of them can run.
  // Without that, a slow first wakeup promotes several due timers in one
  // go, they fall into ready_ in submission order, and this test flakes.
  Gate gate{handler};
  const auto base = std::chrono::steady_clock::now();
  constexpr auto kSlot = std::chrono::milliseconds(200);

  handler.add_callable_at(base + 3 * kSlot,
                          [recorder] { recorder->record(3); });
  handler.add_callable_at(base + 1 * kSlot, [recorder, first] {
    recorder->record(1);
    first->set_value();
  });
  handler.add_callable_at(base + 2 * kSlot,
                          [recorder] { recorder->record(2); });
  handler.add_callable_at(base + 4 * kSlot, [done] { done->set_value(); });

  // Only the 200ms task is due. Releasing now means the worker must honour
  // that deadline rather than the fact that 3 was submitted first.
  std::this_thread::sleep_until(base + kSlot + kSlot / 2);
  gate.release();

  ASSERT_EQ(std::future_status::ready, first_future.wait_for(kTimeout));
  EXPECT_EQ(std::vector<int>({1}), recorder->snapshot());

  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  EXPECT_EQ(std::vector<int>({1, 2, 3}), recorder->snapshot());
}

TEST(task_handler, scheduled_task_can_be_cancelled_before_its_deadline) {
  TaskHandler handler;
  auto ran = std::make_shared<std::atomic_bool>(false);

  const TaskId id =
      handler.add_callable_after(std::chrono::hours(1), [ran] { *ran = true; });
  EXPECT_EQ(1U, handler.pending());
  EXPECT_TRUE(handler.cancel(id));
  EXPECT_EQ(0U, handler.pending());
  EXPECT_FALSE(handler.cancel(id));

  handler.flush();
  EXPECT_FALSE(ran->load());
}

TEST(task_handler, undue_scheduled_tasks_are_discarded_on_stop) {
  auto ran = std::make_shared<std::atomic_bool>(false);
  {
    TaskHandler handler;
    handler.add_callable_after(std::chrono::hours(1), [ran] { *ran = true; });
  }
  EXPECT_FALSE(ran->load());
}

// Regression test: undue timers were left in the queue by stop(), so pending()
// kept counting work nothing would run and a later start() resurrected tasks
// whose deadline had passed while the handler was down.
TEST(task_handler, stop_discards_undue_scheduled_tasks) {
  TaskHandler handler;
  auto ran = std::make_shared<std::atomic_bool>(false);

  handler.add_callable_after(std::chrono::milliseconds(20),
                             [ran] { *ran = true; });
  EXPECT_EQ(1U, handler.pending());
  handler.stop();
  EXPECT_EQ(0U, handler.pending());

  handler.start();
  handler.add_callable<Blocked>([] {});
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  handler.flush();
  EXPECT_FALSE(ran->load());
}

TEST(task_handler, max_pending_refuses_work_instead_of_growing) {
  TaskHandlerOptions options;
  options.max_pending = 4;
  TaskHandler handler{std::move(options)};

  Gate gate{handler};
  for (int i = 0; i < 4; i++)
    handler.add_callable([] {});
  EXPECT_EQ(4U, handler.pending());

  EXPECT_THROW(handler.add_callable([] {}), conan::TaskHandlerQueueFull);
  EXPECT_THROW(handler.add_callable<Future>([] { return kNum; }),
               conan::TaskHandlerQueueFull);
  EXPECT_THROW(handler.add_callable_after(std::chrono::hours(1), [] {}),
               conan::TaskHandlerQueueFull);
  // Refusing a submission must not disturb the work already accepted.
  EXPECT_EQ(4U, handler.pending());

  gate.release();
  handler.flush();
  handler.add_callable<Blocked>([] {});
}

TEST(task_handler, max_pending_does_not_refuse_recursive_submissions) {
  TaskHandlerOptions options;
  options.max_pending = 1;
  TaskHandler handler{std::move(options)};

  auto inner = std::make_shared<std::atomic_int>(0);
  // Blocked and Future run inline on the worker thread, so a task that submits
  // more work cannot be refused by a queue it is not going to be put in.
  handler.add_callable<Blocked>([&handler, inner] {
    handler.add_callable<Blocked>([inner] { ++*inner; });
    *inner += handler.add_callable<Future>([] { return kNum; }).get();
  });
  EXPECT_EQ(kNum + 1, inner->load());
}

TEST(task_handler, submission_failures_share_one_base_type) {
  TaskHandlerOptions options;
  options.max_pending = 1;
  TaskHandler handler{std::move(options)};

  Gate gate{handler};
  handler.add_callable([] {});
  EXPECT_THROW(handler.add_callable([] {}), conan::TaskHandlerError);
  gate.release();

  handler.stop();
  EXPECT_THROW(handler.add_callable([] {}), conan::TaskHandlerError);
  EXPECT_THROW(handler.add_callable([] {}), std::runtime_error);
}

TEST(task_handler, runtime_version_matches_the_header) {
  EXPECT_EQ(std::string{TASKHANDLER_VERSION_STRING},
            std::string{conan::runtime_version()});
  EXPECT_EQ(TASKHANDLER_VERSION_MAJOR * 10000 +
                TASKHANDLER_VERSION_MINOR * 100 + TASKHANDLER_VERSION_PATCH,
            TASKHANDLER_VERSION);
}

TEST(task_handler, exception_hook_sees_queued_failures) {
  auto reported = std::make_shared<std::promise<std::string>>();
  auto reported_future = reported->get_future();

  TaskHandlerOptions options;
  options.on_exception = [reported](std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception &caught) {
      reported->set_value(caught.what());
    }
  };
  TaskHandler handler{std::move(options)};

  handler.add_callable([] { throw std::runtime_error("hooked boom"); });

  ASSERT_EQ(std::future_status::ready, reported_future.wait_for(kTimeout));
  EXPECT_EQ(std::string{"hooked boom"}, reported_future.get());
}

TEST(task_handler, exception_hook_is_not_used_for_blocked_or_future) {
  auto hook_calls = std::make_shared<std::atomic_int>(0);

  TaskHandlerOptions options;
  options.on_exception = [hook_calls](std::exception_ptr) { ++*hook_calls; };
  TaskHandler handler{std::move(options)};

  EXPECT_THROW(
      handler.add_callable<Blocked>([] { throw std::runtime_error("a"); }),
      std::runtime_error);
  auto future_tmp = handler.add_callable<Future>(
      []() -> int { throw std::runtime_error("b"); });
  EXPECT_THROW(future_tmp.get(), std::runtime_error);

  handler.flush();
  EXPECT_EQ(0, hook_calls->load());
}

TEST(task_handler, submitting_to_a_stopped_handler_throws) {
  TaskHandler handler;
  handler.stop();
  EXPECT_FALSE(handler.running());
  EXPECT_THROW(handler.add_callable([] {}), TaskHandlerStopped);
  EXPECT_THROW(handler.add_callable<Blocked>([] {}), TaskHandlerStopped);
  EXPECT_THROW(handler.add_callable<Future>([] {}), TaskHandlerStopped);
  EXPECT_THROW(handler.add_callable_after(std::chrono::seconds(1), [] {}),
               TaskHandlerStopped);
}

TEST(task_handler, stop_and_start_are_idempotent_and_reversible) {
  TaskHandler handler;
  handler.stop();
  handler.stop();
  EXPECT_FALSE(handler.running());

  handler.start();
  handler.start();
  EXPECT_TRUE(handler.running());

  int num_tmp{};
  handler.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

// Regression test: stop() used to call std::thread::join() unconditionally, so
// tearing a handler down from one of its own tasks threw
// std::system_error("Resource deadlock avoided") and terminated the process.
TEST(task_handler, stop_from_inside_a_task_does_not_terminate) {
  auto ran = std::make_shared<std::atomic_bool>(false);
  TaskHandler handler;
  handler.add_callable<Blocked>([&handler, ran] {
    handler.stop();
    *ran = true;
  });
  EXPECT_TRUE(ran->load());
  EXPECT_THROW(handler.add_callable([] {}), TaskHandlerStopped);
}

// Regression test: start() returned early whenever the thread object was still
// joinable, which it always is after a stop() requested from inside a task,
// since a worker cannot join itself. The handler could never be revived.
TEST(task_handler, start_revives_a_handler_stopped_from_inside_a_task) {
  TaskHandler handler;
  handler.add_callable<Blocked>([&handler] { handler.stop(); });

  // The worker finishes unwinding out of the task on its own, so wait for the
  // stop to take effect rather than assuming it already has.
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (handler.running() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_FALSE(handler.running());

  // start() joins the exiting worker if it has not finished yet, then
  // spawns a new one. running() being false is not enough on its own:
  // the stop flag is set before the thread actually leaves.
  handler.start();
  EXPECT_TRUE(handler.running());

  int num_tmp{};
  handler.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

// Regression test: start() from inside a task took the lifecycle mutex, which a
// concurrent stop() holds while waiting to join that very worker. Both sides
// blocked forever.
TEST(task_handler, start_from_inside_a_task_does_not_deadlock_against_stop) {
  TaskHandler handler;
  auto returned = std::make_shared<std::promise<void>>();
  auto returned_future = returned->get_future();

  handler.add_callable([&handler, returned] {
    // Long enough for the stop() below to be waiting in join() by the time
    // start() is called, which is the case that used to hang.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    handler.start();
    returned->set_value();
  });

  handler.stop();
  ASSERT_EQ(std::future_status::ready, returned_future.wait_for(kTimeout));
  // The stop request stands: taking it back from inside a task is what would
  // leave the stop() above waiting on a worker that never exits.
  EXPECT_FALSE(handler.running());
}

TEST(task_handler, uninit_from_inside_a_task_does_not_terminate) {
  ReinitGuard reinit_guard;
  auto ran = std::make_shared<std::atomic_bool>(false);
  TaskHandler::instance().add_callable<Blocked>([ran] {
    TaskHandler::uninit();
    *ran = true;
  });
  EXPECT_TRUE(ran->load());
}

// Regression test: shutdown used to drop and leak whatever was still queued,
// which also left add_callable<Blocked> callers waiting on a promise that
// would never be fulfilled.
TEST(task_handler, shutdown_drains_queued_work) {
  constexpr int kCount = 32;
  auto ran = std::make_shared<std::atomic_int>(0);

  ReinitGuard reinit_guard;
  Gate gate{TaskHandler::instance()};
  for (int i = 0; i < kCount; i++)
    TaskHandler::instance().add_callable([ran] { ++*ran; });
  ASSERT_EQ(0, ran->load()) << "tasks should still be queued behind the gate";

  gate.release();
  TaskHandler::uninit();
  EXPECT_EQ(kCount, ran->load());
}

TEST(task_handler, init_and_uninit_are_idempotent) {
  ReinitGuard reinit_guard;
  TaskHandler::init();
  TaskHandler::init();
  TaskHandler::uninit();
  TaskHandler::uninit();
  TaskHandler::init();

  int num_tmp{};
  TaskHandler::instance().add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

// Shared handlers are deliberately never freed, so a reference taken before a
// shutdown stays valid instead of dangling.
TEST(task_handler, instance_reference_survives_uninit) {
  ReinitGuard reinit_guard;
  TaskHandler &handler = TaskHandler::instance();
  TaskHandler::uninit();
  EXPECT_FALSE(handler.running());

  TaskHandler::init();
  EXPECT_TRUE(handler.running());
  EXPECT_EQ(&handler, &TaskHandler::instance());

  int num_tmp{};
  handler.add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

#if defined(__linux__)
TEST(task_handler, worker_thread_is_named) {
  TaskHandlerOptions options;
  options.thread_name = "th-named";
  TaskHandler handler{std::move(options)};

  auto name = handler.add_callable<Future>([] {
    char buffer[32]{};
    pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
    return std::string{buffer};
  });
  ASSERT_EQ(std::future_status::ready, name.wait_for(kTimeout));
  EXPECT_EQ(std::string{"th-named"}, name.get());
}

TEST(task_handler, shared_handlers_get_distinct_thread_names) {
  auto name_of = [](TaskHandler &handler) {
    return handler
        .add_callable<Future>([] {
          char buffer[32]{};
          pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
          return std::string{buffer};
        })
        .get();
  };
  EXPECT_EQ(std::string{"conan-task-0"}, name_of(TaskHandler::instance<0>()));
  EXPECT_EQ(std::string{"conan-task-1"}, name_of(TaskHandler::instance<1>()));
  EXPECT_EQ(std::string{"conan-task-2"}, name_of(TaskHandler::instance<2>()));
}
#endif
