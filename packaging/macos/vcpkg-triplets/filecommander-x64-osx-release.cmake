# FileCommander release dependencies for an Intel macOS build.
#
# This triplet is intentionally kept in the repository so the arm64 and
# x86_64 dependency builds use the same linkage and deployment settings.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 12.0)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0"
)
