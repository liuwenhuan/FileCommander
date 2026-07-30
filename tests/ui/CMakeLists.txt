set(UI_TEST_SOURCES
    test_main.cpp
    test_TabManager.cpp
    test_PropertiesDialog.cpp
    test_CommandHistory.cpp
    test_TextViewer.cpp
    test_TextEncodingDetector.cpp
    test_SeekSlider.cpp
    test_RemoteThumbnailFetcher.cpp
    test_RemoteThumbnail.cpp
    test_ThumbnailAsyncDiskHit.cpp
    test_ExifThumbnail.cpp
    test_VideoFrameQuality.cpp
    test_ThumbnailSweep.cpp
    test_ThumbnailCacheReuse.cpp
    test_ThumbnailMemoryBudget.cpp
    test_ThumbnailCachePrune.cpp
    test_ThumbnailDiskFormat.cpp
    test_ThumbnailSweepDrive.cpp
    test_Mp4RangePlan.cpp
    test_IconFileView.cpp
    test_VideoRangePlan.cpp
    test_FilePanelFont.cpp
    test_TabSessionLifecycle.cpp
    test_NetworkArchiveBrowse.cpp
    test_FilePanelTreeSync.cpp
    test_TitleBarTheme.cpp
    test_TreeRootBuilder.cpp
    test_DirectoryTreeModel.cpp
    test_SlideSceneImageCache.cpp
    test_SearchEngine.cpp
    test_DragPayload.cpp
    test_TreeNavigation.cpp
    test_PanelDiskInfo.cpp
    test_PanelRemoval.cpp
    test_OverwritePrompt.cpp
    test_ProviderChecksum.cpp
    test_SelectedEntryInfos.cpp
    test_MainWindowPreviewSwap.cpp
    test_MainWindowActions.cpp
    test_FilePanelTheme.cpp
    test_FileListViewLayout.cpp
    test_TextEncodingDetector.cpp
    test_ChecksumDialog.cpp
    test_NotepadPanel.cpp
    test_ShortcutsDialog.cpp
    test_StandardButtonLocalization.cpp
    test_ChromeTypography.cpp
    test_MotionPolicy.cpp
    ../archive/test_CompressDialog.cpp
    ../../resources/resources.qrc
)

if(FILECOMMANDER_PREVIEW_MEDIA)
    list(APPEND UI_TEST_SOURCES test_MpvStreamSource.cpp)
endif()

if(FILECOMMANDER_ENABLE_NETWORK)
    list(APPEND UI_TEST_SOURCES test_ConnectDialog.cpp)
endif()

add_executable(ui_tests ${UI_TEST_SOURCES})

# test_MpvStreamSource drives a real libmpv core (headless: vo=null/ao=null) to
# measure what a streamed preview actually reads. src/ui keeps mpv PRIVATE, so
# this target resolves <mpv/client.h> for itself.
if(FILECOMMANDER_PREVIEW_MEDIA)
    target_link_libraries(ui_tests PRIVATE FileCommander::Mpv)
endif()

# resources.qrc is compiled into the app target, not this one, so a test that
# needs a real theme stylesheet reads it from the source tree instead.
target_compile_definitions(ui_tests PRIVATE TTC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# test_main.cpp supplies main() (with a QApplication), so gtest_main is not linked.
target_link_libraries(ui_tests PRIVATE
    ui
    GTest::gtest
    Qt5::Core
    Qt5::Widgets
    Qt5::Test
)

add_test(NAME ui_tests COMMAND ui_tests)
filecommander_set_test_runtime(ui_tests)
