cmake_minimum_required(VERSION 3.16)

if(NOT WIN32)
    message(FATAL_ERROR "wmf_media_probe is a Windows-only acceptance probe.")
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Widgets Multimedia MultimediaWidgets)

add_executable(wmf_media_probe wmf_media_probe.cpp)
target_link_libraries(wmf_media_probe PRIVATE
    Qt5::Core
    Qt5::Widgets
    Qt5::Multimedia
    Qt5::MultimediaWidgets
)
set_target_properties(wmf_media_probe PROPERTIES WIN32_EXECUTABLE FALSE)
