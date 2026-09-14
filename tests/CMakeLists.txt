find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# The same suite is compiled twice, once against each way of consuming the
# library, so the header-only and compiled builds cannot silently diverge.
foreach(flavour header_only task_handler)
    set(target task_handler_test_${flavour})
    add_executable(${target} task_handler_test.cc)
    taskhandler_set_warnings(${target})
    target_link_libraries(${target} PRIVATE
        TaskHandler::${flavour}
        GTest::gtest_main
        )
    gtest_discover_tests(${target}
        TEST_PREFIX "${flavour}."
        DISCOVERY_TIMEOUT 60
        )
endforeach()
