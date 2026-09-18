// The compiled-library translation unit for ThreadPool. Header-only builds
// inline the same definitions from detail/thread_pool-inl.h instead, so the
// two modes cannot drift apart.

#include "conan/thread_pool.h"

#include "conan/detail/thread_pool-inl.h"
