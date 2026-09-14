# A minimal downstream project, used by CI to prove that an installed
# TaskHandler can actually be found and linked, both ways round.

cmake_minimum_required(VERSION 3.22)

project(TaskHandlerConsumer LANGUAGES CXX)

find_package(TaskHandler 0.2 REQUIRED)

add_executable(consumer_compiled main.cc)
target_link_libraries(consumer_compiled PRIVATE TaskHandler::task_handler)

add_executable(consumer_header_only main.cc)
target_link_libraries(consumer_header_only PRIVATE TaskHandler::header_only)
