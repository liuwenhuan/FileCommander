add_library(viewer STATIC
    TextViewer.h
    TextViewer.cpp
)

target_include_directories(viewer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(viewer PUBLIC
    Qt5::Widgets
)
