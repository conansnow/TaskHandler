# Deliberately dependency-free: a queue whose whole point is to be small should
# not pull a benchmark framework into the build just to time itself, and the
# numbers that matter here are throughput and round-trip latency, both of which
# a loop and a steady_clock measure well enough to catch a regression.
add_executable(task_handler_benchmark task_handler_benchmark.cc)
taskhandler_set_warnings(task_handler_benchmark)
target_link_libraries(task_handler_benchmark PRIVATE TaskHandler::task_handler)
