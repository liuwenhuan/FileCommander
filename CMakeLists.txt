cmake_minimum_required(VERSION 3.16)
project(ttc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Widgets Gui Concurrent Test)
find_package(LibArchive)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/ui)
add_subdirectory(src/viewer)
add_subdirectory(src/archive)
add_subdirectory(src/search)

add_executable(ttc
    src/main.cpp
    resources/resources.qrc
)

target_link_libraries(ttc PRIVATE
    core ui viewer archive search
    Qt5::Widgets Qt5::Core Qt5::Gui Qt5::Concurrent
)

add_subdirectory(tests)
