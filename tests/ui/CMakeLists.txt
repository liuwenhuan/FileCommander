set(UI_TEST_SOURCES
    test_main.cpp
    test_TabManager.cpp
    test_BreadcrumbBar.cpp
    test_PropertiesDialog.cpp
    test_AnimatedPreview.cpp
    test_MarkdownTables.cpp
    test_ThemedChromeArtwork.cpp
    test_CommandRegistry.cpp
    test_ScratchDirs.cpp
    test_CommandHistory.cpp
    test_TextViewer.cpp
    test_TextEncodingDetector.cpp
    test_SeekSlider.cpp
    test_ByteSearch.cpp
    test_FindBar.cpp
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
    test_ArchiveEntryIcons.cpp
    test_ComputerRename.cpp
    test_ThumbnailRenameEditor.cpp
    test_IconTinting.cpp
    test_TransferProgressLayout.cpp
    test_QuickViewControls.cpp
    test_ArchivePreviewAsync.cpp
    test_SeekWatchdog.cpp
    test_OpenWithMenu.cpp
    test_ShellShortcuts.cpp
    test_ThumbnailIconScaling.cpp
    test_VideoRangePlan.cpp
    test_MediaEngineContract.cpp
    test_FilePanelFont.cpp
    test_TabSessionLifecycle.cpp
    test_NetworkArchiveBrowse.cpp
    test_FilePanelTreeSync.cpp
    test_TitleBarAccount.cpp
    test_TitleBarTheme.cpp
    test_FramelessWindowChrome.cpp
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
    test_QuickViewInPlaceEdit.cpp
    test_QuickViewMarkdownToolbar.cpp
    test_QuickViewMotion.cpp
    test_MainWindowPreviewSwap.cpp
    test_DeleteConfirmDialog.cpp
    test_MainWindowActions.cpp
    test_UpdateUi.cpp
    test_WindowActivation.cpp
    test_FilePanelTheme.cpp
    test_TabMotion.cpp
    test_FileListViewLayout.cpp
    test_ChecksumDialog.cpp
    test_NotepadPanel.cpp
    test_ExternalConnectDialog.cpp
    test_AccountDialog.cpp
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
    test_DialogContentWidth.cpp
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

# Shared test helpers (tests/support): FC_TRY_* replaces Qt's QTRY_* macros,
# which cannot fail inside a gtest body -- see TryUntil.h. Unconditional: the
# first attempt landed inside the mpv block above, so every build without mpv
# failed to find the header.
target_include_directories(ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../support)

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

# So the crash handler in test_main.cpp can print function names rather than
# bare addresses: backtrace_symbols_fd reads the dynamic symbol table, and
# without -rdynamic a statically-linked-in symbol is simply not in it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(ui_tests PRIVATE -rdynamic)
endif()

# DbgHelp is what turns the addresses the crash reporter captures into
# function names and line numbers.
if(WIN32)
    target_link_libraries(ui_tests PRIVATE dbghelp)
endif()

add_test(NAME ui_tests COMMAND ui_tests)
filecommander_set_test_runtime(ui_tests)

# Right-click actions against a real server (SMB or WebDAV, whichever bookmark
# it is pointed at). Same gate as tests/core's live suites: nothing here runs,
# or is even built, without a server to point it at.
if(TTC_BUILD_LIVE_SMB_TESTS OR TTC_BUILD_LIVE_WEBDAV_TESTS)
    add_executable(remote_archive_live_tests
        test_main.cpp
        test_RemoteArchiveLive.cpp
        ../../resources/resources.qrc
    )
    target_include_directories(remote_archive_live_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../support)
    target_compile_definitions(remote_archive_live_tests PRIVATE TTC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(remote_archive_live_tests PRIVATE
        ui
        GTest::gtest
        Qt5::Core
        Qt5::Widgets
        Qt5::Test
    )
    add_dependencies(remote_archive_live_tests FileCommander-smb-helper)
    set_target_properties(remote_archive_live_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    add_test(NAME remote_archive_live_tests COMMAND $<TARGET_FILE:remote_archive_live_tests>)
    filecommander_set_test_runtime(remote_archive_live_tests)
    set_tests_properties(remote_archive_live_tests PROPERTIES
        LABELS "live;smb;webdav"
        RUN_SERIAL TRUE
        TIMEOUT 900
    )
endif()
