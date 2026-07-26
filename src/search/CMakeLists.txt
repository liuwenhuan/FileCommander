add_library(search STATIC
    SearchEngine.h
    SearchEngine.cpp
    SearchDialog.h
    SearchDialog.cpp
)

target_include_directories(search PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(search PUBLIC
    # FileProvider/FileInfo: a search on a network tab walks the backend rather
    # than the local filesystem, so the engine speaks the provider abstraction.
    core
    widgets
    Qt5::Widgets
    Qt5::Core
    Qt5::Concurrent
)
