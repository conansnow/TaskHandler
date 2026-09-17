// The one translation unit of the compiled-library build. Everything it
// contains lives in detail/task_handler-inl.h, which the header-only build
// inlines instead, so the two modes cannot drift apart.

#include "conan/task_handler.h"

#include "conan/detail/task_handler-inl.h"
