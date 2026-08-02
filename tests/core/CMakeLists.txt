set(CORE_TEST_SOURCES
    test_main.cpp
    test_Phosphor.cpp
    filesystem/test_FileInfo.cpp
    filesystem/test_FileSystemModel.cpp
    filesystem/test_DirectorySizeTask.cpp
    filesystem/test_IconCache.cpp
    filesystem/test_ProviderPath.cpp
    filesystem/test_ProviderLocality.cpp
    filesystem/test_ExternalPaths.cpp
    filesystem/test_ParentEntry.cpp
    operations/test_FileOperations.cpp
    operations/test_OperationError.cpp
    operations/test_OperationQueue.cpp
    operations/test_ProviderTransfer.cpp
    operations/test_ServerSideMove.cpp
    operations/test_PreserveMtime.cpp
    operations/test_ConflictSizes.cpp
    config/test_SessionManager.cpp
    config/test_Settings.cpp
    notepad/test_NotepadStore.cpp
    sync/test_DirectorySync.cpp
    sync/test_SyncScanner.cpp
    compare/test_TextDiff.cpp
    diagnostics/test_RuntimeCounters.cpp
    platform/test_PathSemantics.cpp
    platform/test_FeatureCapabilities.cpp
    platform/test_FolderArguments.cpp
    platform/test_InstanceCoordinator.cpp
)

if(FILECOMMANDER_ENABLE_NETWORK)
    list(APPEND CORE_TEST_SOURCES
        platform/test_CredentialStore.cpp
        network/test_SftpProvider.cpp
        network/test_CurlFtpProvider.cpp
        network/test_CurlWebDavProvider.cpp
        network/test_TransferErrorMapping.cpp
        network/test_WebDavUpload.cpp
        network/test_WebDavChunkedUpload.cpp
        network/test_NetworkSession.cpp
    )
    if(NOT WIN32)
        list(APPEND CORE_TEST_SOURCES network/test_ConnectionPool.cpp)
    else()
        list(APPEND CORE_TEST_SOURCES
            network/test_WindowsSmbProvider.cpp
            network/test_SmbHostBrowser.cpp
        )
    endif()
endif()
if(WIN32)
    list(APPEND CORE_TEST_SOURCES
        devices/test_WindowsRemovableStorage.cpp
    )
endif()
if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    list(APPEND CORE_TEST_SOURCES
        network/test_GvfsMounter.cpp
        network/test_SmbClientGate.cpp
        network/test_SmbHelperProtocol.cpp
    )
endif()

add_executable(core_tests ${CORE_TEST_SOURCES})

target_link_libraries(core_tests PRIVATE
    core
    ui
    GTest::gtest
    Qt5::Widgets
    Qt5::Core
    Qt5::Test
)

add_test(NAME core_tests COMMAND core_tests)
filecommander_set_test_runtime(core_tests)

if(TTC_BUILD_LIVE_SMB_TESTS)
    add_executable(smb_live_tests
        test_main.cpp
        network/test_SmbLive.cpp
    )

    target_link_libraries(smb_live_tests PRIVATE
        core
        GTest::gtest
        Qt5::Widgets
        Qt5::Core
        Qt5::Test
    )
    add_dependencies(smb_live_tests FileCommander-smb-helper)
    set_target_properties(smb_live_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    add_test(NAME smb_live_tests COMMAND $<TARGET_FILE:smb_live_tests>)
    filecommander_set_test_runtime(smb_live_tests)
    set_tests_properties(smb_live_tests PROPERTIES
        LABELS "live;smb"
        RUN_SERIAL TRUE
        TIMEOUT 1800
    )
endif()

if(TTC_BUILD_LIVE_WEBDAV_TESTS)
    add_executable(webdav_live_tests
        test_main.cpp
        network/test_WebDavLive.cpp
    )

    target_link_libraries(webdav_live_tests PRIVATE
        core
        GTest::gtest
        Qt5::Widgets
        Qt5::Core
        Qt5::Test
    )

    add_test(NAME webdav_live_tests COMMAND $<TARGET_FILE:webdav_live_tests>)
    filecommander_set_test_runtime(webdav_live_tests)
    set_tests_properties(webdav_live_tests PROPERTIES
        LABELS "live;webdav"
        RUN_SERIAL TRUE
        TIMEOUT 900
    )
endif()
