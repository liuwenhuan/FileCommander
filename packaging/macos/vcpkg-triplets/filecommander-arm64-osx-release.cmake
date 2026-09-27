# FileCommander release dependencies for an arm64 macOS build.
#
# vcpkg performs the target build with Apple's cross compiler, so this triplet
# can be used on an Intel Mac. Host tools are supplied separately through the
# x64 FileCommander triplet.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 12.0)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0"
)

# Qt's cross build must run qmake/moc/rcc/uic on the host while emitting
# libraries for arm64. The host triplet is supplied to vcpkg at install time;
# this tells the Qt 5 port where to find its x86_64 host tools.
set(VCPKG_QT_HOST_PLATFORM x64-osx)
set(VCPKG_QT_HOST_MKSPEC macx-clang)
set(VCPKG_QT_HOST_TOOLS_ROOT
    "$ENV{FILECOMMANDER_QT_HOST_TOOLS_ROOT}")
