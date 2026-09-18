#ifndef CONAN_THREAD_POOL_H_
#define CONAN_THREAD_POOL_H_

// ThreadPool is the type for work that is allowed to run concurrently.
// TaskHandler stays one worker per instance: that is what makes handler-owned
// state lock-free. A pool would break that property, so it is a sibling rather
// than a mode of the handler. Bounce results back onto a handler when they
// must touch that state.
//
// Include this header for the pool. It pulls in conan/task_handler.h for the
// shared policy tags, visibility macros and move-only task storage.

#include "conan/task_handler.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace conan {

// Base of every exception ThreadPool itself throws, so a caller that only
// wants to know a submission was refused can catch one type. Distinct from
// TaskHandlerError: the two types refuse work for different objects.
class TASKHANDLER_VISIBLE ThreadPoolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class TASKHANDLER_VISIBLE ThreadPoolStopped : public ThreadPoolError {
public:
  ThreadPoolStopped() : ThreadPoolError{"conan::ThreadPool is stopped"} {}
};

class TASKHANDLER_VISIBLE ThreadPoolQueueFull : public ThreadPoolError {
public:
  ThreadPoolQueueFull() : ThreadPoolError{"conan::ThreadPool queue is full"} {}
};

struct ThreadPoolOptions {
  // Number of worker threads. Zero, the default, means
  // max(1, hardware_concurrency()). There is no zero-thread pool.
  std::size_t thread_count{0};

  // Workers are named "{prefix}-{index}" when this is non-empty, for
  // debuggers and profilers. Linux truncates the result to 15 characters.
  std::string thread_name_prefix{};

  // Invoked on a worker thread when a Queued task throws. Queued tasks have
  // no future to report a failure through, so without this hook the exception
  // is discarded. Blocked and Future tasks deliver their exceptions to the
  // caller and never reach this hook. An exception thrown by the hook itself
  // is ignored.
  //
  // A pool may invoke this from several workers at once. The callable must
  // be safe for that, or the caller must synchronize it; the library will
  // not. Serializing the hook inside the pool would stall workers, and a
  // hook that called back into the pool could deadlock.
  std::function<void(std::exception_ptr)> on_exception{};

  // Largest number of tasks the pool will hold at once, counting queued work
  // only, not tasks already running. Zero, the default, means no limit.
  //
  // A limit is backpressure: once it is reached, submitting throws
  // ThreadPoolQueueFull instead of letting a producer that outruns the workers
  // grow the queue until the process runs out of memory. Recursive Blocked
  // and Future submissions from a worker thread run inline without queueing,
  // so they are unaffected by the limit and cannot be refused by it.
  std::size_t max_pending{0};
};

class TASKHANDLER_API ThreadPool {
public:
  // Constructing a pool starts its workers; destroying it drains the queue
  // and joins. The destructor swallows errors from join() rather than
  // throwing, because a destructor must not.
  explicit ThreadPool(ThreadPoolOptions options = {});
  ~ThreadPool() noexcept;

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;
  ThreadPool(ThreadPool &&) = delete;
  ThreadPool &operator=(ThreadPool &&) = delete;

  // Submits `callable` and returns immediately. Exceptions escaping the task
  // are reported to ThreadPoolOptions::on_exception and otherwise discarded.
  // There is no TaskId: a pool does not cancel queued work. Workers share
  // one FIFO queue; a single worker runs queued tasks in submission order.
  // Tasks on different workers may overlap.
  template <detail::QueuedPolicy T = Queued, detail::TaskCallable C>
  void add_callable(C &&callable);

  // Submits `callable` and blocks until it has run, rethrowing whatever it
  // threw. Called from one of this pool's worker threads it runs the task
  // inline instead of deadlocking a 1-thread pool, which is what makes
  // recursive use safe. If every worker is blocked waiting for more pool
  // work, the pool still deadlocks: inline-on-worker does not fix that.
  template <detail::BlockedPolicy T, detail::TaskCallable C>
  void add_callable(C &&callable);

  // Submits `callable` and returns a future for its result. The future is
  // typed: a task returning int yields std::future<int>, and move-only results
  // such as std::unique_ptr work. Called from a worker thread it runs inline,
  // for the same reason Blocked does.
  template <detail::FuturePolicy T, detail::TaskCallable C>
  std::future<detail::TaskResultT<C>> add_callable(C &&callable);

  // Tasks accepted but not yet started. Excludes tasks currently running.
  [[nodiscard]] std::size_t pending() const;

  // Blocks until every task submitted so far has finished and the workers
  // are idle. Returns immediately when called from a worker thread, where
  // waiting could only deadlock.
  void flush();

  [[nodiscard]] bool is_worker_thread() const;
  [[nodiscard]] bool running() const;
  [[nodiscard]] std::size_t thread_count() const noexcept;

  // Starts the workers if they are not running. Works after a stop(),
  // including one that was requested from inside a task. Idempotent.
  //
  // Calling start() from inside one of this pool's own tasks does nothing:
  // that worker is running by definition, and a stop request already made
  // can only be taken back from another thread, because a stop() that is
  // waiting to join this worker would otherwise never be let go.
  void start();

  // Runs everything already queued, then stops the workers and joins them.
  // Idempotent.
  //
  // Calling stop() from inside a task only records the request and returns:
  // a worker cannot join itself, so it finishes draining and exits on its
  // own. The other workers drain and exit too.
  void stop();

private:
  static std::size_t resolved_thread_count(std::size_t requested) noexcept;

  void submit(detail::Task task);
  void ensure_accepting() const;
  void request_stop();
  void run_worker(std::size_t index);
  void run_one_task(std::unique_lock<std::mutex> &lock);
  void report_exception(std::exception_ptr error) const noexcept;
  void destroy_user_code_nothrow(auto &&destroy) const noexcept;
  void apply_thread_name(std::size_t index) const noexcept;

  ThreadPoolOptions options_{};
  std::size_t thread_count_{1};

  mutable std::mutex mutex_{};
  std::condition_variable work_cv_{};
  std::condition_variable idle_cv_{};

  std::deque<detail::Task> queue_{};

  bool stop_requested_{false};
  std::size_t busy_{0};
  std::vector<std::thread::id> worker_ids_{};

  // Guards start()/stop() against each other so that `threads_` is only ever
  // touched by one caller at a time. Lock order is lifecycle_mutex_ then
  // mutex_, never the other way round.
  std::mutex lifecycle_mutex_{};
  std::vector<std::thread> threads_{};
};

template <detail::QueuedPolicy T, detail::TaskCallable C>
void ThreadPool::add_callable(C &&callable) {
  submit(detail::Task{std::forward<C>(callable)});
}

template <detail::BlockedPolicy T, detail::TaskCallable C>
// The callable is deliberately not forwarded: this overload blocks until the
// task has run, so the task borrows the caller's object instead of owning it.
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
void ThreadPool::add_callable(C &&callable) {
  if (is_worker_thread()) {
    std::invoke(callable);
    return;
  }

  std::promise<void> promise;
  std::future<void> future = promise.get_future();

  // Capturing by reference is safe here, and only here, because this function
  // does not return until the task has run to completion. stop() drains the
  // queue, so a Blocked task that was already accepted still runs and fulfils
  // the promise.
  submit(detail::Task{[&callable, &promise] {
    try {
      std::invoke(callable);
      promise.set_value();
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
  }});

  future.get();
}

template <detail::FuturePolicy T, detail::TaskCallable C>
std::future<detail::TaskResultT<C>> ThreadPool::add_callable(C &&callable) {
  auto work = detail::package_work(std::forward<C>(callable));

  if (is_worker_thread()) {
    work.packaged();
    return std::move(work.future);
  }

  submit(detail::Task{
      [packaged = std::move(work.packaged)]() mutable { packaged(); }});
  return std::move(work.future);
}

} // namespace conan

#ifdef TASKHANDLER_HEADER_ONLY
#include "conan/detail/thread_pool-inl.h"
#endif

#endif // CONAN_THREAD_POOL_H_
