#ifndef CONAN_DETAIL_THREAD_NAME_H_
#define CONAN_DETAIL_THREAD_NAME_H_

// Shared by TaskHandler and ThreadPool. Not part of the public interface.

#include <cstddef>
#include <string>
#include <string_view>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace conan::detail {

inline void set_current_thread_name(std::string_view name) noexcept {
  if (name.empty())
    return;
  try {
#if defined(__linux__)
    // Linux caps thread names at 16 bytes including the terminator.
    const std::string truncated{name.substr(0, 15)};
    pthread_setname_np(pthread_self(), truncated.c_str());
#elif defined(__APPLE__)
    const std::string copy{name};
    pthread_setname_np(copy.c_str());
#elif defined(_WIN32)
    const std::string utf8{name};
    const int wide_size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wide_size <= 0)
      return;
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(),
                            wide_size) <= 0)
      return;
    static_cast<void>(SetThreadDescription(GetCurrentThread(), wide.c_str()));
#endif
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // Naming is best-effort for debuggers. A failure must not take the worker
    // down, and this runs during thread start where exceptions are unwelcome.
  }
}

} // namespace conan::detail

#endif // CONAN_DETAIL_THREAD_NAME_H_
