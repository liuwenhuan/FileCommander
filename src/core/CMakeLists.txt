add_library(core STATIC
    filesystem/FileInfo.h
    filesystem/FileInfo.cpp
    filesystem/FileSystemModel.h
    filesystem/FileSystemModel.cpp
    filesystem/FileProvider.h
    filesystem/LocalFileProvider.h
    filesystem/LocalFileProvider.cpp
    filesystem/IconCache.h
    filesystem/IconCache.cpp
    operations/FileOpTypes.h
    operations/FileOperations.h
    operations/FileOperations.cpp
    operations/OperationQueue.h
    operations/OperationQueue.cpp
    operations/FileSplitter.h
    operations/FileSplitter.cpp
    config/Settings.h
    config/Settings.cpp
    config/SessionManager.h
    config/SessionManager.cpp
    sync/DirectorySync.h
    sync/DirectorySync.cpp
    compare/TextDiff.h
    compare/TextDiff.cpp
    network/GvfsMounter.h
    network/GvfsMounter.cpp
    network/SftpProvider.h
    network/SftpProvider.cpp
)

# libssh2 powers the SFTP file backend (network/SftpProvider).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH2 REQUIRED libssh2)

target_include_directories(core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/filesystem
    ${CMAKE_CURRENT_SOURCE_DIR}/operations
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/sync
    ${CMAKE_CURRENT_SOURCE_DIR}/compare
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${LIBSSH2_INCLUDE_DIRS}
)

target_link_libraries(core PUBLIC
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Concurrent
    ${LIBSSH2_LIBRARIES}
)
