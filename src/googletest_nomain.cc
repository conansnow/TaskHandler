#include "task_handler.h"
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace conan;

namespace {

// Generous enough that a loaded machine will not trip it, small enough that a
// genuine hang still fails instead of blocking the suite forever.
constexpr auto kTimeout = std::chrono::seconds(10);

constexpr int kNum = 6;

// State shared with a task is held by shared_ptr and captured by value: a task
// that runs late must never write through a reference to a dead test frame.
template <typename T> using Shared = std::shared_ptr<T>;

// Parks the worker thread inside a task so that everything submitted afterwards
// is provably still queued. Without this, ordering and scheduling tests have to
// guess at a sleep duration.
class Gate {
public:
  explicit Gate(TaskHandler *handler)
      : release_{std::make_shared<std::promise<void>>()} {
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    std::shared_future<void> release_future = release_->get_future().share();
    handler->add_callable([entered, release_future] {
      entered->set_value();
      release_future.wait();
    });
    entered_future.wait_for(kTimeout);
  }

  // Releasing from the destructor keeps a failed ASSERT from leaving the worker
  // parked forever and hanging every later test.
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
  Shared<std::promise<void>> release_;
  bool released_{false};
};

class Recorder {
public:
  void record(int value) {
    std::lock_guard<std::mutex> guard(mutex_);
    values_.push_back(value);
  }

  std::vector<int> snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
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

// The returned task outlives this frame, so it must own its callable. Before
// the fix, the worker read the lambda temporary after the frame was gone.
std::future<std::any> submit_from_dead_frame() {
  Payload payload;
  return TaskHandler::get_instance()->add_callable<Future>(
      [payload] { return payload.a + payload.b + int(payload.s.size()); });
}

void clobber_stack() {
  volatile char junk[2048];
  for (size_t i = 0; i < sizeof(junk); i++)
    junk[i] = char(0x7F);
}

#ifdef TASKHANDLER_COMPILED_LIB
// Compiled-lib mode requires an explicit init(). Doing it here rather than in a
// test body keeps the suite independent of test order, --gtest_filter and
// --gtest_shuffle.
class TaskHandlerEnvironment : public ::testing::Environment {
public:
  void SetUp() override { TaskHandler::init(); }
  void TearDown() override { TaskHandler::uninit(); }
};

const auto *const task_handler_environment =
    ::testing::AddGlobalTestEnvironment(new TaskHandlerEnvironment);

// Restores the handlers even if an assertion returns early, so that a failure
// here cannot cascade into every later test.
struct ReinitGuard {
  ~ReinitGuard() { TaskHandler::init(); }
};
#endif

} // namespace

TEST(test_example, test1) { EXPECT_EQ(1, 1); }

TEST(task_handler, test_blocked) {
  int num_tmp{};
  TaskHandler::get_instance()->add_callable<Blocked>([&] { num_tmp = kNum; });
  EXPECT_EQ(kNum, num_tmp);
}

TEST(task_handler, test_queued) {
  auto done = std::make_shared<std::promise<int>>();
  auto done_future = done->get_future();
  TaskHandler::get_instance()->add_callable([done] { done->set_value(kNum); });
  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  EXPECT_EQ(kNum, done_future.get());
}

TEST(task_handler, test_future) {
  auto num_tmp = std::make_shared<std::atomic_int>(0);
  auto future_void = TaskHandler::get_instance()->add_callable<Future>(
      [num_tmp] { *num_tmp = kNum; });
  ASSERT_EQ(std::future_status::ready, future_void.wait_for(kTimeout));
  future_void.get();
  EXPECT_EQ(kNum, num_tmp->load());

  auto future_int =
      TaskHandler::get_instance()->add_callable<Future>([] { return kNum; });
  ASSERT_EQ(std::future_status::ready, future_int.wait_for(kTimeout));
  EXPECT_EQ(kNum, std::any_cast<int>(future_int.get()));

  auto future_string = TaskHandler::get_instance()->add_callable<Future>(
      [] { return std::string{"Conan Snow"}; });
  ASSERT_EQ(std::future_status::ready, future_string.wait_for(kTimeout));
  EXPECT_EQ(std::string{"Conan Snow"},
            std::any_cast<std::string>(future_string.get()));
}

// Regression test: the Future overload used to capture the caller's callable by
// reference, so a temporary lambda was read after its frame had gone away.
TEST(task_handler, test_future_callable_outlives_caller_frame) {
  Gate gate{TaskHandler::get_instance()};
  auto future_tmp = submit_from_dead_frame();
  clobber_stack();
  gate.release();

  ASSERT_EQ(std::future_status::ready, future_tmp.wait_for(kTimeout));
  EXPECT_EQ(0x11111111 + 0x22222222 + 7, std::any_cast<int>(future_tmp.get()));
}

TEST(task_handler, test_priority) {
  auto recorder = std::make_shared<Recorder>();
  Gate gate{TaskHandler::get_instance()};

  // Every task below is queued while the worker sits in the gate, so execution
  // order is decided purely by priority rather than by timing.
  const std::vector<int> priorities{0, -1, -2, 5, -1, 3, 0, -5};
  for (int priority : priorities)
    TaskHandler::get_instance()->add_callable(
        [recorder, priority] { recorder->record(priority); }, priority);

  auto done = std::make_shared<std::promise<void>>();
  auto done_future = done->get_future();
  TaskHandler::get_instance()->add_callable([done] { done->set_value(); },
                                            std::numeric_limits<int>::max());
  gate.release();

  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
  // Ascending priority, and stable within a priority: the two -1s and the two
  // 0s keep their submission order.
  EXPECT_EQ(std::vector<int>({-5, -2, -1, -1, 0, 0, 3, 5}),
            recorder->snapshot());
}

TEST(task_handler, test_multithread) {
  auto future0 =
      TaskHandler::get_instance<0>()->add_callable<Future>([] { return kNum; });
  auto future1 = TaskHandler::get_instance<1>()->add_callable<Future>(
      [] { return kNum + 1; });
  auto future2 = TaskHandler::get_instance<2>()->add_callable<Future>(
      [] { return kNum + 2; });

  ASSERT_EQ(std::future_status::ready, future0.wait_for(kTimeout));
  ASSERT_EQ(std::future_status::ready, future1.wait_for(kTimeout));
  ASSERT_EQ(std::future_status::ready, future2.wait_for(kTimeout));
  EXPECT_EQ(kNum, std::any_cast<int>(future0.get()));
  EXPECT_EQ(kNum + 1, std::any_cast<int>(future1.get()));
  EXPECT_EQ(kNum + 2, std::any_cast<int>(future2.get()));
}

TEST(task_handler, test_blocked_recursion) {
  auto inner_ran = std::make_shared<std::atomic_bool>(false);
  TaskHandler::get_instance()->add_callable<Blocked>([inner_ran] {
    TaskHandler::get_instance()->add_callable<Blocked>(
        [inner_ran] { *inner_ran = true; });
  });
  EXPECT_TRUE(inner_ran->load());
}

TEST(task_handler, test_future_recursion) {
  auto num_tmp = std::make_shared<std::atomic_int>(0);
  TaskHandler::get_instance()->add_callable<Blocked>([num_tmp] {
    auto future_tmp =
        TaskHandler::get_instance()->add_callable<Future>([] { return kNum; });
    *num_tmp = std::any_cast<int>(future_tmp.get());
  });
  EXPECT_EQ(kNum, num_tmp->load());
}

// Regression test: an exception escaping a task used to propagate out of the
// worker thread and call std::terminate.
TEST(task_handler, test_queued_exception_does_not_kill_worker) {
  TaskHandler::get_instance()->add_callable(
      [] { throw std::runtime_error("queued boom"); });

  auto done = std::make_shared<std::promise<void>>();
  auto done_future = done->get_future();
  TaskHandler::get_instance()->add_callable([done] { done->set_value(); });
  ASSERT_EQ(std::future_status::ready, done_future.wait_for(kTimeout));
}

TEST(task_handler, test_blocked_propagates_exception) {
  EXPECT_THROW(TaskHandler::get_instance()->add_callable<Blocked>(
                   [] { throw std::runtime_error("blocked boom"); }),
               std::runtime_error);

  // The handler must still be usable afterwards.
  int num_tmp{};
  TaskHandler::get_instance()->add_callable<Blocked>([&] { num_tmp = 6; });
  EXPECT_EQ(6, num_tmp);
}

TEST(task_handler, test_future_propagates_exception) {
  auto future_tmp = TaskHandler::get_instance()->add_callable<Future>(
      []() -> int { throw std::runtime_error("future boom"); });
  ASSERT_EQ(std::future_status::ready, future_tmp.wait_for(kTimeout));
  EXPECT_THROW(future_tmp.get(), std::runtime_error);
}

TEST(task_handler, test_concurrent_producers) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 100;
  auto ran = std::make_shared<std::atomic_int>(0);

  std::vector<std::thread> producers;
  for (int i = 0; i < kProducers; i++)
    producers.emplace_back([ran] {
      for (int j = 0; j < kPerProducer; j++)
        TaskHandler::get_instance()->add_callable<Blocked>([ran] { ++*ran; });
    });
  for (auto &producer : producers)
    producer.join();

  // Blocked returns only once the task has run, so every submission must have
  // completed by the time the producers are joined.
  EXPECT_EQ(kProducers * kPerProducer, ran->load());
}

#ifdef TASKHANDLER_COMPILED_LIB
// Regression test: shutdown used to drop and leak whatever was still queued,
// which also left add_callable<Blocked> callers waiting on a promise that would
// never be fulfilled.
TEST(task_handler, test_shutdown_drains_queued_work) {
  constexpr int kCount = 32;
  auto ran = std::make_shared<std::atomic_int>(0);

  ReinitGuard reinit_guard;
  Gate gate{TaskHandler::get_instance()};
  for (int i = 0; i < kCount; i++)
    TaskHandler::get_instance()->add_callable([ran] { ++*ran; });
  ASSERT_EQ(0, ran->load()) << "tasks should still be queued behind the gate";

  gate.release();
  TaskHandler::uninit();
  EXPECT_EQ(kCount, ran->load());
}

TEST(task_handler, test_init_uninit_are_idempotent) {
  ReinitGuard reinit_guard;
  EXPECT_EQ(0, TaskHandler::init());
  EXPECT_EQ(0, TaskHandler::init());
  EXPECT_EQ(0, TaskHandler::uninit());
  EXPECT_EQ(0, TaskHandler::uninit());
  EXPECT_EQ(0, TaskHandler::init());

  int num_tmp{};
  TaskHandler::get_instance()->add_callable<Blocked>([&] { num_tmp = 6; });
  EXPECT_EQ(6, num_tmp);
}
#endif
