add_executable(core_tests
    test_main.cpp
    filesystem/test_FileInfo.cpp
    filesystem/test_IconCache.cpp
    operations/test_FileOperations.cpp
    config/test_SessionManager.cpp
    config/test_Settings.cpp
)

target_link_libraries(core_tests PRIVATE
    core
    gtest
    Qt5::Widgets
    Qt5::Core
)

add_test(NAME core_tests COMMAND core_tests)
