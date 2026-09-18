#ifndef CONAN_DETAIL_TASK_HANDLER_INL_H_
#define CONAN_DETAIL_TASK_HANDLER_INL_H_

// Out-of-line definitions, and not part of the public interface: include
// conan/task_handler.h instead. In header-only builds this file is pulled in by
// that header and everything below is `inline`; in compiled-library builds it
// is compiled exactly once, into src/task_handler.cc.

#ifndef TASKHANDLER_HEADER_ONLY
#include "conan/task_handler.h"
#endif

#include "conan/detail/thread_name.h"

#include <array>
#include <cstdlib>
#include <format>

namespace conan {

TASKHANDLER_INLINE const char *runtime_version() noexcept {
  return TASKHANDLER_VERSION_STRING;
}

TASKHANDLER_INLINE TaskHandler::TaskHandler(TaskHandlerOptions options)
    : options_{std::move(options)} {
  start();
}

TASKHANDLER_INLINE TaskHandler::~TaskHandler() noexcept {
  try {
    stop();
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // std::thread::join can throw system_error. A destructor must not.
  }
}

TASKHANDLER_INLINE void TaskHandler::start() {
  // Taking the lifecycle mutex from inside a task would deadlock against a
  // stop() that is holding it while waiting to join this very worker. Clearing
  // the stop request instead is no better: the worker would stop exiting and
  // that stop() would wait forever. The worker is running either way, so there
  // is nothing this call can usefully do.
  if (is_current_thread())
    return;

  std::lock_guard<std::mutex> lifecycle_guard{lifecycle_mutex_};
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock{mutex_};
      // Already running, and not on the way out.
      if (worker_id_ != std::thread::id{} && !stop_requested_)
        return;
    }
    // A stop() requested from inside a task returns without joining, because a
    // thread cannot join itself, so the thread object outlives the worker it
    // owned. running() is already false once the stop flag is set, which used
    // to make start() return here while the worker was still exiting and leave
    // the handler stopped for good. Join waits that out, then we spawn below.
    thread_.join();
  }

  std::unique_lock<std::mutex> lock{mutex_};
  stop_requested_ = false;
  worker_id_ = std::thread::id{};
  thread_ = std::thread{[this] { run_worker(); }};

  // Letting the worker publish its own id closes the window in which a task
  // could already be running while is_current_thread() still answers false.
  idle_cv_.wait(lock, [this] { return worker_id_ != std::thread::id{}; });
}

TASKHANDLER_INLINE void TaskHandler::stop() {
  // A worker cannot join itself: std::thread::join would throw
  // resource_deadlock_would_occur and terminate the process. Recording the
  // request is enough, because the worker checks it on the way round the loop
  // and exits once the queue is drained.
  if (is_current_thread()) {
    request_stop();
    return;
  }

  std::lock_guard<std::mutex> lifecycle_guard{lifecycle_mutex_};
  request_stop();
  if (thread_.joinable())
    thread_.join();
}

TASKHANDLER_INLINE void TaskHandler::request_stop() {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    stop_requested_ = true;
  }
  work_cv_.notify_all();
  idle_cv_.notify_all();
}

TASKHANDLER_INLINE void TaskHandler::run_worker() {
  apply_thread_name();

  std::unique_lock<std::mutex> lock{mutex_};
  worker_id_ = std::this_thread::get_id();
  idle_cv_.notify_all();

  for (;;) {
    promote_due_timers(std::chrono::steady_clock::now());

    if (!ready_.empty()) {
      run_one_ready_task(lock);
      continue;
    }

    // An empty runnable queue is checked before the stop flag so that work
    // accepted before the stop request still runs, which is what keeps
    // add_callable<Blocked> callers from waiting on a promise nobody fulfils.
    if (stop_requested_)
      break;

    if (!timed_.empty())
      work_cv_.wait_until(lock, timed_.begin()->first.deadline);
    else
      work_cv_.wait(lock);
  }

  discard_undue_timers(lock);
  // Cleared only after discarded timers are gone: their destructors still
  // need is_current_thread() to answer true.
  worker_id_ = std::thread::id{};
  idle_cv_.notify_all();
}

TASKHANDLER_INLINE void
TaskHandler::run_one_ready_task(std::unique_lock<std::mutex> &lock) {
  auto node = ready_.extract(ready_.begin());
  detail::Task task = std::move(node.mapped());
  busy_ = true;

  lock.unlock();
  try {
    task();
  } catch (...) {
    report_exception(std::current_exception());
  }
  destroy_user_code_nothrow([&task] { task = nullptr; });
  lock.lock();

  busy_ = false;
  idle_cv_.notify_all();
}

TASKHANDLER_INLINE void
TaskHandler::discard_undue_timers(std::unique_lock<std::mutex> &lock) {
  // Delayed tasks that never came due are dropped rather than held for a later
  // start(): stop() promises to discard them, and keeping them would leave
  // pending() counting work that nothing is going to run. They are destroyed
  // below with the lock released, because a task's destructor is user code and
  // may well touch this handler. worker_id_ stays set until that is done so
  // is_current_thread() is still true: a destructor that calls start() or
  // stop() must not try to take lifecycle_mutex_ against a join already in
  // progress.
  std::map<detail::TimerKey, detail::TimerEntry> discarded;
  discarded.swap(timed_);

  lock.unlock();
  destroy_user_code_nothrow([&discarded] { discarded.clear(); });
  lock.lock();
}

TASKHANDLER_INLINE void
TaskHandler::promote_due_timers(std::chrono::steady_clock::time_point now) {
  while (!timed_.empty() && timed_.begin()->first.deadline <= now) {
    auto node = timed_.extract(timed_.begin());
    try {
      // Keeping the original sequence number means a task stays cancellable
      // across the move. try_emplace default-constructs an empty Task first
      // so a throwing allocation cannot consume the callable; moving the
      // stored Task is noexcept.
      auto it =
          ready_
              .try_emplace(detail::ReadyKey{.priority = node.mapped().priority,
                                            .sequence = node.key().sequence})
              .first;
      it->second = std::move(node.mapped().task);
    } catch (...) {
      timed_.insert(std::move(node));
      throw;
    }
  }
}

TASKHANDLER_INLINE void TaskHandler::ensure_accepting() const {
  if (stop_requested_)
    throw TaskHandlerStopped{};
  if (options_.max_pending != 0 &&
      ready_.size() + timed_.size() >= options_.max_pending)
    throw TaskHandlerQueueFull{};
}

TASKHANDLER_INLINE TaskId TaskHandler::submit(detail::Task task, int priority) {
  TaskId id;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    ensure_accepting();

    id.kind_ = TaskId::Kind::ready;
    id.priority_ = priority;
    id.sequence_ = ++sequence_;
    ready_.emplace(
        detail::ReadyKey{.priority = priority, .sequence = id.sequence_},
        std::move(task));
  }
  work_cv_.notify_one();
  return id;
}

TASKHANDLER_INLINE TaskId
TaskHandler::submit_at(detail::Task task, int priority,
                       std::chrono::steady_clock::time_point deadline) {
  TaskId id;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    ensure_accepting();

    id.kind_ = TaskId::Kind::timed;
    id.priority_ = priority;
    id.sequence_ = ++sequence_;
    id.deadline_ = deadline;
    timed_.emplace(
        detail::TimerKey{.deadline = deadline, .sequence = id.sequence_},
        detail::TimerEntry{.priority = priority, .task = std::move(task)});
  }
  // Notified unconditionally: the new deadline may be earlier than the one the
  // worker is currently sleeping on, in which case it has to re-arm.
  work_cv_.notify_one();
  return id;
}

TASKHANDLER_INLINE bool TaskHandler::cancel(const TaskId &id) {
  if (!id.valid())
    return false;

  detail::Task discarded;
  bool cancelled = false;
  bool cancelled_timer = false;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    // Kind::timed means the task was submitted delayed. Promotion does not
    // rewrite the caller's TaskId, so a due timer may already be in ready_.
    // Looking only at timed_ would make post-promotion cancel a no-op.
    if (id.kind_ == TaskId::Kind::timed) {
      if (auto node = timed_.extract(detail::TimerKey{
              .deadline = id.deadline_, .sequence = id.sequence_})) {
        discarded = std::move(node.mapped().task);
        cancelled = true;
        cancelled_timer = true;
      }
    }
    if (!cancelled) {
      if (auto node = ready_.extract(detail::ReadyKey{
              .priority = id.priority_, .sequence = id.sequence_})) {
        discarded = std::move(node.mapped());
        cancelled = true;
      }
    }
  }

  // Cancelling the timer the worker is sleeping on would otherwise leave it
  // parked until that (now stale) deadline.
  if (cancelled_timer)
    work_cv_.notify_one();

  destroy_user_code_nothrow([&discarded] { discarded = nullptr; });
  return cancelled;
}

TASKHANDLER_INLINE std::size_t TaskHandler::pending() const {
  std::lock_guard<std::mutex> lock{mutex_};
  return ready_.size() + timed_.size();
}

TASKHANDLER_INLINE void TaskHandler::flush() {
  if (is_current_thread())
    return;

  std::unique_lock<std::mutex> lock{mutex_};
  idle_cv_.wait(lock, [this] {
    // The second disjunct keeps flush() from hanging on a handler whose worker
    // has already exited.
    return (ready_.empty() && !busy_) || worker_id_ == std::thread::id{};
  });
}

TASKHANDLER_INLINE bool TaskHandler::is_current_thread() const {
  std::lock_guard<std::mutex> lock{mutex_};
  return worker_id_ == std::this_thread::get_id();
}

TASKHANDLER_INLINE bool TaskHandler::running() const {
  std::lock_guard<std::mutex> lock{mutex_};
  return !stop_requested_ && worker_id_ != std::thread::id{};
}

TASKHANDLER_INLINE void
TaskHandler::destroy_user_code_nothrow(auto &&destroy) const noexcept {
  try {
    std::invoke(std::forward<decltype(destroy)>(destroy));
  } catch (...) {
    report_exception(std::current_exception());
  }
}

TASKHANDLER_INLINE void
TaskHandler::report_exception(std::exception_ptr error) const noexcept {
  if (!options_.on_exception)
    return;
  try {
    options_.on_exception(std::move(error));
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // A reporting hook that fails must not take the worker thread down with
    // it; that would be strictly worse than the exception it was reporting.
  }
}

TASKHANDLER_INLINE void TaskHandler::apply_thread_name() const noexcept {
  detail::set_current_thread_name(options_.thread_name);
}

namespace detail {

class InstanceRegistry {
public:
  InstanceRegistry() = default;
  ~InstanceRegistry() { stop_all(); }

  InstanceRegistry(const InstanceRegistry &) = delete;
  InstanceRegistry &operator=(const InstanceRegistry &) = delete;
  InstanceRegistry(InstanceRegistry &&) = delete;
  InstanceRegistry &operator=(InstanceRegistry &&) = delete;

  TaskHandler &get(std::size_t index) {
    std::lock_guard<std::mutex> lock{mutex_};
    return *create(index);
  }

  void start_all() {
    std::array<TaskHandler *, TaskHandler::instance_count()> snapshot{};
    {
      std::lock_guard<std::mutex> lock{mutex_};
      for (std::size_t index = 0; index < TaskHandler::instance_count();
           index++)
        snapshot[index] = create(index);
    }
    // start() waits for the worker handshake and may join an exiting thread.
    // Neither can run while this mutex is held: a task on a shared handler
    // that calls instance() would wait for us, and we would wait for it.
    for (TaskHandler *handler : snapshot)
      handler->start();
  }

  void stop_all() {
    // Two passes so a handler created by instance() while the first pass was
    // joining is still stopped. The registry mutex is not held across stop():
    // join waits for the current task, and that task is allowed to call
    // instance() / init() / uninit().
    for (int pass = 0; pass < 2; pass++) {
      std::array<TaskHandler *, TaskHandler::instance_count()> snapshot{};
      {
        std::lock_guard<std::mutex> lock{mutex_};
        snapshot = handlers_;
      }
      for (TaskHandler *handler : snapshot) {
        if (handler != nullptr)
          handler->stop();
      }
    }
  }

private:
  TaskHandler *create(std::size_t index) {
    if (handlers_[index] == nullptr) {
      TaskHandlerOptions options;
      options.thread_name = std::format("conan-task-{}", index);
      // Deliberately never freed. A reference handed out by instance() has to
      // stay valid for the rest of the program, including while other static
      // objects are running their destructors. The registry itself is leaked
      // for the same reason; only the worker threads are shut down.
      handlers_[index] = new TaskHandler{std::move(options)};
    }
    return handlers_[index];
  }

  std::mutex mutex_{};
  std::array<TaskHandler *, TaskHandler::instance_count()> handlers_{};
};

TASKHANDLER_INLINE InstanceRegistry &registry() {
  // Never destroyed. A static destructor that calls instance() after this
  // object's usual lifetime would otherwise touch a dead mutex. Handlers are
  // leaked for the same reason. atexit still joins the workers so process
  // exit matches uninit().
  static auto *instance = new InstanceRegistry;
  static const int atexit_stop = [] {
    std::atexit([] { instance->stop_all(); });
    return 0;
  }();
  static_cast<void>(atexit_stop);
  return *instance;
}

} // namespace detail

TASKHANDLER_INLINE TaskHandler &TaskHandler::instance(std::size_t index) {
  if (index >= kInstanceCount)
    throw std::out_of_range{"conan::TaskHandler: instance index out of range"};
  return detail::registry().get(index);
}

TASKHANDLER_INLINE void TaskHandler::init() { detail::registry().start_all(); }

TASKHANDLER_INLINE void TaskHandler::uninit() { detail::registry().stop_all(); }

} // namespace conan

#endif // CONAN_DETAIL_TASK_HANDLER_INL_H_
