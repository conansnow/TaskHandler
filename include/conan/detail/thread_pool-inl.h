#ifndef CONAN_DETAIL_THREAD_POOL_INL_H_
#define CONAN_DETAIL_THREAD_POOL_INL_H_

// Out-of-line definitions, and not part of the public interface: include
// conan/thread_pool.h instead. In header-only builds this file is pulled in by
// that header and everything below is `inline`; in compiled-library builds it
// is compiled exactly once, into src/thread_pool.cc.

#ifndef TASKHANDLER_HEADER_ONLY
#include "conan/thread_pool.h"
#endif

#include "conan/detail/thread_name.h"

#include <algorithm>
#include <format>
#include <utility>

namespace conan {

TASKHANDLER_INLINE std::size_t
ThreadPool::resolved_thread_count(std::size_t requested) noexcept {
  if (requested != 0)
    return requested;
  const unsigned hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? std::size_t{1} : std::size_t{hardware};
}

TASKHANDLER_INLINE ThreadPool::ThreadPool(ThreadPoolOptions options)
    : options_{std::move(options)},
      thread_count_{resolved_thread_count(options_.thread_count)} {
  start();
}

TASKHANDLER_INLINE ThreadPool::~ThreadPool() noexcept {
  try {
    stop();
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // std::thread::join can throw system_error. A destructor must not.
  }
}

TASKHANDLER_INLINE void ThreadPool::start() {
  // Taking the lifecycle mutex from inside a task would deadlock against a
  // stop() that is holding it while waiting to join this very worker. Clearing
  // the stop request instead is no better: the workers would stop exiting and
  // that stop() would wait forever.
  if (is_worker_thread())
    return;

  std::lock_guard<std::mutex> lifecycle_guard{lifecycle_mutex_};
  if (!threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (!stop_requested_ && worker_ids_.size() == thread_count_)
        return;
    }
    // A stop() requested from inside a task returns without joining, so the
    // thread objects outlive the workers they owned. Join waits that out,
    // then we spawn below.
    for (std::thread &thread : threads_) {
      if (thread.joinable())
        thread.join();
    }
    threads_.clear();
  }

  std::unique_lock<std::mutex> lock{mutex_};
  stop_requested_ = false;
  worker_ids_.clear();
  try {
    // mutex_ stays held across spawn so a concurrent submit cannot accept
    // work into a pool that then fails to start and leaves nobody to run it.
    // reserve is inside the try for the same reason: a throwing allocation
    // used to leave stop_requested_ false with threads_ empty.
    threads_.reserve(thread_count_);
    for (std::size_t index = 0; index < thread_count_; index++)
      threads_.emplace_back([this, index] { run_worker(index); });

    // Letting the workers publish their own ids closes the window in which a
    // task could already be running while is_worker_thread() still answers
    // false.
    idle_cv_.wait(lock, [this] { return worker_ids_.size() == thread_count_; });
  } catch (...) {
    // Restore a stopped pool rather than one that accepts work with no
    // workers. Join before rethrowing so a constructor failure cannot
    // destroy joinable std::thread objects (that is std::terminate).
    stop_requested_ = true;
    work_cv_.notify_all();
    idle_cv_.notify_all();
    if (lock.owns_lock())
      lock.unlock();
    for (std::thread &thread : threads_) {
      if (thread.joinable())
        thread.join();
    }
    threads_.clear();
    throw;
  }
}

TASKHANDLER_INLINE void ThreadPool::stop() {
  if (is_worker_thread()) {
    request_stop();
    return;
  }

  std::lock_guard<std::mutex> lifecycle_guard{lifecycle_mutex_};
  request_stop();
  for (std::thread &thread : threads_) {
    if (thread.joinable())
      thread.join();
  }
  threads_.clear();
}

TASKHANDLER_INLINE void ThreadPool::request_stop() {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    stop_requested_ = true;
  }
  work_cv_.notify_all();
  idle_cv_.notify_all();
}

TASKHANDLER_INLINE void ThreadPool::run_worker(std::size_t index) {
  apply_thread_name(index);

  std::unique_lock<std::mutex> lock{mutex_};
  worker_ids_.push_back(std::this_thread::get_id());
  idle_cv_.notify_all();

  for (;;) {
    if (!queue_.empty()) {
      run_one_task(lock);
      continue;
    }

    // An empty queue is checked before the stop flag so that work accepted
    // before the stop request still runs, which is what keeps
    // add_callable<Blocked> callers from waiting on a promise nobody fulfils.
    if (stop_requested_)
      break;

    work_cv_.wait(lock);
  }

  // Cleared only after this worker has finished running user code, so
  // is_worker_thread() stays true for a destructor that calls back in.
  std::erase(worker_ids_, std::this_thread::get_id());
  idle_cv_.notify_all();
}

TASKHANDLER_INLINE void
ThreadPool::run_one_task(std::unique_lock<std::mutex> &lock) {
  detail::Task task = std::move(queue_.front());
  queue_.pop_front();
  ++busy_;

  lock.unlock();
  try {
    task();
  } catch (...) {
    report_exception(std::current_exception());
  }
  destroy_user_code_nothrow([&task] { task = nullptr; });
  lock.lock();

  --busy_;
  idle_cv_.notify_all();
}

TASKHANDLER_INLINE void ThreadPool::ensure_accepting() const {
  if (stop_requested_)
    throw ThreadPoolStopped{};
  if (options_.max_pending != 0 && queue_.size() >= options_.max_pending)
    throw ThreadPoolQueueFull{};
}

TASKHANDLER_INLINE void ThreadPool::submit(detail::Task task) {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    ensure_accepting();
    queue_.push_back(std::move(task));
  }
  work_cv_.notify_one();
}

TASKHANDLER_INLINE std::size_t ThreadPool::pending() const {
  std::lock_guard<std::mutex> lock{mutex_};
  return queue_.size();
}

TASKHANDLER_INLINE void ThreadPool::flush() {
  if (is_worker_thread())
    return;

  std::unique_lock<std::mutex> lock{mutex_};
  idle_cv_.wait(lock, [this] {
    // Idle, or stopped and every worker has exited. worker_ids_.empty()
    // alone is also true in the window after start() clears the ids and
    // before the first worker publishes, which would let flush() return
    // while the queue still has work.
    return (queue_.empty() && busy_ == 0) ||
           (stop_requested_ && worker_ids_.empty());
  });
}

TASKHANDLER_INLINE bool ThreadPool::is_worker_thread() const {
  std::lock_guard<std::mutex> lock{mutex_};
  const std::thread::id id = std::this_thread::get_id();
  return std::ranges::find(worker_ids_, id) != worker_ids_.end();
}

TASKHANDLER_INLINE bool ThreadPool::running() const {
  std::lock_guard<std::mutex> lock{mutex_};
  return !stop_requested_ && !worker_ids_.empty();
}

TASKHANDLER_INLINE std::size_t ThreadPool::thread_count() const noexcept {
  return thread_count_;
}

TASKHANDLER_INLINE void
ThreadPool::destroy_user_code_nothrow(auto &&destroy) const noexcept {
  try {
    std::invoke(std::forward<decltype(destroy)>(destroy));
  } catch (...) {
    report_exception(std::current_exception());
  }
}

TASKHANDLER_INLINE void
ThreadPool::report_exception(std::exception_ptr error) const noexcept {
  if (!options_.on_exception)
    return;
  try {
    options_.on_exception(std::move(error));
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // A reporting hook that fails must not take a worker down with it; that
    // would be strictly worse than the exception it was reporting.
  }
}

TASKHANDLER_INLINE void
ThreadPool::apply_thread_name(std::size_t index) const noexcept {
  if (options_.thread_name_prefix.empty())
    return;
  try {
    detail::set_current_thread_name(
        std::format("{}-{}", options_.thread_name_prefix, index));
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // Naming is best-effort; std::format can throw on allocation.
  }
}

} // namespace conan

#endif // CONAN_DETAIL_THREAD_POOL_INL_H_
