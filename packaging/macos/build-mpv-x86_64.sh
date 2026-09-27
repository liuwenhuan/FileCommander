#!/usr/bin/env bash
#
# Build the small shared libmpv SDK used by the Intel macOS package.
#
# This deliberately builds libmpv only. The command-line player, Cocoa
# frontend, Vulkan, Lua, yt-dlp and other optional features are not needed by
# FileCommander and can pull in a large unrelated dependency tree.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
VERSION="${FILECOMMANDER_MPV_VERSION:-0.41.0}"
DEPS_ROOT="${FILECOMMANDER_DEPS_ROOT:-${SOURCE_DIR}/.deps/macos}"
PREFIX="${FILECOMMANDER_MPV_ROOT:-${DEPS_ROOT}/mpv-x64}"
WORK_ROOT="${FILECOMMANDER_MPV_BUILD_ROOT:-${SOURCE_DIR}/build/mpv-src}"
BUILD_DIR="${FILECOMMANDER_MPV_BUILD_DIR:-${SOURCE_DIR}/build/mpv-build-x64}"
DEPLOYMENT_TARGET="${FILECOMMANDER_MACOS_DEPLOYMENT_TARGET:-12.0}"
JOBS="${FILECOMMANDER_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

command -v meson >/dev/null 2>&1 || die "meson is required"
command -v ninja >/dev/null 2>&1 || die "ninja is required"
command -v curl >/dev/null 2>&1 || die "curl is required"
command -v tar >/dev/null 2>&1 || die "tar is required"
command -v pkg-config >/dev/null 2>&1 || die "pkg-config is required"

HEADER="${PREFIX}/include/mpv/client.h"
LIBRARY="${PREFIX}/lib/libmpv.2.dylib"
if [[ -f "${HEADER}" && -e "${LIBRARY}" ]]; then
    printf 'libmpv x86_64 is already available at %s\n' "${PREFIX}"
    exit 0
fi

mkdir -p "${WORK_ROOT}" "${PREFIX}"
ARCHIVE="${WORK_ROOT}/mpv-${VERSION}.tar.gz"
SOURCE_TREE="${WORK_ROOT}/mpv-${VERSION}"
if [[ ! -f "${ARCHIVE}" ]]; then
    URL="https://github.com/mpv-player/mpv/archive/refs/tags/v${VERSION}.tar.gz"
    printf 'Downloading %s\n' "${URL}"
    curl --fail --location --retry 3 --output "${ARCHIVE}" "${URL}"
fi
if [[ ! -f "${SOURCE_TREE}/meson.build" ]]; then
    rm -rf "${SOURCE_TREE}"
    tar -xzf "${ARCHIVE}" -C "${WORK_ROOT}"
    if [[ ! -d "${SOURCE_TREE}" ]]; then
        extracted="${WORK_ROOT}/mpv-${VERSION}"
        [[ -d "${extracted}" ]] ||
            die "mpv source tree was not extracted: ${extracted}"
    fi
fi
[[ -f "${SOURCE_TREE}/meson.build" ]] ||
    die "mpv source tree was not extracted: ${SOURCE_TREE}"

BREW="${FILECOMMANDER_BREW:-$(command -v brew || true)}"
[[ -n "${BREW}" ]] || die "Homebrew is required for FFmpeg, libass and libplacebo"
BREW_PREFIX="${FILECOMMANDER_BREW_PREFIX:-$("${BREW}" --prefix)}"
FFMPEG_PREFIX="${FILECOMMANDER_FFMPEG_ROOT:-$("${BREW}" --prefix ffmpeg)}"
LIBASS_PREFIX="${FILECOMMANDER_LIBASS_ROOT:-$("${BREW}" --prefix libass)}"
LIBPLACEBO_PREFIX="${FILECOMMANDER_LIBPLACEBO_ROOT:-$("${BREW}" --prefix libplacebo)}"
for dependency in "${FFMPEG_PREFIX}" "${LIBASS_PREFIX}" "${LIBPLACEBO_PREFIX}"; do
    [[ -d "${dependency}/lib" ]] ||
        die "required mpv dependency is missing: ${dependency}"
done

export PKG_CONFIG_PATH="${FFMPEG_PREFIX}/lib/pkgconfig:${LIBASS_PREFIX}/lib/pkgconfig:${LIBPLACEBO_PREFIX}/lib/pkgconfig:${BREW_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
pkg-config --exists libavcodec libavfilter libavformat libavutil libswresample libswscale libass libplacebo ||
    die "pkg-config cannot find the libmpv build dependencies"

meson setup "${BUILD_DIR}" "${SOURCE_TREE}" \
    --prefix="${PREFIX}" \
    --libdir=lib \
    --buildtype=release \
    --default-library=shared \
    -Dcplayer=false \
    -Dlibmpv=true \
    -Dbuild-date=false \
    -Dtests=false \
    -Dhtml-build=disabled \
    -Dmanpage-build=disabled \
    -Dpdf-build=disabled \
    -Djavascript=disabled \
    -Dlua=disabled \
    -Diconv=disabled \
    -Djpeg=disabled \
    -Dlcms2=disabled \
    -Dlibarchive=disabled \
    -Dlibavdevice=disabled \
    -Dlibbluray=disabled \
    -Drubberband=disabled \
    -Duchardet=disabled \
    -Dvapoursynth=disabled \
    -Dzimg=disabled \
    -Dcdda=disabled \
    -Ddvbin=disabled \
    -Ddvdnav=disabled \
    -Dalsa=disabled \
    -Daudiounit=disabled \
    -Dcoreaudio=enabled \
    -Davfoundation=disabled \
    -Djack=disabled \
    -Dopenal=disabled \
    -Dpipewire=disabled \
    -Dpulse=disabled \
    -Dsndio=disabled \
    -Dcocoa=disabled \
    -Dgl-cocoa=disabled \
    -Dgl=enabled \
    -Dplain-gl=enabled \
    -Dvulkan=disabled \
    -Dshaderc=disabled \
    -Dspirv-cross=disabled \
    -Dx11=disabled \
    -Dwayland=disabled \
    -Degl=disabled \
    -Ddrm=disabled \
    -Dgbm=disabled \
    -Dvaapi=disabled \
    -Dvideotoolbox-gl=disabled \
    -Dvideotoolbox-pl=disabled \
    -Dmacos-cocoa-cb=disabled \
    -Dmacos-media-player=disabled \
    -Dmacos-touchbar=disabled \
    -Dswift-build=disabled \
    -Dcplugins=disabled \
    -Dvector=enabled \
    -Dzlib=enabled \
    -Dc_args="-arch x86_64 -mmacosx-version-min=${DEPLOYMENT_TARGET}" \
    -Dc_link_args="-arch x86_64 -mmacosx-version-min=${DEPLOYMENT_TARGET}" \
    -Dcpp_args="-arch x86_64 -mmacosx-version-min=${DEPLOYMENT_TARGET}" \
    -Dcpp_link_args="-arch x86_64 -mmacosx-version-min=${DEPLOYMENT_TARGET}"

meson compile -C "${BUILD_DIR}" -j "${JOBS}"
meson install -C "${BUILD_DIR}"

[[ -f "${HEADER}" ]] ||
    die "libmpv installed without the client header: ${HEADER}"
[[ -e "${LIBRARY}" ]] ||
    die "libmpv installed without the shared library: ${LIBRARY}"

file -b "${LIBRARY}" | grep -q 'x86_64' ||
    die "libmpv is not x86_64: ${LIBRARY}"

cat > "${PREFIX}/.filecommander-mpv" <<EOF
filecommander_mpv_version=${VERSION}
filecommander_arch=x86_64
filecommander_macos_deployment_target=${DEPLOYMENT_TARGET}
EOF

printf 'libmpv x86_64 ready: %s\n' "${PREFIX}"
