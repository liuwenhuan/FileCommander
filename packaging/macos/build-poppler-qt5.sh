#!/usr/bin/env bash
#
# Build the Qt5 bindings of Poppler for the Intel macOS package.
#
# The vcpkg Poppler port used by this repository intentionally exposes only
# Qt6. FileCommander is still a Qt5 application, so PDF support uses a small
# source build against the same Qt5 and vcpkg prefix as the application.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
DEPS_ROOT="${FILECOMMANDER_DEPS_ROOT:-${SOURCE_DIR}/.deps/macos}"
VCPKG_ROOT="${DEPS_ROOT}/vcpkg"
VCPKG_INSTALL_ROOT="${VCPKG_ROOT}/installed"
TRIPLET="filecommander-x64-osx-release"
VCPKG_PREFIX="${VCPKG_INSTALL_ROOT}/${TRIPLET}"
POPPLER_VERSION="${FILECOMMANDER_POPPLER_VERSION:-26.04.0}"
PREFIX="${1:-${DEPS_ROOT}/poppler-qt5-x64}"
WORK_ROOT="${FILECOMMANDER_POPPLER_BUILD_ROOT:-${SOURCE_DIR}/build/poppler-src}"
DEPLOYMENT_TARGET="${FILECOMMANDER_MACOS_DEPLOYMENT_TARGET:-12.0}"
JOBS="${FILECOMMANDER_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

command -v cmake >/dev/null 2>&1 || die "cmake is required"
command -v ninja >/dev/null 2>&1 || die "ninja is required"
command -v curl >/dev/null 2>&1 || die "curl is required"
command -v tar >/dev/null 2>&1 || die "tar is required"
[[ -x "${VCPKG_ROOT}/vcpkg" ]] ||
    die "vcpkg is missing at ${VCPKG_ROOT}; run packaging/macos/bootstrap-deps.sh --arch x86_64"
[[ -d "${VCPKG_PREFIX}" ]] ||
    die "vcpkg x86_64 prefix is missing: ${VCPKG_PREFIX}"

HEADER="${PREFIX}/include/poppler/qt5/poppler-qt5.h"
LIBRARY="${PREFIX}/lib/libpoppler-qt5.dylib"
if [[ -f "${HEADER}" && -e "${LIBRARY}" ]]; then
    printf 'Poppler Qt5 is already available at %s\n' "${PREFIX}"
    exit 0
fi

mkdir -p "${WORK_ROOT}" "${PREFIX}"

# Keep the dependency set in the x86_64 vcpkg prefix. These are the same
# libraries Poppler needs for its core renderer; Qt itself is already present.
"${VCPKG_ROOT}/vcpkg" install \
    boost-container freetype libiconv libjpeg-turbo libpng openjpeg tiff zlib \
    --recurse \
    --triplet="${TRIPLET}" \
    --host-triplet="${TRIPLET}" \
    --overlay-triplets="${SCRIPT_DIR}/vcpkg-triplets" \
    --x-install-root="${VCPKG_INSTALL_ROOT}"

ARCHIVE="${WORK_ROOT}/poppler-${POPPLER_VERSION}.tar.xz"
SOURCE_TREE="${WORK_ROOT}/poppler-${POPPLER_VERSION}"
if [[ ! -f "${ARCHIVE}" ]]; then
    URL="https://poppler.freedesktop.org/poppler-${POPPLER_VERSION}.tar.xz"
    printf 'Downloading %s\n' "${URL}"
    curl --fail --location --retry 3 --output "${ARCHIVE}" "${URL}"
fi
if [[ ! -f "${SOURCE_TREE}/CMakeLists.txt" ]]; then
    rm -rf "${SOURCE_TREE}"
    tar -xJf "${ARCHIVE}" -C "${WORK_ROOT}"
fi
[[ -f "${SOURCE_TREE}/CMakeLists.txt" ]] ||
    die "Poppler source tree was not extracted: ${SOURCE_TREE}"

BUILD_DIR="${WORK_ROOT}/build-${TRIPLET}"
PKGCONFIG_PATH="${VCPKG_PREFIX}/lib/pkgconfig:${VCPKG_PREFIX}/share/pkgconfig"
export PKG_CONFIG_PATH="${PKGCONFIG_PATH}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

cmake -S "${SOURCE_TREE}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    -DCMAKE_INSTALL_NAME_DIR=@rpath \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    -DVCPKG_HOST_TRIPLET="${TRIPLET}" \
    -DVCPKG_INSTALLED_DIR="${VCPKG_INSTALL_ROOT}" \
    -DCMAKE_PREFIX_PATH="${VCPKG_PREFIX}" \
    -DQt5_DIR="${VCPKG_PREFIX}/share/cmake/Qt5" \
    -DENABLE_QT5=ON \
    -DENABLE_QT6=OFF \
    -DENABLE_GLIB=OFF \
    -DENABLE_GOBJECT_INTROSPECTION=OFF \
    -DENABLE_CPP=ON \
    -DENABLE_UTILS=OFF \
    -DBUILD_GTK_TESTS=OFF \
    -DBUILD_QT5_TESTS=OFF \
    -DBUILD_QT6_TESTS=OFF \
    -DBUILD_CPP_TESTS=OFF \
    -DBUILD_MANUAL_TESTS=OFF \
    -DENABLE_BOOST=OFF \
    -DENABLE_LIBCURL=OFF \
    -DENABLE_LCMS=OFF \
    -DENABLE_NSS3=OFF \
    -DENABLE_GPGME=OFF \
    -DENABLE_LIBTIFF=OFF \
    -DFONT_CONFIGURATION=generic \
    -DRUN_GPERF_IF_PRESENT=OFF

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
cmake --install "${BUILD_DIR}"

[[ -f "${HEADER}" ]] ||
    die "Poppler installed without the Qt5 header: ${HEADER}"
[[ -e "${LIBRARY}" ]] ||
    die "Poppler installed without the Qt5 library: ${LIBRARY}"

cat > "${PREFIX}/.filecommander-poppler-qt5" <<EOF
filecommander_poppler_version=${POPPLER_VERSION}
filecommander_arch=x86_64
filecommander_qt_prefix=${VCPKG_PREFIX}
filecommander_macos_deployment_target=${DEPLOYMENT_TARGET}
EOF

printf 'Poppler Qt5 ready: %s\n' "${PREFIX}"
