#ifndef CONAN_TASK_HANDLER_H_
#define CONAN_TASK_HANDLER_H_

// TaskHandler is a single-worker task queue: every handler owns exactly one
// thread, and everything submitted to that handler runs on it, one task at a
// time. That is what makes it useful as an event handler -- state owned by a
// handler needs no locking, because only its worker ever touches it.
//
// The library can be consumed in two ways:
//
//   * Header-only (the default). Include this header and nothing else.
//   * Compiled library. Define TASKHANDLER_COMPILED_LIB when building both the
//     library and its consumers, and link the `task_handler` target. Add
//     TASKHANDLER_SHARED_LIB on top when using the shared build. The CMake
//     target propagates both automatically.

// The version is spelled out here as well as in CMake, because a header-only
// consumer has no CMake project to ask. The build checks the two against each
// other, so they cannot drift apart.
#define TASKHANDLER_VERSION_MAJOR 0
#define TASKHANDLER_VERSION_MINOR 2
#define TASKHANDLER_VERSION_PATCH 0
#define TASKHANDLER_VERSION_STRING "0.2.0"

// Comparable form, for #if checks against a required version.
#define TASKHANDLER_VERSION                                                    \
  (TASKHANDLER_VERSION_MAJOR * 10000 + TASKHANDLER_VERSION_MINOR * 100 +       \
   TASKHANDLER_VERSION_PATCH)

#if defined(TASKHANDLER_COMPILED_LIB)
#define TASKHANDLER_INLINE
#if defined(TASKHANDLER_SHARED_LIB)
#if defined(_WIN32)
#if defined(TASKHANDLER_EXPORTS)
#define TASKHANDLER_API __declspec(dllexport)
#else
#define TASKHANDLER_API __declspec(dllimport)
#endif
#else
#define TASKHANDLER_API __attribute__((visibility("default")))
#endif
#else
#define TASKHANDLER_API
#endif
#else
#define TASKHANDLER_HEADER_ONLY
#define TASKHANDLER_INLINE inline
#define TASKHANDLER_API
#endif

// Types whose typeinfo and vtable are shared between the library and its
// consumers must keep default visibility even when the library is built with
// -fvisibility=hidden, otherwise an exception thrown inside the shared object
// cannot be caught by type on the other side of the boundary.
#if defined(_WIN32)
#define TASKHANDLER_VISIBLE
#else
#define TASKHANDLER_VISIBLE __attribute__((visibility("default")))
#endif

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace conan {

// The version the linked library was built as. In a header-only build this is
// necessarily TASKHANDLER_VERSION_STRING; in a compiled build it is whatever
// the library itself was compiled from, which is how a header that has drifted
// away from the binary next to it can be spotted at runtime.
[[nodiscard]] TASKHANDLER_API const char *runtime_version() noexcept;

// Submission-policy tags, selected as the first template argument of
// add_callable(). See TaskHandler for what each one does.
struct Queued {};
struct Blocked {};
struct Future {};

namespace detail {

template <typename> struct IsQueued : std::false_type {};
template <> struct IsQueued<Queued> : std::true_type {};
template <typename T> inline constexpr bool is_queued_v = IsQueued<T>::value;

template <typename> struct IsBlocked : std::false_type {};
template <> struct IsBlocked<Blocked> : std::true_type {};
template <typename T> inline constexpr bool is_blocked_v = IsBlocked<T>::value;

template <typename> struct IsFuture : std::false_type {};
template <> struct IsFuture<Future> : std::true_type {};
template <typename T> inline constexpr bool is_future_v = IsFuture<T>::value;

// The callable as the queue stores it: an lvalue of the decayed type, which is
// how std::packaged_task and TaskImpl below invoke it.
template <typename C> using CallableRef = std::decay_t<C> &;

template <typename C>
inline constexpr bool is_task_v = std::is_invocable_v<CallableRef<C>>;

template <typename C> using TaskResultT = std::invoke_result_t<CallableRef<C>>;

// Type-erased nullary task. Unlike std::function this never requires the
// callable to be copy-constructible, so move-only state (a std::packaged_task,
// a captured std::unique_ptr) can be queued directly.
class TASKHANDLER_VISIBLE Task {
public:
  Task() = default;
  virtual ~Task() = default;
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;
  Task(Task &&) = delete;
  Task &operator=(Task &&) = delete;

  virtual void run() = 0;
};

template <typename C> class TaskImpl final : public Task {
public:
  explicit TaskImpl(C callable) : callable_{std::move(callable)} {}

  void run() override { callable_(); }

private:
  C callable_;
};

template <typename C> std::unique_ptr<Task> make_task(C &&callable) {
  return std::unique_ptr<Task>{
      new TaskImpl<std::decay_t<C>>{std::forward<C>(callable)}};
}

// Ordering of runnable tasks: higher priority first, and within one priority
// the order in which the tasks were submitted. The sequence number is what
// makes equal priorities stable instead of merely unspecified.
struct ReadyKey {
  int priority{0};
  std::uint64_t sequence{0};
};

inline bool operator<(const ReadyKey &lhs, const ReadyKey &rhs) noexcept {
  if (lhs.priority != rhs.priority)
    return lhs.priority > rhs.priority;
  return lhs.sequence < rhs.sequence;
}

struct TimerKey {
  std::chrono::steady_clock::time_point deadline{};
  std::uint64_t sequence{0};
};

inline bool operator<(const TimerKey &lhs, const TimerKey &rhs) noexcept {
  if (lhs.deadline != rhs.deadline)
    return lhs.deadline < rhs.deadline;
  return lhs.sequence < rhs.sequence;
}

struct TimerEntry {
  int priority{0};
  std::unique_ptr<Task> task{};
};

} // namespace detail

// Base of every exception the library itself throws, so that a caller which
// only wants to know that a submission was refused can catch one type.
class TASKHANDLER_VISIBLE TaskHandlerError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Thrown by add_callable() and friends when the handler has been stopped and
// can no longer accept work.
class TASKHANDLER_VISIBLE TaskHandlerStopped : public TaskHandlerError {
public:
  TaskHandlerStopped() : TaskHandlerError{"conan::TaskHandler is stopped"} {}
};

// Thrown by add_callable() and friends when the handler already holds
// TaskHandlerOptions::max_pending tasks. Only ever thrown by a handler that was
// given a limit; the default queue is unbounded.
class TASKHANDLER_VISIBLE TaskHandlerQueueFull : public TaskHandlerError {
public:
  TaskHandlerQueueFull()
      : TaskHandlerError{"conan::TaskHandler queue is full"} {}
};

// Identifies a submitted task for as long as it has not started running.
// Default-constructed ids are invalid and cancelling one is a no-op.
class TaskId {
public:
  TaskId() = default;

  [[nodiscard]] bool valid() const noexcept { return sequence_ != 0; }
  explicit operator bool() const noexcept { return valid(); }

private:
  friend class TaskHandler;

  enum class Kind : unsigned char { none, ready, timed };

  Kind kind_{Kind::none};
  int priority_{0};
  std::uint64_t sequence_{0};
  std::chrono::steady_clock::time_point deadline_{};
};

struct TaskHandlerOptions {
  // Name given to the worker thread, for debuggers and profilers. Linux
  // truncates thread names to 15 characters; other platforms may ignore it.
  std::string thread_name{};

  // Invoked on the worker thread when a Queued task throws. Queued tasks have
  // no future to report a failure through, so without this hook the exception
  // is discarded. Blocked and Future tasks deliver their exceptions to the
  // caller and never reach this hook. An exception thrown by the hook itself
  // is ignored.
  std::function<void(std::exception_ptr)> on_exception{};

  // Largest number of tasks the handler will hold at once, counting both
  // runnable and not-yet-due ones. Zero, the default, means no limit.
  //
  // A limit is backpressure: once it is reached, submitting throws
  // TaskHandlerQueueFull instead of letting a producer that outruns its worker
  // grow the queue until the process runs out of memory. Recursive Blocked and
  // Future submissions from the worker thread run inline without queueing, so
  // they are unaffected by the limit and cannot be refused by it.
  std::size_t max_pending{0};
};

class TASKHANDLER_API TaskHandler {
public:
  // Constructing a handler starts its worker thread; destroying it drains the
  // runnable queue and joins.
  explicit TaskHandler(TaskHandlerOptions options = {});
  ~TaskHandler();

  TaskHandler(const TaskHandler &) = delete;
  TaskHandler &operator=(const TaskHandler &) = delete;
  TaskHandler(TaskHandler &&) = delete;
  TaskHandler &operator=(TaskHandler &&) = delete;

  // Submits `callable` and returns immediately. Exceptions escaping the task
  // are reported to TaskHandlerOptions::on_exception and otherwise discarded.
  template <typename T = Queued, typename C,
            std::enable_if_t<detail::is_queued_v<T>, int> = 0,
            std::enable_if_t<detail::is_task_v<C>, int> = 0>
  TaskId add_callable(C &&callable, int priority = 0);

  // Submits `callable` and blocks until it has run, rethrowing whatever it
  // threw. Called from the handler's own worker thread it runs the task inline
  // instead of deadlocking, which is what makes recursive use safe.
  template <typename T, typename C,
            std::enable_if_t<detail::is_blocked_v<T>, int> = 0,
            std::enable_if_t<detail::is_task_v<C>, int> = 0>
  void add_callable(C &&callable, int priority = 0);

  // Submits `callable` and returns a future for its result. The future is
  // typed: a task returning int yields std::future<int>, and move-only results
  // such as std::unique_ptr work.
  template <typename T, typename C,
            std::enable_if_t<detail::is_future_v<T>, int> = 0,
            std::enable_if_t<detail::is_task_v<C>, int> = 0>
  std::future<detail::TaskResultT<C>> add_callable(C &&callable,
                                                   int priority = 0);

  // Submits `callable` to run no earlier than `delay` from now. The task
  // becomes runnable at its deadline and is then ordered by priority like any
  // other task, so a busy handler may run it later than requested.
  template <typename Rep, typename Period, typename C,
            std::enable_if_t<detail::is_task_v<C>, int> = 0>
  TaskId add_callable_after(std::chrono::duration<Rep, Period> delay,
                            C &&callable, int priority = 0);

  template <typename C, std::enable_if_t<detail::is_task_v<C>, int> = 0>
  TaskId add_callable_at(std::chrono::steady_clock::time_point deadline,
                         C &&callable, int priority = 0);

  // Cancels a task that has not started running yet. Returns false if the task
  // already ran, is running, was already cancelled, or the id is invalid.
  bool cancel(const TaskId &id);

  // Tasks accepted but not yet started, including those waiting on a deadline.
  // Excludes the task currently running.
  [[nodiscard]] std::size_t pending() const;

  // Blocks until every runnable task submitted so far has finished and the
  // worker is idle. Tasks still waiting on a deadline are not waited for.
  // Returns immediately when called from the worker thread, where waiting
  // could only deadlock.
  void flush();

  [[nodiscard]] bool is_current_thread() const;
  [[nodiscard]] bool running() const;

  // Starts the worker if it is not running. Works after a stop(), including
  // one that was requested from inside a task. Idempotent.
  //
  // Calling start() from inside one of the handler's own tasks does nothing:
  // the worker is running by definition, and a stop request already made can
  // only be taken back from another thread, because a stop() that is waiting
  // to join this worker would otherwise never be let go.
  void start();

  // Runs everything already queued, then stops the worker and joins it.
  // Deadline-based tasks that are not yet due are discarded. Idempotent.
  //
  // Calling stop() from inside a task only records the request and returns:
  // the worker cannot join itself, so it finishes draining and exits on its
  // own.
  void stop();

  // Shared handlers, created on first use. The reference stays valid for the
  // rest of the program, so it is never left dangling by uninit().
  [[nodiscard]] static constexpr std::size_t instance_count() noexcept {
    return kInstanceCount;
  }

  template <std::size_t Index = 0>
  [[nodiscard]] static TaskHandler &instance() {
    static_assert(Index < kInstanceCount,
                  "instance index must be less than instance_count()");
    return instance(Index);
  }

  [[nodiscard]] static TaskHandler &instance(std::size_t index);

  // Starts every shared handler. Optional: instance() creates a started
  // handler on its own. Useful to pay the thread-creation cost up front, and
  // to bring the handlers back after uninit(). Idempotent.
  static void init();

  // Drains and stops every shared handler that has been created. Runs
  // automatically at program exit. After this, submitting to a shared handler
  // throws TaskHandlerStopped until init() is called again. Idempotent.
  static void uninit();

private:
  TaskId submit(std::unique_ptr<detail::Task> task, int priority);
  TaskId submit_at(std::unique_ptr<detail::Task> task, int priority,
                   std::chrono::steady_clock::time_point deadline);
  // Both submission paths share one set of accept-or-refuse rules. Call with
  // mutex_ held; throws rather than returning a code so that the refusal
  // reaches the caller of add_callable() unchanged.
  void ensure_accepting() const;
  [[nodiscard]] bool worker_alive() const;
  void request_stop();
  void run_worker();
  void promote_due_timers(std::chrono::steady_clock::time_point now);
  void report_exception(std::exception_ptr error) const noexcept;
  void apply_thread_name() const noexcept;

  static constexpr std::size_t kInstanceCount{3};

  TaskHandlerOptions options_{};

  mutable std::mutex mutex_{};
  std::condition_variable work_cv_{};
  std::condition_variable idle_cv_{};

  std::map<detail::ReadyKey, std::unique_ptr<detail::Task>> ready_{};
  std::map<detail::TimerKey, detail::TimerEntry> timed_{};

  std::uint64_t sequence_{0};
  bool stop_requested_{false};
  bool busy_{false};
  std::thread::id worker_id_{};

  // Guards start()/stop() against each other so that `thread_` is only ever
  // touched by one caller at a time.
  std::mutex lifecycle_mutex_{};
  std::thread thread_{};
};

template <typename T, typename C, std::enable_if_t<detail::is_queued_v<T>, int>,
          std::enable_if_t<detail::is_task_v<C>, int>>
TaskId TaskHandler::add_callable(C &&callable, int priority) {
  return submit(detail::make_task(std::forward<C>(callable)), priority);
}

template <typename T, typename C,
          std::enable_if_t<detail::is_blocked_v<T>, int>,
          std::enable_if_t<detail::is_task_v<C>, int>>
// The callable is deliberately not forwarded: this overload blocks until the
// task has run, so the task borrows the caller's object instead of owning it.
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
void TaskHandler::add_callable(C &&callable, int priority) {
  if (is_current_thread()) {
    callable();
    return;
  }

  std::promise<void> promise;
  std::future<void> future = promise.get_future();

  // Capturing by reference is safe here, and only here, because this function
  // does not return until the task has run to completion. If the handler is
  // stopped before the task runs, the promise is destroyed and get() below
  // throws std::future_error rather than blocking forever.
  submit(detail::make_task([&callable, &promise] {
           try {
             callable();
             promise.set_value();
           } catch (...) {
             promise.set_exception(std::current_exception());
           }
         }),
         priority);

  // get() rather than wait(), so that a task which threw reports the failure
  // to the caller instead of silently succeeding.
  future.get();
}

template <typename T, typename C, std::enable_if_t<detail::is_future_v<T>, int>,
          std::enable_if_t<detail::is_task_v<C>, int>>
std::future<detail::TaskResultT<C>> TaskHandler::add_callable(C &&callable,
                                                              int priority) {
  // The task outlives this call, so it has to own the callable rather than
  // reference a caller temporary that is about to go out of scope.
  std::packaged_task<detail::TaskResultT<C>()> packaged{
      std::forward<C>(callable)};
  std::future<detail::TaskResultT<C>> future = packaged.get_future();

  if (is_current_thread()) {
    packaged();
    return future;
  }

  submit(detail::make_task(
             [packaged = std::move(packaged)]() mutable { packaged(); }),
         priority);
  return future;
}

template <typename Rep, typename Period, typename C,
          std::enable_if_t<detail::is_task_v<C>, int>>
TaskId TaskHandler::add_callable_after(std::chrono::duration<Rep, Period> delay,
                                       C &&callable, int priority) {
  return add_callable_at(std::chrono::steady_clock::now() + delay,
                         std::forward<C>(callable), priority);
}

template <typename C, std::enable_if_t<detail::is_task_v<C>, int>>
TaskId
TaskHandler::add_callable_at(std::chrono::steady_clock::time_point deadline,
                             C &&callable, int priority) {
  return submit_at(detail::make_task(std::forward<C>(callable)), priority,
                   deadline);
}

} // namespace conan

#ifdef TASKHANDLER_HEADER_ONLY
#include "conan/detail/task_handler-inl.h"
#endif

#endif // CONAN_TASK_HANDLER_H_
