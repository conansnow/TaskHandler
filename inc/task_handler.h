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
#include <condition_variable>
#include <functional>
#include <future>
#include <list>
#include <mutex>
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
    return thises_static[num_thread];
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
        thises_static[i] = new TaskHandler;
        thises_static[i]->start();
      }
    }
    ~PreLoader() {
      for (size_t i = 0; i < MAX_THREADS; i++) {
        thises_static[i]->stop();
        delete thises_static[i];
      }
    }
  };
  static inline PreLoader pre_loader{};
#endif

private:
  std::list<Node *> list_node_{};
  std::mutex q_mutex_{};

  std::thread thread_{};
  int is_stop_{0};

  std::condition_variable cv_{};
  bool ready_{false};
};

inline void TaskHandler::start() {
  thread_ = std::thread([&] {
    while (!is_stop_) {
      std::unique_lock mutex_gurad(q_mutex_);
      if (list_node_.empty()) {
        ready_ = false;
        cv_.wait(mutex_gurad, [&] { return ready_; });
        if (is_stop_)
          break;
      }
      auto node = list_node_.front();
      list_node_.pop_front();
      mutex_gurad.unlock();
      std::visit(
          [](auto &&c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, Callable<std::function, void>> ||
                          std::is_same_v<T,
                                         Callable<std::packaged_task, void>> ||
                          std::is_same_v<
                              T, Callable<std::packaged_task, std::any>>)
              c();
          },
          node->callable_);
      delete node;
    };
  });
}

inline void TaskHandler::stop() {
  is_stop_ = 1;

  std::unique_lock mutex_gurad(q_mutex_);
  ready_ = true;
  mutex_gurad.unlock();
  cv_.notify_one();

  thread_.join();
}

template <typename T, typename C, std::enable_if_t<is_queued_v<T>, void> *,
          std::enable_if_t<std::is_invocable_r_v<void, C>, void> *>
void TaskHandler::add_callable(C &&callable, const int priority) {
  auto node =
      new Node{priority, std::in_place_type_t<Callable<std::function, void>>{},
               std::forward<C>(callable)};

  std::unique_lock mutex_gurad(q_mutex_);
  add2list(list_node_, node);

  if (!ready_) {
    ready_ = true;
    mutex_gurad.unlock();
    cv_.notify_one();
  }
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

  auto node = new Node{
      priority, std::in_place_type_t<Callable<std::function, void>>{}, [&] {
        callable();
        promise_tmp.set_value();
      }};

  std::unique_lock mutex_gurad(q_mutex_);
  add2list(list_node_, node);

  if (!ready_) {
    ready_ = true;
    mutex_gurad.unlock();
    cv_.notify_one();
  } else
    mutex_gurad.unlock();
  future_tmp.wait();
}

template <typename T, typename C, std::enable_if_t<is_future_v<T>, void> *,
          std::enable_if_t<std::is_invocable_r_v<void, C>, void> *, typename R>
std::future<R> TaskHandler::add_callable(C &&callable, const int priority) {
  std::packaged_task<R()> packaged_task_tmp{};
  if constexpr (std::is_same_v<R, void>)
    packaged_task_tmp = std::packaged_task<R()>{callable};
  else
    packaged_task_tmp =
        std::packaged_task<R()>{[&] { return std::any{callable()}; }};

  auto future_tmp = packaged_task_tmp.get_future();

  if (std::this_thread::get_id() == thread_.get_id()) {
    packaged_task_tmp();
    return future_tmp;
  }

  auto node = new Node{priority,
                       std::in_place_type_t<Callable<std::packaged_task, R>>{},
                       std::move(packaged_task_tmp)};

  std::unique_lock mutex_gurad(q_mutex_);
  add2list(list_node_, node);

  if (!ready_) {
    ready_ = true;
    mutex_gurad.unlock();
    cv_.notify_one();
  }
  return future_tmp;
}
CURRENT_NAMESPACE_END

#undef CONAN_DISABLE_COPY
#undef CONAN_DISABLE_MOVE
#endif // TASK_HANDLER_H_
