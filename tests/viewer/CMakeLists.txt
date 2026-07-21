add_executable(viewer_tests
    test_OfficeConverter.cpp
)

target_link_libraries(viewer_tests PRIVATE
    viewer
    gtest
    gtest_main
    Qt5::Core
)

add_test(NAME viewer_tests COMMAND viewer_tests)
