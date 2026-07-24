add_executable(core_tests
    test_main.cpp
    filesystem/test_FileInfo.cpp
    filesystem/test_FileSystemModel.cpp
    filesystem/test_IconCache.cpp
    operations/test_FileOperations.cpp
    operations/test_OperationQueue.cpp
    operations/test_FileSplitter.cpp
    operations/test_ProviderTransfer.cpp
    config/test_SessionManager.cpp
    config/test_Settings.cpp
    sync/test_DirectorySync.cpp
    compare/test_TextDiff.cpp
    network/test_GvfsMounter.cpp
    network/test_SftpProvider.cpp
    network/test_CurlFtpProvider.cpp
    network/test_CurlWebDavProvider.cpp
    network/test_NetworkSession.cpp
)

target_link_libraries(core_tests PRIVATE
    core
    gtest
    Qt5::Widgets
    Qt5::Core
    Qt5::Test
)

add_test(NAME core_tests COMMAND core_tests)
