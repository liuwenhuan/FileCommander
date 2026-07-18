add_executable(core_tests
    filesystem/test_FileInfo.cpp
    operations/test_FileOperations.cpp
)

target_link_libraries(core_tests PRIVATE
    core
    gtest
    gtest_main
    Qt5::Core
)

add_test(NAME core_tests COMMAND core_tests)
