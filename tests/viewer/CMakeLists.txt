# test_main.cpp supplies main() (with a QApplication), so gtest_main is not
# linked: the F4 editor tests instantiate real widgets and grab their pixels.
add_executable(viewer_tests
    test_AnimatedImage.cpp
    test_main.cpp
    test_OfficeConverter.cpp
    test_TextEditorTheme.cpp
    test_TextEditorEditing.cpp
    test_BinarySniff.cpp
    test_HexEditor.cpp
    test_TextEditorIntegration.cpp
    test_HexEditorChrome.cpp
)

# HexEditor is a real widget, so its tests need a real QApplication. It is
# created on first use inside test_HexEditor.cpp rather than from a custom
# main(), which keeps gtest_main and the widget-free tests in this binary
# unaffected.
target_link_libraries(viewer_tests PRIVATE
    viewer
    GTest::gtest
    Qt5::Core
    Qt5::Widgets
    # QTest::qWait, for the animation tests: a QMovie is driven by the event
    # loop, so watching one means pumping it.
    Qt5::Test
)

# resources.qrc is compiled into the app target, not this one, so the theme
# tests read the real stylesheets out of the source tree instead.
target_compile_definitions(viewer_tests PRIVATE
    TTC_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME viewer_tests COMMAND viewer_tests)
filecommander_set_test_runtime(viewer_tests)
