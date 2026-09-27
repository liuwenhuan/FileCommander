#!/usr/bin/env bash
#
# Build FileCommander for Intel and Apple Silicon on one macOS host, then
# merge the two complete bundles into a Universal2 app.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
DEPS_ROOT="${FILECOMMANDER_DEPS_ROOT:-${SOURCE_DIR}/.deps/macos}"
BUILD_ROOT="${FILECOMMANDER_MACOS_BUILD_ROOT:-${SOURCE_DIR}/build/macos}"
DEPLOYMENT_TARGET="${FILECOMMANDER_MACOS_DEPLOYMENT_TARGET:-12.0}"
BUILD_TYPE="${FILECOMMANDER_BUILD_TYPE:-Release}"
VCPKG_ROOT="${DEPS_ROOT}/vcpkg"
VCPKG_INSTALL_ROOT="${VCPKG_ROOT}/installed"
VCPKG_HOST_TRIPLET="filecommander-x64-osx-release"
JOBS="${FILECOMMANDER_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
case "${BUILD_TYPE}" in
    Debug|debug)
        BUILD_CONFIG_DIR="debug"
        ;;
    Release|release)
        BUILD_CONFIG_DIR="release"
        ;;
    *)
        BUILD_CONFIG_DIR="$(printf '%s' "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"
        ;;
esac

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

command -v cmake >/dev/null 2>&1 || die "cmake is required"
command -v ninja >/dev/null 2>&1 || die "ninja is required"
command -v xcrun >/dev/null 2>&1 || die "Xcode command line tools are required"
[[ -x "${VCPKG_ROOT}/vcpkg" ]] ||
    die "dependencies are missing; run bootstrap-deps.sh for both architectures"

build_one() {
    local arch="$1"
    local triplet="$2"
    local prefix="${VCPKG_INSTALL_ROOT}/${triplet}"
    local build_dir="${BUILD_ROOT}/${arch}-${BUILD_CONFIG_DIR}"
    local app="${build_dir}/FileCommander.app"

    [[ -d "${prefix}" ]] ||
        die "missing ${arch} dependencies at ${prefix}; run bootstrap-deps.sh --arch ${arch}"

    cmake -S "${SOURCE_DIR}" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET="${triplet}" \
        -DVCPKG_HOST_TRIPLET="${VCPKG_HOST_TRIPLET}" \
        -DVCPKG_INSTALLED_DIR="${VCPKG_INSTALL_ROOT}" \
        -DFILECOMMANDER_DEPS_ROOT="${prefix}" \
        -DFILECOMMANDER_USE_VCPKG=ON \
        -DCMAKE_OSX_ARCHITECTURES="${arch}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DTTC_BUILD_TESTS=OFF \
        -DTTC_BUILD_BENCH=OFF \
        -DFILECOMMANDER_ENABLE_NETWORK=ON \
        -DFILECOMMANDER_ENABLE_LINUX_INTEGRATION=OFF \
        -DFILECOMMANDER_PREVIEW_PDF=OFF \
        -DFILECOMMANDER_PREVIEW_MEDIA=OFF \
        -DFILECOMMANDER_PREVIEW_OFFICE=ON
    cmake --build "${build_dir}" --parallel "${JOBS}"
    [[ -d "${app}" ]] || die "CMake did not produce ${app}"
}

deploy_one() {
    local app="$1"
    local prefix="$2"
    local macdeployqt
    macdeployqt="$(find \
        "${VCPKG_INSTALL_ROOT}/${VCPKG_HOST_TRIPLET}/tools" \
        -type f -name macdeployqt -perm -111 -print -quit 2>/dev/null || true)"
    [[ -n "${macdeployqt}" ]] ||
        die "host macdeployqt was not found below ${VCPKG_INSTALL_ROOT}/${VCPKG_HOST_TRIPLET}/tools"

    "${macdeployqt}" "${app}" -always-overwrite \
        "-libpath=${prefix}/lib" \
        "-executable=${app}/Contents/Library/LaunchServices/com.aigutta.FileCommander.privileged-helper"
}

X86_TRIPLET="filecommander-x64-osx-release"
ARM64_TRIPLET="filecommander-arm64-osx-release"
build_one x86_64 "${X86_TRIPLET}"
build_one arm64 "${ARM64_TRIPLET}"
X86_APP="${BUILD_ROOT}/x86_64-${BUILD_CONFIG_DIR}/FileCommander.app"
ARM64_APP="${BUILD_ROOT}/arm64-${BUILD_CONFIG_DIR}/FileCommander.app"

deploy_one "${X86_APP}" "${VCPKG_INSTALL_ROOT}/${X86_TRIPLET}"
deploy_one "${ARM64_APP}" "${VCPKG_INSTALL_ROOT}/${ARM64_TRIPLET}"

OUTPUT_APP="${BUILD_ROOT}/FileCommander-universal2.app"
python3 "${SCRIPT_DIR}/merge-universal-app.py" \
    --x86-app "${X86_APP}" \
    --arm64-app "${ARM64_APP}" \
    --output "${OUTPUT_APP}"

if [[ "${FILECOMMANDER_ADHOC_SIGN:-1}" == "1" ]]; then
    codesign --force --deep --sign - "${OUTPUT_APP}"
fi

printf 'Universal2 app: %s\n' "${OUTPUT_APP}"
