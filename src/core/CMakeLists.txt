add_library(core STATIC
    filesystem/FileInfo.h
    filesystem/FileInfo.cpp
    filesystem/FileSystemModel.h
    filesystem/FileSystemModel.cpp
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
)

target_include_directories(core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/filesystem
    ${CMAKE_CURRENT_SOURCE_DIR}/operations
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/sync
    ${CMAKE_CURRENT_SOURCE_DIR}/compare
)

target_link_libraries(core PUBLIC
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Concurrent
)
