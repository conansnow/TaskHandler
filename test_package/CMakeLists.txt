# Mirrors ci/consumer: both exported targets, C++23, no extra options.
cmake_minimum_required(VERSION 3.28)
project(TaskHandlerTestPackage LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(TaskHandler 0.3 REQUIRED)

add_executable(consumer_compiled src/example.cpp)
target_link_libraries(consumer_compiled PRIVATE TaskHandler::task_handler)

add_executable(consumer_header_only src/example.cpp)
target_link_libraries(consumer_header_only PRIVATE TaskHandler::header_only)
