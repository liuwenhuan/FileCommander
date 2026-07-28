set(UI_SOURCES
    AppIcon.h
    AppIcon.cpp
    theme/PhosphorEffect.h
    theme/PhosphorEffect.cpp
    MainWindow.h
    MainWindow.cpp
    BreadcrumbBar.h
    BreadcrumbBar.cpp
    FilePanel.h
    FilePanel.cpp
    FileListView.h
    FileListView.cpp
    IconFileView.h
    IconFileView.cpp
    ThumbnailCache.h
    ThumbnailCache.cpp
    RemoteThumbnailFetcher.h
    RemoteThumbnailFetcher.cpp
    ExifThumbnail.h
    ExifThumbnail.cpp
    ThumbnailSweep.h
    ThumbnailSweep.cpp
    Mp4RangePlan.h
    Mp4RangePlan.cpp
    VideoRangePlan.h
    VideoRangePlan.cpp
    ThumbnailDelegate.h
    ThumbnailDelegate.cpp
    CommandBar.h
    CommandBar.cpp
    CommandHistory.h
    CommandHistory.cpp
    QuickView.h
    QuickView.cpp
    SlideSceneBuilder.h
    SlideSceneBuilder.cpp
    ViewerWindow.h
    ViewerWindow.cpp
    FunctionKeyBar.h
    FunctionKeyBar.cpp
    DragPixmap.h
    DragPixmap.cpp
    NotepadPanel.h
    NotepadPanel.cpp
    StatusBarWidget.h
    StatusBarWidget.cpp
    TabManager.h
    TabManager.cpp
    tree/TreeRootBuilder.h
    tree/TreeRootBuilder.cpp
    tree/TreeDirLister.h
    tree/TreeDirLister.cpp
    tree/DirectoryTreeModel.h
    tree/DirectoryTreeModel.cpp
    tree/NetworkTreeRegistry.h
    tree/NetworkTreeRegistry.cpp
    TabBar.h
    TabBar.cpp
    TitleBar.h
    TitleBar.cpp
    theme/ThemeManager.h
    theme/ThemeManager.cpp
    i18n/TranslationManager.h
    i18n/TranslationManager.cpp
    dialogs/OperationProgressDialog.h
    dialogs/OperationProgressDialog.cpp
    dialogs/OverwriteConfirmDialog.h
    dialogs/OverwriteConfirmDialog.cpp
    dialogs/ShortcutsDialog.h
    dialogs/ShortcutsDialog.cpp
    dialogs/MultiRenameDialog.h
    dialogs/MultiRenameDialog.cpp
    dialogs/SyncDialog.h
    dialogs/SyncDialog.cpp
    dialogs/SyncModel.h
    dialogs/SyncModel.cpp
    dialogs/SyncActionDelegate.h
    dialogs/SyncActionDelegate.cpp
    dialogs/CompareDialog.h
    dialogs/CompareDialog.cpp
    dialogs/PropertiesDialog.h
    dialogs/PropertiesDialog.cpp
    dialogs/CommandOutputDialog.h
    dialogs/CommandOutputDialog.cpp
    dialogs/ChecksumDialog.h
    dialogs/ChecksumDialog.cpp
    dialogs/TransferProgressDialog.h
    dialogs/TransferProgressDialog.cpp
    dialogs/AboutDialog.h
    dialogs/AboutDialog.cpp
)

if(FILECOMMANDER_PREVIEW_MEDIA)
    list(APPEND UI_SOURCES
        MpvWidget.h
        MpvWidget.cpp
        MpvStreamSource.h
        MpvStreamSource.cpp
        AudioPlayer.h
        AudioPlayer.cpp
    )
endif()

if(NOT FILECOMMANDER_PREVIEW_MEDIA)
    list(APPEND UI_SOURCES
        MediaPreviewStubs.cpp
        AudioPlayer.h
        MpvWidget.h
    )
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    list(APPEND UI_SOURCES
        dialogs/ConnectDialog.h
        dialogs/ConnectDialog.cpp
        dialogs/ExternalConnectDialog.h
        dialogs/ExternalConnectDialog.cpp
        dialogs/SecureWipeDialog.h
        dialogs/SecureWipeDialog.cpp
        dialogs/UpdateDialog.h
        dialogs/UpdateDialog.cpp
    )
endif()

add_library(ui STATIC ${UI_SOURCES})

find_package(ZLIB REQUIRED)
find_package(Qt5 COMPONENTS OpenGL REQUIRED)

if(FILECOMMANDER_PREVIEW_MEDIA)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
endif()

if(FILECOMMANDER_PREVIEW_PDF)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(POPPLER_QT5 REQUIRED IMPORTED_TARGET poppler-qt5)
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    find_package(Qt5 COMPONENTS X11Extras REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
endif()

target_include_directories(ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dialogs
    ${CMAKE_CURRENT_SOURCE_DIR}/tree
    ${CMAKE_CURRENT_SOURCE_DIR}/theme
    ${CMAKE_CURRENT_SOURCE_DIR}/i18n
)

target_link_libraries(ui PUBLIC
    core
    viewer
    archive
    search
    widgets
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
    Qt5::OpenGL
    Qt5::Concurrent
)
target_link_libraries(ui PRIVATE ZLIB::ZLIB)

if(FILECOMMANDER_PREVIEW_MEDIA)
    target_link_libraries(ui PRIVATE PkgConfig::MPV)
endif()
if(FILECOMMANDER_PREVIEW_PDF)
    target_link_libraries(ui PRIVATE PkgConfig::POPPLER_QT5)
endif()
if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    target_link_libraries(ui PRIVATE Qt5::X11Extras PkgConfig::XCB)
endif()
