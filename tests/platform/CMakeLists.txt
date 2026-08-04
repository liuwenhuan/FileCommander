add_executable(privilege_tests
    test_PrivilegedFileOperation.cpp
)

target_link_libraries(privilege_tests PRIVATE
    platform
    GTest::gtest
    GTest::gtest_main
    Qt5::Core
)

add_test(NAME privilege_tests COMMAND privilege_tests)
filecommander_set_test_runtime(privilege_tests)

add_executable(linux_privilege_broker_tests
    test_LinuxPrivilegeBroker.cpp
)

target_link_libraries(linux_privilege_broker_tests PRIVATE
    platform
    GTest::gtest
    GTest::gtest_main
    Qt5::Core
)

add_test(NAME linux_privilege_broker_tests COMMAND linux_privilege_broker_tests)
filecommander_set_test_runtime(linux_privilege_broker_tests)

if(WIN32)
    add_executable(windows_privilege_broker_tests
        test_WindowsPrivilegeBroker.cpp
    )

    target_link_libraries(windows_privilege_broker_tests PRIVATE
        platform
        GTest::gtest
        GTest::gtest_main
        Qt5::Core
    )

    add_test(NAME windows_privilege_broker_tests COMMAND windows_privilege_broker_tests)
    filecommander_set_test_runtime(windows_privilege_broker_tests)
endif()

add_executable(open_with_tests
    test_OpenWithHandlers.cpp
    test_FileHoles.cpp
)

target_link_libraries(open_with_tests PRIVATE
    platform
    GTest::gtest
    GTest::gtest_main
    Qt5::Core
)

add_test(NAME open_with_tests COMMAND open_with_tests)
filecommander_set_test_runtime(open_with_tests)
