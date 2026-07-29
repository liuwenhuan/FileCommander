add_executable(viewer_tests
    test_OfficeConverter.cpp
)

target_link_libraries(viewer_tests PRIVATE
    viewer
    GTest::gtest
    GTest::gtest_main
    Qt5::Core
)

add_test(NAME viewer_tests COMMAND viewer_tests)
filecommander_set_test_runtime(viewer_tests)
