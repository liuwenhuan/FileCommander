cmake_minimum_required(VERSION 3.16)
project(ttc VERSION 0.1.0 LANGUAGES CXX C)

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

# test-ws worktree only: rename output binary to `test-ttc` so this isolated
# dev/test build never collides with (or gets killed alongside) the main `ttc`.
set_target_properties(ttc PROPERTIES OUTPUT_NAME test-ttc)

add_subdirectory(tests)
add_subdirectory(bench)

# --- Translations -----------------------------------------------------------
# Contributor workflow (see resources/translations/README.md):
#   cmake --build build --target update-translations   # refresh .ts from source
#   <edit ttc_<lang>.ts in Qt Linguist>
#   cmake --build build --target release-translations   # compile .ts -> .qm
# The .qm are bundled via resources.qrc; a translator can also drop a compiled
# ttc_<lang>.qm into ~/.config/totalcommander/translations/ to add a language
# without rebuilding.
file(GLOB TS_FILES ${CMAKE_SOURCE_DIR}/resources/translations/ttc_*.ts)
# Resolve lupdate/lrelease. Prefer Qt5's LinguistTools cmake package; otherwise
# probe the versioned Qt bin dirs directly (the bare /usr/bin/lrelease is a
# qtchooser stub that can't find a default Qt). Qt6's tools read/write Qt5 .ts
# files fine, so they're an acceptable fallback.
find_package(Qt5 QUIET COMPONENTS LinguistTools)
if(Qt5LinguistTools_FOUND)
    set(LUPDATE_CMD Qt5::lupdate)
    set(LRELEASE_CMD Qt5::lrelease)
else()
    find_program(LUPDATE_CMD NAMES lupdate lupdate-qt5
        PATHS /usr/lib/qt5/bin /usr/lib/qt6/bin /usr/lib/qt5/libexec /usr/lib/qt6/libexec
        NO_DEFAULT_PATH)
    find_program(LRELEASE_CMD NAMES lrelease lrelease-qt5
        PATHS /usr/lib/qt5/bin /usr/lib/qt6/bin /usr/lib/qt5/libexec /usr/lib/qt6/libexec
        NO_DEFAULT_PATH)
endif()
if(LUPDATE_CMD AND LRELEASE_CMD)
    add_custom_target(update-translations
        COMMAND ${LUPDATE_CMD} -recursive ${CMAKE_SOURCE_DIR}/src -ts ${TS_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "lupdate: extracting translatable strings into .ts files"
        VERBATIM)
    add_custom_target(release-translations
        COMMAND ${LRELEASE_CMD} ${TS_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "lrelease: compiling .ts files into .qm"
        VERBATIM)
endif()
