#ifndef TASK_HANDLER_H_
#define TASK_HANDLER_H_
#pragma once

#ifdef TASKHANDLER_COMPILED_LIB
// #undef TASKHANDLER_HEADER_ONLY
#ifdef TASKHANDLER_SHARED_LIB
#ifdef _WIN32
#ifdef TASKHANDLER_EXPORTS
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
#define TASKHANDLER_INLINE
#else
#define TASKHANDLER_API
#if defined(_WIN32) && _MSC_VER < 1921
#error header-only support on windows needs vs2019 at least.
#endif
#define TASKHANDLER_HEADER_ONLY
#define TASKHANDLER_INLINE inline
#endif

#define CURRENT_NAMESPACE_START namespace conan {
#define CURRENT_NAMESPACE_END }
#define CURRENT_NAMESPACE conan

#define CONAN_DISABLE_COPY(TYPE)                                               \
  TYPE(const TYPE &) = delete;                                                 \
  TYPE &operator=(const TYPE &) = delete;
#define CONAN_DISABLE_MOVE(TYPE)                                               \
  TYPE(const TYPE &&) = delete;                                                \
  TYPE &operator=(const TYPE &&) = delete;

#include <algorithm>
#include <any>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>

CURRENT_NAMESPACE_START
struct Queued {};
struct Blocked {};
struct Future {};

template <typename> struct is_Queued : public std::false_type {};
template <> struct is_Queued<Queued> : public std::true_type {};
template <typename T> inline constexpr bool is_queued_v = is_Queued<T>::value;

template <typename> struct is_Blocked : public std::false_type {};
template <> struct is_Blocked<Blocked> : public std::true_type {};
template <typename T> inline constexpr bool is_blocked_v = is_Blocked<T>::value;

template <typename> struct is_Future : public std::false_type {};
template <> struct is_Future<Future> : public std::true_type {};
template <typename T> inline constexpr bool is_future_v = is_Future<T>::value;

template <template <typename T> typename C_T, typename R, typename... Args>
struct Callable {
  CONAN_DISABLE_COPY(Callable)
  CONAN_DISABLE_MOVE(Callable)

  template <typename C> Callable(C &&c) : work_{std::forward<C>(c)} {}

  std::invoke_result_t<C_T<R()>> operator()(Args... args) {
    return work_(args...);
  }

  C_T<R(Args...)> work_;
};

class TaskHandler;

class Node {
  friend class TaskHandler;

private:
  using HandleT = std::variant<Callable<std::function, void>,
                               Callable<std::packaged_task, void>,
                               Callable<std::packaged_task, std::any>>;

  template <typename... Args>
  Node(const int priority, Args &&...args)
      : priority_{priority}, callable_{std::forward<Args>(args)...} {}

private:
  const int priority_;
  HandleT callable_;
};

class TASKHANDLER_API TaskHandler {
public:
  template <size_t num_thread = 0> static TaskHandler *get_instance() noexcept {
    static_assert(num_thread < MAX_THREADS,
                  "num_thread must less than MAX_THREADS");
    TaskHandler *instance = thises_static[num_thread];
    if (!instance) {
      // Returning null here would only defer the failure to a null dereference
      // inside add_callable, far away from the actual mistake.
      std::fprintf(stderr, "conan::TaskHandler: no handler for index %zu. In "
                           "compiled-lib mode TaskHandler::init() must be "
                           "called before get_instance(), and get_instance() "
                           "must not be used after uninit().\n",
                   num_thread);
      std::abort();
    }
    return instance;
  }

  template <typename T = Queued, typename C,
            std::enable_if_t<is_queued_v<T>, void> * = nullptr,
            std::enable_if_t<std::is_invocable_r_v<void, C>, void> * = nullptr>
  void add_callable(C &&callable, const int priority = 0);

  template <typename T, typename C,
            std::enable_if_t<is_blocked_v<T>, void> * = nullptr,
            std::enable_if_t<std::is_invocable_r_v<void, C>, void> * = nullptr>
  void add_callable(C &&callable, const int priority = 0);

  template <typename T, typename C,
            std::enable_if_t<is_future_v<T>, void> * = nullptr,
            std::enable_if_t<std::is_invocable_r_v<void, C>, void> * = nullptr,
            typename R = std::conditional_t<
                std::is_same_v<std::invoke_result_t<C>, void>, void, std::any>>
  std::future<R> add_callable(C &&callable, const int priority = 0);

#ifdef TASKHANDLER_COMPILED_LIB
  static int init();
  static int uninit();
#endif

private:
  void start();
  void stop();

  // Hands `node` to the worker thread. Insertion happens under q_mutex_ while
  // is_stop_ is still clear, which is what lets stop() guarantee that every
  // accepted node eventually runs.
  void enqueue(std::unique_ptr<Node> node) {
    // add2list needs an lvalue: deducing N as a plain pointer would make its
    // comparator take a const rvalue reference, which cannot bind to *it.
    Node *raw = node.get();
    {
      std::lock_guard<std::mutex> mutex_guard(q_mutex_);
      if (is_stop_)
        throw std::runtime_error("conan::TaskHandler is stopped");
      add2list(list_node_, raw);
      node.release();
    }
    cv_.notify_one();
  }

  template <typename Container, typename N>
  void add2list(Container &&container, N &&node) {
    if constexpr (std::is_pointer_v<std::decay_t<N>>) {
      if (container.empty() || container.back()->priority_ <= node->priority_)
        container.emplace_back(std::forward<N>(node));
      else {
        auto at = std::upper_bound(container.begin(), container.end(),
                                   node->priority_,
                                   [](const int priority, const N &&obj) {
                                     return priority < obj->priority_;
                                   });
        container.emplace(at, std::forward<N>(node));
      }
    } else {
      if (container.empty() || container.back().priority_ <= node.priority_)
        container.emplace_back(std::forward<N>(node));
      else {
        auto at =
            std::upper_bound(container.begin(), container.end(), node.priority_,
                             [](const int priority, const N &&obj) {
                               return priority < obj.priority_;
                             });
        container.emplace(at, std::forward<N>(node));
      }
    }
  }

private:
  TaskHandler() = default;
  ~TaskHandler() = default;

  static inline constexpr size_t MAX_THREADS{3};
#ifdef TASKHANDLER_HEADER_ONLY
  static TASKHANDLER_INLINE TaskHandler *thises_static[MAX_THREADS]{};
#else
  static TASKHANDLER_INLINE TaskHandler *thises_static[MAX_THREADS];
#endif

private:
#ifdef TASKHANDLER_HEADER_ONLY
  struct PreLoader {
    PreLoader() {
      for (size_t i = 0; i < MAX_THREADS; i++) {
        // Publish only once the worker is running, so that a concurrent
        // get_instance() cannot hand out a handler with no thread behind it.
        auto *handler = new TaskHandler;
        handler->start();
        thises_static[i] = handler;
      }
    }
    ~PreLoader() {
      for (size_t i = 0; i < MAX_THREADS; i++) {
        thises_static[i]->stop();
        delete thises_static[i];
        thises_static[i] = nullptr;
      }
    }
  };
  static inline PreLoader pre_loader{};
#endif

private:
  std::list<Node *> list_node_{};
  std::mutex q_mutex_{};

  std::thread thread_{};
  std::atomic_bool is_stop_{false};

  std::condition_variable cv_{};
};

inline void TaskHandler::start() {
  thread_ = std::thread([this] {
    for (;;) {
      std::unique_ptr<Node> node;
      {
        std::unique_lock<std::mutex> mutex_guard(q_mutex_);
        cv_.wait(mutex_guard,
                 [this] { return !list_node_.empty() || is_stop_; });
        // An empty queue here implies is_stop_. Checking the queue first drains
        // work that was accepted before the stop request instead of discarding
        // it, which is what keeps add_callable<Blocked> callers from waiting on
        // a promise that nobody will ever fulfil.
        if (list_node_.empty())
          break;
        node.reset(list_node_.front());
        list_node_.pop_front();
      }
      try {
        std::visit([](auto &&c) { c(); }, node->callable_);
      } catch (...) {
        // A Queued task has no channel to report a failure on, so the
        // exception is dropped rather than taking the process down with it.
        // Blocked and Future tasks capture their own exceptions and hand them
        // back to the caller.
      }
    }
  });
}

inline void TaskHandler::stop() {
  {
    // Setting the flag under the mutex is what makes the notify below
    // race-free: the worker either is already waiting and gets woken, or has
    // yet to reach the wait and will observe is_stop_ in its predicate.
    std::lock_guard<std::mutex> mutex_guard(q_mutex_);
    is_stop_ = true;
  }
  cv_.notify_all();

  if (thread_.joinable())
    thread_.join();
}

template <typename T, typename C, std::enable_if_t<is_queued_v<T>, void> *,
          std::enable_if_t<std::is_invocable_r_v<void, C>, void> *>
void TaskHandler::add_callable(C &&callable, const int priority) {
  enqueue(std::unique_ptr<Node>{
      new Node{priority, std::in_place_type_t<Callable<std::function, void>>{},
               std::forward<C>(callable)}});
}

template <typename T, typename C, std::enable_if_t<is_blocked_v<T>, void> *,
          std::enable_if_t<std::is_invocable_r_v<void, C>, void> *>
void TaskHandler::add_callable(C &&callable, const int priority) {
  if (std::this_thread::get_id() == thread_.get_id()) {
    callable();
    return;
  }

  std::promise<void> promise_tmp;
  auto future_tmp = promise_tmp.get_future();

  // Capturing by reference is safe here, and only here, because this function
  // does not return until the task has run to completion.
  enqueue(std::unique_ptr<Node>{new Node{
      priority, std::in_place_type_t<Callable<std::function, void>>{}, [&] {
        try {
          callable();
          promise_tmp.set_value();
        } catch (...) {
          promise_tmp.set_exception(std::current_exception());
        }
      }}});

  // get() rather than wait(), so a task that threw reports the failure to the
  // caller instead of silently succeeding.
  future_tmp.get();
}

template <typename T, typename C, std::enable_if_t<is_future_v<T>, void> *,
          std::enable_if_t<std::is_invocable_r_v<void, C>, void> *, typename R>
std::future<R> TaskHandler::add_callable(C &&callable, const int priority) {
  // The task outlives this call, so it has to own the callable. Capturing it by
  // reference leaves the worker thread reading a caller temporary that has
  // already gone out of scope.
  std::packaged_task<R()> packaged_task_tmp{};
  if constexpr (std::is_same_v<R, void>)
    packaged_task_tmp = std::packaged_task<R()>{std::forward<C>(callable)};
  else
    packaged_task_tmp = std::packaged_task<R()>{
        [c = std::forward<C>(callable)]() mutable { return std::any{c()}; }};

  auto future_tmp = packaged_task_tmp.get_future();

  if (std::this_thread::get_id() == thread_.get_id()) {
    packaged_task_tmp();
    return future_tmp;
  }

  enqueue(std::unique_ptr<Node>{
      new Node{priority, std::in_place_type_t<Callable<std::packaged_task, R>>{},
               std::move(packaged_task_tmp)}});
  return future_tmp;
}
CURRENT_NAMESPACE_END

#undef CONAN_DISABLE_COPY
#undef CONAN_DISABLE_MOVE
#endif // TASK_HANDLER_H_
