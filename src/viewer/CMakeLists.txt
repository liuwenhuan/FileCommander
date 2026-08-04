add_library(viewer STATIC
    TextEditor.h
    TextEditor.cpp
    HexEditor.h
    HexEditor.cpp
    BinarySniff.h
    BinarySniff.cpp
    ImageViewer.h
    ImageViewer.cpp
    OfficeConverter.h
    OfficeConverter.cpp
)

target_include_directories(viewer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(viewer PUBLIC
    # core, for text/TextEncodingDetector: the F4 editor must detect a file's
    # encoding exactly the way the QuickView preview pane does. core is below
    # viewer in the layer order, so this respects the dependency direction.
    core
    widgets
    Qt5::Widgets
)
