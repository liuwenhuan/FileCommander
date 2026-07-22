add_library(viewer STATIC
    TextEditor.h
    TextEditor.cpp
    ImageViewer.h
    ImageViewer.cpp
    OfficeConverter.h
    OfficeConverter.cpp
)

target_include_directories(viewer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(viewer PUBLIC
    widgets
    Qt5::Widgets
)
