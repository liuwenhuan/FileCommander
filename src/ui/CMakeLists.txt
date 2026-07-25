add_library(ui STATIC
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
    MpvWidget.h
    MpvWidget.cpp
    AudioPlayer.h
    AudioPlayer.cpp
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
    dialogs/ConnectDialog.h
    dialogs/ConnectDialog.cpp
    dialogs/CommandOutputDialog.h
    dialogs/CommandOutputDialog.cpp
    dialogs/ChecksumDialog.h
    dialogs/ChecksumDialog.cpp
    dialogs/SecureWipeDialog.h
    dialogs/SecureWipeDialog.cpp
    dialogs/TransferProgressDialog.h
    dialogs/TransferProgressDialog.cpp
    dialogs/ExternalConnectDialog.h
    dialogs/ExternalConnectDialog.cpp
    dialogs/AboutDialog.h
    dialogs/AboutDialog.cpp
    dialogs/UpdateDialog.h
    dialogs/UpdateDialog.cpp
)

# zlib provides crc32() for the checksum dialog. It is already pulled in
# transitively (libarchive/Qt) but link it explicitly so <zlib.h> resolves.
find_package(ZLIB REQUIRED)

# libmpv (render API) for the QuickView video preview page.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

# poppler-qt5 for the QuickView PDF preview page. The .pc exposes
# -I/usr/include/poppler/qt5 so <poppler-qt5.h> resolves directly.
pkg_check_modules(POPPLER_QT5 REQUIRED poppler-qt5)

# QOpenGLWidget lives in the Qt5::OpenGL module; the top-level find_package does
# not request it, so pull it in here (bundled with qtbase5-dev).
find_package(Qt5 COMPONENTS OpenGL REQUIRED)

# Qt5::X11Extras (QX11Info) + libxcb: MainWindow sets the _NET_WM_OPAQUE_REGION
# property directly on its X window so the compositor can skip alpha-blending
# the opaque interior of the translucent (shadowed) frameless window.
find_package(Qt5 COMPONENTS X11Extras REQUIRED)
pkg_check_modules(XCB REQUIRED xcb)

target_include_directories(ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dialogs
    ${CMAKE_CURRENT_SOURCE_DIR}/theme
    ${CMAKE_CURRENT_SOURCE_DIR}/i18n
)
target_include_directories(ui PRIVATE ${MPV_INCLUDE_DIRS} ${POPPLER_QT5_INCLUDE_DIRS}
    ${XCB_INCLUDE_DIRS})

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
target_link_libraries(ui PRIVATE ${MPV_LIBRARIES} ${POPPLER_QT5_LIBRARIES}
    Qt5::X11Extras ${XCB_LIBRARIES} ZLIB::ZLIB)
target_compile_options(ui PRIVATE ${MPV_CFLAGS_OTHER} ${POPPLER_QT5_CFLAGS_OTHER})
