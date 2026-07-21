if(NOT TARGET gtest)
    # Debian/Ubuntu/Deepin ship libgtest-dev as source only; build it here.
    add_subdirectory(/usr/src/googletest ${CMAKE_BINARY_DIR}/googletest-build EXCLUDE_FROM_ALL)
endif()

add_subdirectory(core)
add_subdirectory(ui)
add_subdirectory(archive)
add_subdirectory(viewer)
