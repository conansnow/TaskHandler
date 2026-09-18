find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# The same suite is compiled twice, once against each way of consuming the
# library, so the header-only and compiled builds cannot silently diverge.
foreach(suite task_handler thread_pool)
    foreach(flavour header_only task_handler)
        set(target ${suite}_test_${flavour})
        add_executable(${target} ${suite}_test.cc)
        taskhandler_set_warnings(${target})
        target_link_libraries(${target} PRIVATE
            TaskHandler::${flavour}
            GTest::gtest_main
            )
        # gtest_discover_tests runs the exe at build time. A Windows shared
        # build puts task_handler.dll next to the library, not next to this
        # test, so the exe fails to start with 0xc0000135 unless the DLL is
        # copied first. The copy has to be registered before discovery so
        # both POST_BUILD steps run in that order.
        if(WIN32 AND BUILD_SHARED_LIBS AND flavour STREQUAL "task_handler")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:task_handler>
                    $<TARGET_FILE_DIR:${target}>
                )
        endif()
        gtest_discover_tests(${target}
            TEST_PREFIX "${flavour}."
            DISCOVERY_TIMEOUT 60
            )
    endforeach()
endforeach()
