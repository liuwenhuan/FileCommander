add_library(ui STATIC
    MainWindow.h
    MainWindow.cpp
    BreadcrumbBar.h
    BreadcrumbBar.cpp
    FilePanel.h
    FilePanel.cpp
    FileListView.h
    FileListView.cpp
    CommandBar.h
    CommandBar.cpp
    CommandHistory.h
    CommandHistory.cpp
    QuickView.h
    QuickView.cpp
    MpvWidget.h
    MpvWidget.cpp
    FunctionKeyBar.h
    FunctionKeyBar.cpp
    StatusBarWidget.h
    StatusBarWidget.cpp
    TabManager.h
    TabManager.cpp
    TabBar.h
    TabBar.cpp
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
    dialogs/CompareDialog.h
    dialogs/CompareDialog.cpp
    dialogs/PropertiesDialog.h
    dialogs/PropertiesDialog.cpp
    dialogs/ConnectDialog.h
    dialogs/ConnectDialog.cpp
)

# libmpv (render API) for the QuickView video preview page.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

# QOpenGLWidget lives in the Qt5::OpenGL module; the top-level find_package does
# not request it, so pull it in here (bundled with qtbase5-dev).
find_package(Qt5 COMPONENTS OpenGL REQUIRED)

target_include_directories(ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dialogs
    ${CMAKE_CURRENT_SOURCE_DIR}/theme
    ${CMAKE_CURRENT_SOURCE_DIR}/i18n
)
target_include_directories(ui PRIVATE ${MPV_INCLUDE_DIRS})

target_link_libraries(ui PUBLIC
    core
    viewer
    archive
    search
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
    Qt5::OpenGL
)
target_link_libraries(ui PRIVATE ${MPV_LIBRARIES})
target_compile_options(ui PRIVATE ${MPV_CFLAGS_OTHER})
