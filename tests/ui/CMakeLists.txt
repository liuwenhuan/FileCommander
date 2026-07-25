add_executable(ui_tests
    test_main.cpp
    test_TabManager.cpp
    test_PropertiesDialog.cpp
    test_CommandHistory.cpp
    test_TextViewer.cpp
    test_SeekSlider.cpp
    test_RemoteThumbnailFetcher.cpp
    test_RemoteThumbnail.cpp
)

# test_main.cpp supplies main() (with a QApplication), so gtest_main is not linked.
target_link_libraries(ui_tests PRIVATE
    ui
    gtest
    Qt5::Core
    Qt5::Widgets
    Qt5::Test
)

add_test(NAME ui_tests COMMAND ui_tests)
