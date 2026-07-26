add_executable(ui_tests
    test_main.cpp
    test_TabManager.cpp
    test_PropertiesDialog.cpp
    test_CommandHistory.cpp
    test_TextViewer.cpp
    test_SeekSlider.cpp
    test_RemoteThumbnailFetcher.cpp
    test_RemoteThumbnail.cpp
    test_ExifThumbnail.cpp
    test_VideoFrameQuality.cpp
    test_ThumbnailSweep.cpp
    test_ThumbnailSweepDrive.cpp
    test_Mp4RangePlan.cpp
    test_IconFileView.cpp
    test_VideoRangePlan.cpp
    test_FilePanelFont.cpp
    test_TreeRootBuilder.cpp
    test_DirectoryTreeModel.cpp
)

# resources.qrc is compiled into the app target, not this one, so a test that
# needs a real theme stylesheet reads it from the source tree instead.
target_compile_definitions(ui_tests PRIVATE TTC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# test_main.cpp supplies main() (with a QApplication), so gtest_main is not linked.
target_link_libraries(ui_tests PRIVATE
    ui
    gtest
    Qt5::Core
    Qt5::Widgets
    Qt5::Test
)

add_test(NAME ui_tests COMMAND ui_tests)
