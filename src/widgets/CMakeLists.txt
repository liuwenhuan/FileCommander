# Shared self-drawn window chrome (frameless dialog base + title bar widgets)
# plus small reusable controls. Kept in its own tiny library so ui, archive, and
# search can all wear the same themed frameless look without introducing a
# dependency cycle between them.
add_library(widgets STATIC
    TitleButton.h
    DialogTitleBar.h
    DialogTitleBar.cpp
    FramelessDialog.h
    FramelessDialog.cpp
    SeekSlider.h
    SeekSlider.cpp
    ThemedDialogs.h
    ThemedDialogs.cpp
)

target_include_directories(widgets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(widgets PUBLIC
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
)
