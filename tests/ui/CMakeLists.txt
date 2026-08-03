set(UI_TEST_SOURCES
    test_main.cpp
    test_TabManager.cpp
    test_BreadcrumbBar.cpp
    test_PropertiesDialog.cpp
    test_CommandHistory.cpp
    test_TextViewer.cpp
    test_TextEncodingDetector.cpp
    test_SeekSlider.cpp
    test_RemoteThumbnailFetcher.cpp
    test_RemoteThumbnail.cpp
    test_ThumbnailAsyncDiskHit.cpp
    test_ImagePreviewLoader.cpp
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
    test_MediaEngineContract.cpp
    test_FilePanelFont.cpp
    test_TabSessionLifecycle.cpp
    test_NetworkArchiveBrowse.cpp
    test_FilePanelTreeSync.cpp
    test_TitleBarTheme.cpp
    test_TreeRootBuilder.cpp
    test_DirectoryTreeModel.cpp
    test_SlideSceneImageCache.cpp
    test_SearchEngine.cpp
    test_SearchDialogKeys.cpp
    test_DragPayload.cpp
    test_DragMotion.cpp
    test_TreeNavigation.cpp
    test_PanelDiskInfo.cpp
    test_PanelRemoval.cpp
    test_OverwritePrompt.cpp
    test_ProviderChecksum.cpp
    test_SelectedEntryInfos.cpp
    test_QuickViewLazyPages.cpp
    test_QuickViewMotion.cpp
    test_MainWindowPreviewSwap.cpp
    test_DeleteConfirmDialog.cpp
    test_MainWindowActions.cpp
    test_UpdateUi.cpp
    test_WindowActivation.cpp
    test_FilePanelTheme.cpp
    test_TabMotion.cpp
    test_FileListViewLayout.cpp
    test_TextEncodingDetector.cpp
    test_ChecksumDialog.cpp
    test_NotepadPanel.cpp
    test_ExternalConnectDialog.cpp
    test_ShortcutsDialog.cpp
    test_StandardButtonLocalization.cpp
    test_TranslationCatalogs.cpp
    test_ComputerPanel.cpp
    test_ChromeTypography.cpp
    test_CrtContentSurfaces.cpp
    test_MotionPolicy.cpp
    test_DirectoryTreeMotion.cpp
    test_NetworkStateMotion.cpp
    test_OperationProgressMotion.cpp
    test_OperationErrorDialog.cpp
    ../archive/test_CompressDialog.cpp
    ../../resources/resources.qrc
)

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION OR WIN32)
    list(APPEND UI_TEST_SOURCES test_SecureWipeConfirmationDialog.cpp)
endif()

# Mirrors the WIN32 guard src/ui puts on media/WindowsMediaSurface.cpp itself.
if(WIN32)
    list(APPEND UI_TEST_SOURCES test_WindowsMediaSurface.cpp)
endif()

if(FILECOMMANDER_MEDIA_BACKEND_MPV)
    list(APPEND UI_TEST_SOURCES
        test_MpvMediaEngine.cpp
        test_MpvStreamSource.cpp
    )
endif()

if(FILECOMMANDER_ENABLE_NETWORK)
    list(APPEND UI_TEST_SOURCES test_ConnectDialog.cpp)
endif()

add_executable(ui_tests ${UI_TEST_SOURCES})

if(FILECOMMANDER_MEDIA_BACKEND_WINDOWSMF)
    target_sources(ui_tests PRIVATE test_WindowsMediaEngine.cpp)
endif()

add_executable(quickview_office_fixture fixtures/office_fixture_cli.cpp)
add_dependencies(ui_tests quickview_office_fixture)

# test_MpvStreamSource drives a real libmpv core (headless: vo=null/ao=null) to
# measure what a streamed preview actually reads. src/ui keeps mpv PRIVATE, so
# this target resolves <mpv/client.h> for itself.
if(FILECOMMANDER_MEDIA_BACKEND_MPV)
    target_link_libraries(ui_tests PRIVATE FileCommander::Mpv)
endif()

# resources.qrc is compiled into the app target, not this one, so a test that
# needs a real theme stylesheet reads it from the source tree instead.
target_compile_definitions(ui_tests PRIVATE
    TTC_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TTC_OFFICE_FIXTURE_CLI="$<TARGET_FILE:quickview_office_fixture>"
)

if(FILECOMMANDER_MEDIA_BACKEND_WINDOWSMF)
    set(FILECOMMANDER_WMF_FIXTURE_DIR "${CMAKE_BINARY_DIR}/wmf-fixtures-local-av")
    if(EXISTS "${CMAKE_BINARY_DIR}/../wmf-fixtures-local-av")
        set(FILECOMMANDER_WMF_FIXTURE_DIR "${CMAKE_BINARY_DIR}/../wmf-fixtures-local-av")
    endif()
    target_compile_definitions(ui_tests PRIVATE
        TTC_WMF_FIXTURE_DIR="${FILECOMMANDER_WMF_FIXTURE_DIR}"
    )
endif()

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
