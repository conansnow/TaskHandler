# A minimal downstream project, used by CI to prove that TaskHandler can
# actually be found and linked, for both exported targets and both of the ways
# the README tells people to depend on it.
#
#   -DTASKHANDLER_CONSUMER_MODE=find_package  consumes an install tree
#   -DTASKHANDLER_CONSUMER_MODE=fetchcontent  builds it as a subproject
#
# The second mode covers add_subdirectory() as well, since FetchContent ends up
# calling it, and it is the mode that catches a project that quietly builds its
# own tests or install rules inside somebody else's build.

cmake_minimum_required(VERSION 3.22)

project(TaskHandlerConsumer LANGUAGES CXX)

set(TASKHANDLER_CONSUMER_MODE "find_package" CACHE STRING
    "How this project depends on TaskHandler")
set_property(CACHE TASKHANDLER_CONSUMER_MODE PROPERTY STRINGS
    find_package fetchcontent)

if(TASKHANDLER_CONSUMER_MODE STREQUAL "find_package")
    find_package(TaskHandler 0.2 REQUIRED)
elseif(TASKHANDLER_CONSUMER_MODE STREQUAL "fetchcontent")
    include(FetchContent)
    FetchContent_Declare(TaskHandler
        GIT_REPOSITORY https://github.com/conansnow/TaskHandler.git
        GIT_TAG v0.2.0
        )
    # CI sets FETCHCONTENT_SOURCE_DIR_TASKHANDLER to the checkout, so the job
    # tests the tree it was run against rather than whatever the tag holds.
    FetchContent_MakeAvailable(TaskHandler)

    # Tests, examples, benchmarks and install rules default to on only when
    # TaskHandler is the top-level project. A consumer that pays for any of them
    # by accident would find out from its own build times, not from a message.
    foreach(unwanted task_handler_test_task_handler
            task_handler_test_header_only task_handler_example
            task_handler_example_header_only task_handler_benchmark)
        if(TARGET ${unwanted})
            message(FATAL_ERROR
                "TaskHandler built its own ${unwanted} inside a consumer build")
        endif()
    endforeach()
else()
    message(FATAL_ERROR
        "unknown TASKHANDLER_CONSUMER_MODE '${TASKHANDLER_CONSUMER_MODE}': "
        "expected find_package or fetchcontent")
endif()

add_executable(consumer_compiled main.cc)
target_link_libraries(consumer_compiled PRIVATE TaskHandler::task_handler)

add_executable(consumer_header_only main.cc)
target_link_libraries(consumer_header_only PRIVATE TaskHandler::header_only)
