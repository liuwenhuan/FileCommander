add_library(search STATIC
    SearchEngine.h
    SearchEngine.cpp
    SearchDialog.h
    SearchDialog.cpp
)

target_include_directories(search PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(search PUBLIC
    Qt5::Widgets
    Qt5::Core
    Qt5::Concurrent
)
