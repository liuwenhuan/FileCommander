set(UI_SOURCES
    AppIcon.h
    AppIcon.cpp
    Typography.h
    Typography.cpp
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
    ImagePreviewLoader.h
    ImagePreviewLoader.cpp
    QuickView.h
    QuickView.cpp
    media/MediaEngine.h
    media/MediaTypes.h
    media/SeekWatchdog.h
    media/SeekWatchdog.cpp
    TextEncodingDetector.h
    TextEncodingDetector.cpp
    SlideSceneBuilder.h
    SlideSceneBuilder.cpp
    ViewerWindow.h
    ViewerWindow.cpp
    WindowActivation.h
    WindowActivation.cpp
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
    dialogs/OperationErrorDialog.h
    dialogs/OperationErrorDialog.cpp
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
    dialogs/DirectoryStatisticsTask.h
    dialogs/DeleteConfirmDialog.cpp
    dialogs/DirectoryStatisticsTask.cpp
    dialogs/CommandOutputDialog.h
    dialogs/CommandOutputDialog.cpp
    dialogs/ChecksumDialog.h
    dialogs/ChecksumDialog.cpp
    dialogs/TransferProgressDialog.h
    dialogs/TransferProgressDialog.cpp
    dialogs/AboutDialog.h
    dialogs/AboutDialog.cpp
)

if(WIN32)
    list(APPEND UI_SOURCES
        media/WindowsMediaSurface.h
        media/WindowsMediaSurface.cpp
    )
endif()

if(FILECOMMANDER_MEDIA_BACKEND_WINDOWSMF)
    list(APPEND UI_SOURCES
        media/WindowsMediaEngine.h
        media/WindowsMediaEngine.cpp
    )
endif()

if(FILECOMMANDER_MEDIA_BACKEND_MPV)
    list(APPEND UI_SOURCES
        media/MpvMediaEngine.h
        media/MpvMediaEngine.cpp
        media/MpvVideoSurface.h
        media/MpvVideoSurface.cpp
        MpvStreamSource.h
        MpvStreamSource.cpp
    )
else()
    list(APPEND UI_SOURCES
        media/NullMediaEngine.h
        media/NullMediaEngine.cpp
        MpvStreamSource.h
        MpvStreamSourceDisabled.cpp
    )
endif()

if(FILECOMMANDER_ENABLE_NETWORK)
    list(APPEND UI_SOURCES
        dialogs/ConnectDialog.h
        dialogs/ConnectDialog.cpp
    )
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION OR (WIN32 AND FILECOMMANDER_ENABLE_NETWORK))
    list(APPEND UI_SOURCES
        dialogs/ExternalConnectDialog.h
        dialogs/ExternalConnectDialog.cpp
    )
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION OR WIN32)
    list(APPEND UI_SOURCES
        dialogs/SecureWipeConfirmationDialog.h
        dialogs/SecureWipeConfirmationDialog.cpp
        dialogs/SecureWipeDialog.h
        dialogs/SecureWipeDialog.cpp
        dialogs/UpdateDialog.h
        dialogs/UpdateDialog.cpp
    )
endif()

add_library(ui STATIC ${UI_SOURCES})

find_package(ZLIB REQUIRED)
find_package(Qt5 COMPONENTS OpenGL REQUIRED)

if(FILECOMMANDER_MEDIA_BACKEND_MPV)
    if(WIN32)
        set(FILECOMMANDER_MPV_ROOT "$ENV{FILECOMMANDER_MPV_ROOT}" CACHE PATH
            "Windows libmpv SDK containing include/mpv/client.h and libmpv.lib")
        if(NOT EXISTS "${FILECOMMANDER_MPV_ROOT}/include/mpv/client.h" OR
           NOT EXISTS "${FILECOMMANDER_MPV_ROOT}/libmpv.lib")
            message(FATAL_ERROR
                "FILECOMMANDER_PREVIEW_MEDIA requires FILECOMMANDER_MPV_ROOT "
                "to point to the prepared Windows libmpv SDK.")
        endif()
        add_library(FileCommander::Mpv SHARED IMPORTED GLOBAL)
        set_target_properties(FileCommander::Mpv PROPERTIES
            IMPORTED_IMPLIB "${FILECOMMANDER_MPV_ROOT}/libmpv.lib"
            IMPORTED_LOCATION "${FILECOMMANDER_MPV_ROOT}/libmpv-2.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${FILECOMMANDER_MPV_ROOT}/include"
        )
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(MPV REQUIRED IMPORTED_TARGET GLOBAL mpv)
        add_library(FileCommander::Mpv ALIAS PkgConfig::MPV)
    endif()
endif()

if(FILECOMMANDER_PREVIEW_PDF)
    if(WIN32)
        set(FILECOMMANDER_POPPLER_QT5_ROOT
            "$ENV{FILECOMMANDER_POPPLER_QT5_ROOT}" CACHE PATH
            "Poppler Qt5 installation built against the selected Qt 5 SDK")
        if(NOT FILECOMMANDER_POPPLER_QT5_ROOT AND DEFINED ENV{VCPKG_ROOT})
            cmake_path(GET ENV{VCPKG_ROOT} PARENT_PATH _fc_sdk_root)
            set(FILECOMMANDER_POPPLER_QT5_ROOT "${_fc_sdk_root}/poppler-qt5")
        endif()
        if(NOT EXISTS "${FILECOMMANDER_POPPLER_QT5_ROOT}/include/poppler/qt5/poppler-qt5.h")
            message(FATAL_ERROR
                "FILECOMMANDER_PREVIEW_PDF requires FILECOMMANDER_POPPLER_QT5_ROOT "
                "to point to a Poppler Qt5 build made against this Qt 5 SDK.")
        endif()
        add_library(FileCommander::PopplerQt5 SHARED IMPORTED GLOBAL)
        set_target_properties(FileCommander::PopplerQt5 PROPERTIES
            IMPORTED_IMPLIB "${FILECOMMANDER_POPPLER_QT5_ROOT}/lib/poppler-qt5.lib"
            IMPORTED_LOCATION "${FILECOMMANDER_POPPLER_QT5_ROOT}/bin/poppler-qt5.dll"
            INTERFACE_INCLUDE_DIRECTORIES
                "${FILECOMMANDER_POPPLER_QT5_ROOT}/include/poppler/qt5"
        )
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(POPPLER_QT5 REQUIRED IMPORTED_TARGET GLOBAL poppler-qt5)
        add_library(FileCommander::PopplerQt5 ALIAS PkgConfig::POPPLER_QT5)
    endif()
endif()

if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    find_package(Qt5 COMPONENTS X11Extras REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET GLOBAL xcb)
endif()

target_include_directories(ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dialogs
    ${CMAKE_CURRENT_SOURCE_DIR}/tree
    ${CMAKE_CURRENT_SOURCE_DIR}/theme
    ${CMAKE_CURRENT_SOURCE_DIR}/i18n
    ${CMAKE_CURRENT_SOURCE_DIR}/media
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

if(WIN32)
    # ThumbnailCache uses Media Foundation even when the optional playback
    # backend is disabled, so these Windows SDK libraries cannot be tied to
    # FILECOMMANDER_MEDIA_BACKEND_WINDOWSMF.
    target_link_libraries(ui PRIVATE
        shell32
        gdi32
        mf
        mfplat
        mfreadwrite
        mfuuid
        windowscodecs
    )
endif()
if(FILECOMMANDER_MEDIA_BACKEND_WINDOWSMF)
    target_link_libraries(ui PRIVATE ole32 oleaut32)
endif()

if(FILECOMMANDER_MEDIA_BACKEND_MPV)
    target_link_libraries(ui PRIVATE FileCommander::Mpv)
endif()
if(FILECOMMANDER_PREVIEW_PDF)
    target_link_libraries(ui PRIVATE FileCommander::PopplerQt5)
endif()
if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    target_link_libraries(ui PRIVATE Qt5::X11Extras PkgConfig::XCB)
endif()
