add_library(ui STATIC
    MainWindow.h
    MainWindow.cpp
    FilePanel.h
    FilePanel.cpp
    FileListView.h
    FileListView.cpp
    FunctionKeyBar.h
    FunctionKeyBar.cpp
    StatusBarWidget.h
    StatusBarWidget.cpp
    TabManager.h
    TabManager.cpp
    TabBar.h
    TabBar.cpp
    dialogs/OperationProgressDialog.h
    dialogs/OperationProgressDialog.cpp
    dialogs/OverwriteConfirmDialog.h
    dialogs/OverwriteConfirmDialog.cpp
)

target_include_directories(ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/dialogs
)

target_link_libraries(ui PUBLIC
    core
    viewer
    archive
    search
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
)
