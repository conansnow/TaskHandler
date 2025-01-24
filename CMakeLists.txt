cmake_minimum_required(VERSION 3.22)

project(TaskHandler VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)


find_package(GTest CONFIG REQUIRED)


add_library(task_handler SHARED
    src/task_handler.cc
    )
target_include_directories(task_handler PUBLIC
    inc
    )
target_compile_definitions(task_handler PRIVATE
    TASKHANDLER_COMPILED_LIB
    TASKHANDLER_SHARED_LIB
    TASKHANDLER_EXPORTS
    )

add_executable(task_handler_test
    src/googletest_nomain.cc
    )
target_compile_definitions(task_handler_test PRIVATE
    TASKHANDLER_COMPILED_LIB
    TASKHANDLER_SHARED_LIB
    )
target_link_libraries(task_handler_test
    task_handler
    GTest::gtest_main
    )

add_executable(task_handler_test1
    src/googletest_nomain.cc
    )
target_include_directories(task_handler_test1 PRIVATE
    inc
    )
target_link_libraries(task_handler_test1
    GTest::gtest_main
    )