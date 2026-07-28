find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest)
    if(EXISTS /usr/src/googletest/CMakeLists.txt)
        # Debian/Ubuntu/Deepin ship libgtest-dev as source only.
        add_subdirectory(/usr/src/googletest ${CMAKE_BINARY_DIR}/googletest-build EXCLUDE_FROM_ALL)
        add_library(GTest::gtest ALIAS gtest)
        add_library(GTest::gtest_main ALIAS gtest_main)
    else()
        message(FATAL_ERROR
            "GoogleTest was not found. Install gtest (vcpkg on Windows or "
            "libgtest-dev on Debian-family Linux).")
    endif()
endif()

add_subdirectory(core)
add_subdirectory(ui)
add_subdirectory(archive)
add_subdirectory(viewer)
