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

if(WIN32)
    set(_fc_test_runtime_dirs "$<TARGET_FILE_DIR:Qt5::Core>")
    if(FILECOMMANDER_PREVIEW_MEDIA)
        list(APPEND _fc_test_runtime_dirs "${FILECOMMANDER_MPV_ROOT}")
    endif()
    if(FILECOMMANDER_PREVIEW_PDF)
        list(APPEND _fc_test_runtime_dirs "${FILECOMMANDER_POPPLER_QT5_ROOT}/bin")
    endif()
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _fc_test_runtime_dirs
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    list(JOIN _fc_test_runtime_dirs "\\;" FILECOMMANDER_WINDOWS_TEST_RUNTIME_PATH)
endif()

function(filecommander_set_test_runtime test_name)
    if(WIN32)
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "PATH=${FILECOMMANDER_WINDOWS_TEST_RUNTIME_PATH}\\;$ENV{PATH}")
    endif()
endfunction()

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(ui)
add_subdirectory(archive)
add_subdirectory(viewer)
