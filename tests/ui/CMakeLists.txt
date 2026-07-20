add_executable(ui_tests
    test_TabManager.cpp
    test_PropertiesDialog.cpp
    test_CommandHistory.cpp
    test_TextViewer.cpp
)

target_link_libraries(ui_tests PRIVATE
    ui
    gtest
    gtest_main
    Qt5::Core
    Qt5::Widgets
)

add_test(NAME ui_tests COMMAND ui_tests)
