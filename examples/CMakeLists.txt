# Compiled twice, once against each way of consuming the library, for the same
# reason the test suite is. The compiled binary keeps the historical name
# `task_handler_example` so the example comment and the consumer exclude list
# stay accurate.
foreach(example basic thread_pool)
    foreach(flavour header_only task_handler)
        if(example STREQUAL "basic" AND flavour STREQUAL "task_handler")
            set(target task_handler_example)
        elseif(example STREQUAL "basic")
            set(target task_handler_example_header_only)
        elseif(flavour STREQUAL "task_handler")
            set(target ${example}_example)
        else()
            set(target ${example}_example_header_only)
        endif()
        add_executable(${target} ${example}.cc)
        taskhandler_set_warnings(${target})
        target_link_libraries(${target} PRIVATE TaskHandler::${flavour})
    endforeach()
endforeach()
