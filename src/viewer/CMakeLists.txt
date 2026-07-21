add_library(viewer STATIC
    TextViewer.h
    TextViewer.cpp
    TextEditor.h
    TextEditor.cpp
    ImageViewer.h
    ImageViewer.cpp
    OfficeConverter.h
    OfficeConverter.cpp
)

target_include_directories(viewer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# libarchive: OfficeConverter reads embedded images out of OOXML (docx/pptx/xlsx
# are zip containers) to inline them into the office_oxide HTML preview.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)

target_link_libraries(viewer PUBLIC
    Qt5::Widgets
)
target_include_directories(viewer PRIVATE ${LIBARCHIVE_INCLUDE_DIRS})
target_link_libraries(viewer PRIVATE ${LIBARCHIVE_LIBRARIES})
