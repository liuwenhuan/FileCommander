cmake_minimum_required(VERSION 3.16)
project(FileCommander VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(cmake/FileCommanderOptions.cmake)

add_compile_definitions(
    FILECOMMANDER_HAS_NETWORK=${FILECOMMANDER_HAS_NETWORK}
    FILECOMMANDER_HAS_PREVIEW_PDF=${FILECOMMANDER_HAS_PREVIEW_PDF}
    FILECOMMANDER_HAS_PREVIEW_MEDIA=${FILECOMMANDER_HAS_PREVIEW_MEDIA}
    FILECOMMANDER_HAS_PREVIEW_OFFICE=${FILECOMMANDER_HAS_PREVIEW_OFFICE}
    FILECOMMANDER_HAS_LINUX_INTEGRATION=${FILECOMMANDER_HAS_LINUX_INTEGRATION}
)
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Widgets Gui Concurrent Test Network)
if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION)
    find_package(Qt5 REQUIRED COMPONENTS DBus)
endif()
find_package(LibArchive)

enable_testing()

# Generated version header (TTC_VERSION etc.) derived from the project()
# version above, so the About box and the online-update checker can read the
# running version at runtime instead of hard-coding it.
configure_file(${CMAKE_SOURCE_DIR}/src/version.h.in
               ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)

add_subdirectory(src/platform)
add_subdirectory(src/core)
add_subdirectory(src/widgets)
add_subdirectory(src/ui)
add_subdirectory(src/viewer)
add_subdirectory(src/archive)
add_subdirectory(src/search)
if(FILECOMMANDER_ENABLE_LINUX_INTEGRATION AND FILECOMMANDER_ENABLE_NETWORK)
    add_subdirectory(src/smbhelper)
endif()

add_executable(FileCommander
    src/main.cpp
    resources/resources.qrc
)

target_link_libraries(FileCommander PRIVATE
    core ui viewer archive search widgets
    Qt5::Widgets Qt5::Core Qt5::Gui Qt5::Concurrent
)
target_compile_definitions(FileCommander PRIVATE
    FILECOMMANDER_HAS_NETWORK=${FILECOMMANDER_HAS_NETWORK}
    FILECOMMANDER_HAS_PREVIEW_PDF=${FILECOMMANDER_HAS_PREVIEW_PDF}
    FILECOMMANDER_HAS_PREVIEW_MEDIA=${FILECOMMANDER_HAS_PREVIEW_MEDIA}
    FILECOMMANDER_HAS_PREVIEW_OFFICE=${FILECOMMANDER_HAS_PREVIEW_OFFICE}
    FILECOMMANDER_HAS_LINUX_INTEGRATION=${FILECOMMANDER_HAS_LINUX_INTEGRATION}
)

# Tests and benchmarks default ON (the normal development build is unchanged),
# but a packaging build turns them off: they roughly triple build time, and
# tests/ hard-requires /usr/src/googletest, which a clean package builder has no
# reason to install.
option(TTC_BUILD_TESTS "Build the unit test suites" ON)
option(TTC_BUILD_LIVE_SMB_TESTS
       "Build the opt-in live SMB integration test (runtime access still requires FC_SMB_E2E=1)"
       OFF)
option(TTC_BUILD_LIVE_WEBDAV_TESTS
       "Build the opt-in live WebDAV integration test (runtime access still requires FC_WEBDAV_E2E=1)"
       OFF)
option(TTC_BUILD_BENCH "Build the benchmarks" ON)

if(TTC_BUILD_TESTS)
    add_subdirectory(tests)
endif()
if(TTC_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# --- Installation -----------------------------------------------------------
# Consumed by both packaging scripts (see packaging/ and docs/PACKAGING.md) via
# `DESTDIR=<staging> cmake --install`. The desktop file must be named
# FileCommander.desktop: src/main.cpp calls setDesktopFileName("FileCommander"),
# which is how the window manager ties running windows back to the launcher entry.
include(GNUInstallDirs)

install(TARGETS FileCommander RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/FileCommander.desktop
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
endif()
# Note this SVG is deliberately NOT in resources.qrc -- the in-app window icon
# is painted in code (src/main.cpp) so it needs no SVG plugin. This copy exists
# purely for the desktop/launcher entry's Icon=FileCommander.
install(FILES resources/icons/FileCommander.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
if(UNIX AND NOT APPLE)
    install(FILES packaging/FileCommander.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
endif()

# --- Translations -----------------------------------------------------------
# Contributor workflow (see resources/translations/README.md):
#   cmake --build build --target update-translations   # refresh .ts from source
#   <edit ttc_<lang>.ts in Qt Linguist>
#   cmake --build build --target release-translations   # compile .ts -> .qm
# The .qm are bundled via resources.qrc; a translator can also drop a compiled
# ttc_<lang>.qm into ~/.config/FileCommander/translations/ to add a language
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
        PATHS /usr/lib/qt5/bin /usr/lib/qt6/bin /usr/lib/qt5/libexec /usr/lib/qt6/libexec)
    find_program(LRELEASE_CMD NAMES lrelease lrelease-qt5
        PATHS /usr/lib/qt5/bin /usr/lib/qt6/bin /usr/lib/qt5/libexec /usr/lib/qt6/libexec)
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
