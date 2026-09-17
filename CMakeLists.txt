cmake_minimum_required(VERSION 3.22)

project(TaskHandler
    VERSION 0.2.0
    DESCRIPTION "A small single-worker task queue and event handler for C++17"
    HOMEPAGE_URL "https://github.com/conansnow/TaskHandler"
    LANGUAGES CXX
    )

option(TASKHANDLER_BUILD_TESTS "Build the TaskHandler test suite" ${PROJECT_IS_TOP_LEVEL})
option(TASKHANDLER_BUILD_EXAMPLES "Build the TaskHandler examples" ${PROJECT_IS_TOP_LEVEL})
option(TASKHANDLER_BUILD_BENCHMARKS "Build the TaskHandler benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(TASKHANDLER_INSTALL "Generate the TaskHandler install rules" ${PROJECT_IS_TOP_LEVEL})
option(TASKHANDLER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# The library needs C++17 and says so through target_compile_features, so this
# only picks the standard the project's own tests, examples and benchmarks are
# built with. Left overridable so that CI can rebuild the whole thing as C++20
# or C++23 and catch anything a future standard breaks for consumers.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The version is spelled out in the public header as well, because a
# header-only consumer has no CMake project to ask. Checking them against each
# other here means the two cannot drift apart unnoticed.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/include/conan/task_handler.h TASKHANDLER_HEADER_TEXT)
foreach(component MAJOR MINOR PATCH)
    if(NOT TASKHANDLER_HEADER_TEXT MATCHES "#define TASKHANDLER_VERSION_${component} ([0-9]+)")
        message(FATAL_ERROR "TASKHANDLER_VERSION_${component} is missing from include/conan/task_handler.h")
    endif()
    if(NOT "${CMAKE_MATCH_1}" STREQUAL "${PROJECT_VERSION_${component}}")
        message(FATAL_ERROR
            "version mismatch: include/conan/task_handler.h says "
            "TASKHANDLER_VERSION_${component} ${CMAKE_MATCH_1}, project() says "
            "${PROJECT_VERSION_${component}}")
    endif()
endforeach()
if(NOT TASKHANDLER_HEADER_TEXT MATCHES "#define TASKHANDLER_VERSION_STRING \"${PROJECT_VERSION}\"")
    message(FATAL_ERROR
        "version mismatch: TASKHANDLER_VERSION_STRING in "
        "include/conan/task_handler.h is not \"${PROJECT_VERSION}\"")
endif()

include(GNUInstallDirs)

# std::thread needs an explicit -pthread on most Unix toolchains. It only
# happens to link without it on glibc 2.34 and newer, where libpthread was
# folded into libc.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(MSVC)
    # C4251 and C4275 fire on every standard-library member and base of a
    # dllexported class. Hiding the members behind a pimpl to silence them
    # would cost an allocation and an indirection per handler for no benefit,
    # since the library and its consumers are built with the same toolchain.
    set(TASKHANDLER_WARNING_FLAGS /W4 /permissive- /wd4251 /wd4275)
    set(TASKHANDLER_WERROR_FLAG /WX)
else()
    set(TASKHANDLER_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
    set(TASKHANDLER_WERROR_FLAG -Werror)
endif()

# Applies the project's own warning settings without pushing them onto anyone
# who merely consumes the library.
function(taskhandler_set_warnings target)
    target_compile_options(${target} PRIVATE ${TASKHANDLER_WARNING_FLAGS})
    if(TASKHANDLER_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE ${TASKHANDLER_WERROR_FLAG})
    endif()
endfunction()

set(TASKHANDLER_HEADERS
    include/conan/task_handler.h
    include/conan/detail/task_handler-inl.h
    )

# ---------------------------------------------------------------------------
# Header-only usage: TaskHandler::header_only
# ---------------------------------------------------------------------------
add_library(task_handler_header_only INTERFACE)
add_library(TaskHandler::header_only ALIAS task_handler_header_only)
# Without EXPORT_NAME the installed target would be
# TaskHandler::task_handler_header_only, so the name would differ depending on
# whether the project is consumed from the build tree or from an install.
set_target_properties(task_handler_header_only PROPERTIES EXPORT_NAME header_only)
target_compile_features(task_handler_header_only INTERFACE cxx_std_17)
target_include_directories(task_handler_header_only INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
target_link_libraries(task_handler_header_only INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Compiled usage: TaskHandler::task_handler
# ---------------------------------------------------------------------------
add_library(task_handler src/task_handler.cc ${TASKHANDLER_HEADERS})
add_library(TaskHandler::task_handler ALIAS task_handler)
taskhandler_set_warnings(task_handler)

target_compile_features(task_handler PUBLIC cxx_std_17)
target_include_directories(task_handler PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
target_link_libraries(task_handler PUBLIC Threads::Threads)

# These select the header's compiled-library mode, so they are part of the ABI
# contract and have to reach consumers rather than staying private.
target_compile_definitions(task_handler PUBLIC TASKHANDLER_COMPILED_LIB)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(task_handler PUBLIC TASKHANDLER_SHARED_LIB)
    target_compile_definitions(task_handler PRIVATE TASKHANDLER_EXPORTS)
endif()

set_target_properties(task_handler PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    # Without these the visibility attributes in the header are a no-op and the
    # shared object exports every internal template instantiation.
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    )

if(TASKHANDLER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TASKHANDLER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(TASKHANDLER_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(TASKHANDLER_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS task_handler task_handler_header_only
        EXPORT TaskHandlerTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    install(DIRECTORY include/conan
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    install(EXPORT TaskHandlerTargets
        NAMESPACE TaskHandler::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TaskHandler
        )

    configure_package_config_file(
        cmake/TaskHandlerConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/TaskHandlerConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TaskHandler
        )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/TaskHandlerConfigVersion.cmake
        COMPATIBILITY SameMajorVersion
        )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/TaskHandlerConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/TaskHandlerConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TaskHandler
        )
endif()
