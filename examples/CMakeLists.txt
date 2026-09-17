add_executable(task_handler_example basic.cc)
taskhandler_set_warnings(task_handler_example)
target_link_libraries(task_handler_example PRIVATE TaskHandler::task_handler)

# Compiled a second time against the header-only target. The example is the
# API's documentation, so it has to keep building both ways round for the same
# reason the test suite does.
add_executable(task_handler_example_header_only basic.cc)
taskhandler_set_warnings(task_handler_example_header_only)
target_link_libraries(task_handler_example_header_only PRIVATE TaskHandler::header_only)
