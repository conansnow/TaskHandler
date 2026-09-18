# Compiled twice, once against each way of consuming the library, for the same
# reason the test suite is. The compiled binary keeps the historical name
# `task_handler_example` so the example comment and the consumer exclude list
# stay accurate.
foreach(flavour header_only task_handler)
    if(flavour STREQUAL "header_only")
        set(target task_handler_example_header_only)
    else()
        set(target task_handler_example)
    endif()
    add_executable(${target} basic.cc)
    taskhandler_set_warnings(${target})
    target_link_libraries(${target} PRIVATE TaskHandler::${flavour})
endforeach()
