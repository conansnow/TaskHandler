# A minimal downstream project, used by CI to prove that TaskHandler can
# actually be found and linked, for both exported targets and the ways
# the README tells people to depend on it.
#
#   -DTASKHANDLER_CONSUMER_MODE=find_package  consumes an install tree
#   -DTASKHANDLER_CONSUMER_MODE=fetchcontent  builds it as a subproject
#   -DTASKHANDLER_CONSUMER_MODE=pkgconfig     consumes the installed .pc files
#
# The fetchcontent mode covers add_subdirectory() as well, since FetchContent
# ends up calling it, and it is the mode that catches a project that quietly
# builds its own tests or install rules inside somebody else's build.
# pkgconfig needs PKG_CONFIG_PATH to include ${prefix}/lib/pkgconfig.

cmake_minimum_required(VERSION 3.28)

project(TaskHandlerConsumer LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(TASKHANDLER_CONSUMER_MODE "find_package" CACHE STRING
    "How this project depends on TaskHandler")
set_property(CACHE TASKHANDLER_CONSUMER_MODE PROPERTY STRINGS
    find_package fetchcontent pkgconfig)

if(TASKHANDLER_CONSUMER_MODE STREQUAL "find_package")
    find_package(TaskHandler 0.3 REQUIRED)
elseif(TASKHANDLER_CONSUMER_MODE STREQUAL "fetchcontent")
    include(FetchContent)
    FetchContent_Declare(TaskHandler
        GIT_REPOSITORY https://github.com/conansnow/TaskHandler.git
        GIT_TAG v0.3.0
        )
    # CI sets FETCHCONTENT_SOURCE_DIR_TASKHANDLER to the checkout, so the job
    # tests the tree it was run against rather than whatever the tag holds.
    FetchContent_MakeAvailable(TaskHandler)

    # Tests, examples, benchmarks and install rules default to on only when
    # TaskHandler is the top-level project. A consumer that pays for any of them
    # by accident would find out from its own build times, not from a message.
    foreach(unwanted task_handler_test_task_handler
            task_handler_test_header_only thread_pool_test_task_handler
            thread_pool_test_header_only task_handler_example
            task_handler_example_header_only thread_pool_example
            thread_pool_example_header_only task_handler_benchmark)
        if(TARGET ${unwanted})
            message(FATAL_ERROR
                "TaskHandler built its own ${unwanted} inside a consumer build")
        endif()
    endforeach()
elseif(TASKHANDLER_CONSUMER_MODE STREQUAL "pkgconfig")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TASKHANDLER REQUIRED IMPORTED_TARGET taskhandler)
    pkg_check_modules(TASKHANDLER_HEADER_ONLY REQUIRED IMPORTED_TARGET
        taskhandler-header-only)
else()
    message(FATAL_ERROR
        "unknown TASKHANDLER_CONSUMER_MODE '${TASKHANDLER_CONSUMER_MODE}': "
        "expected find_package, fetchcontent or pkgconfig")
endif()

add_executable(consumer_compiled main.cc)
add_executable(consumer_header_only main.cc)

if(TASKHANDLER_CONSUMER_MODE STREQUAL "pkgconfig")
    # .pc files do not encode cxx_std_23 the way the CMake targets do.
    target_link_libraries(consumer_compiled PRIVATE PkgConfig::TASKHANDLER)
    target_link_libraries(consumer_header_only PRIVATE
        PkgConfig::TASKHANDLER_HEADER_ONLY)
    target_compile_features(consumer_compiled PRIVATE cxx_std_23)
    target_compile_features(consumer_header_only PRIVATE cxx_std_23)
else()
    target_link_libraries(consumer_compiled PRIVATE TaskHandler::task_handler)
    target_link_libraries(consumer_header_only PRIVATE TaskHandler::header_only)
endif()
