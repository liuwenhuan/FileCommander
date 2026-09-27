#!/usr/bin/env bash
#
# Build and package the Intel macOS application.
#
# This script intentionally performs one architecture build only. It does not
# invoke the Universal2 builder or cross-compile an arm64 dependency tree.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
DEPS_ROOT="${FILECOMMANDER_DEPS_ROOT:-${SOURCE_DIR}/.deps/macos}"
VCPKG_ROOT="${DEPS_ROOT}/vcpkg"
VCPKG_INSTALL_ROOT="${VCPKG_ROOT}/installed"
TRIPLET="filecommander-x64-osx-release"
VCPKG_PREFIX="${VCPKG_INSTALL_ROOT}/${TRIPLET}"
POPPLER_PREFIX="${FILECOMMANDER_POPPLER_QT5_ROOT:-${DEPS_ROOT}/poppler-qt5-x64}"
BUILD_DIR="${FILECOMMANDER_MACOS_BUILD_DIR:-${SOURCE_DIR}/build/macos-x86_64-release}"
APP="${BUILD_DIR}/FileCommander.app"
ICON="${APP}/Contents/Resources/FileCommander.icns"
DEPLOYMENT_TARGET="${FILECOMMANDER_MACOS_DEPLOYMENT_TARGET:-12.0}"
BUILD_TYPE="${FILECOMMANDER_BUILD_TYPE:-Release}"
JOBS="${FILECOMMANDER_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
BREW="${FILECOMMANDER_BREW:-$(command -v brew || true)}"

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

[[ -n "${BREW}" ]] || die "Homebrew is required for the macOS SMB dependency"
BREW_PREFIX="${FILECOMMANDER_BREW_PREFIX:-$("${BREW}" --prefix)}"
MPV_PREFIX="${FILECOMMANDER_MPV_ROOT:-}"
if [[ -z "${MPV_PREFIX}" ]]; then
    MPV_PREFIX="$("${BREW}" --prefix mpv 2>/dev/null || true)"
fi
SAMBA_PREFIX="$("${BREW}" --prefix samba 2>/dev/null || true)"
FFMPEG_PREFIX="$("${BREW}" --prefix ffmpeg 2>/dev/null || true)"
LIBASS_PREFIX="$("${BREW}" --prefix libass 2>/dev/null || true)"
LIBPLACEBO_PREFIX="$("${BREW}" --prefix libplacebo 2>/dev/null || true)"
PKGCONFIG_PREFIX="$("${BREW}" --prefix pkg-config 2>/dev/null || \
                    "${BREW}" --prefix pkgconf 2>/dev/null || true)"
export PATH="${BREW_PREFIX}/bin:${PATH}"

for tool in cmake ninja xcrun pkg-config install_name_tool otool file plutil; do
    command -v "${tool}" >/dev/null 2>&1 || die "${tool} is required"
done
[[ -d "${VCPKG_PREFIX}" ]] ||
    die "vcpkg x86_64 dependencies are missing: ${VCPKG_PREFIX}"
[[ -d "${MPV_PREFIX}/lib" ]] ||
    die "x86_64 libmpv is not installed: ${MPV_PREFIX}; set FILECOMMANDER_MPV_ROOT or install Homebrew mpv"
[[ -d "${SAMBA_PREFIX}/lib" ]] ||
    die "Homebrew samba is not installed: ${SAMBA_PREFIX}"

export PKG_CONFIG_PATH="${POPPLER_PREFIX}/lib/pkgconfig:${MPV_PREFIX}/lib/pkgconfig:${SAMBA_PREFIX}/lib/pkgconfig:${FFMPEG_PREFIX}/lib/pkgconfig:${LIBASS_PREFIX}/lib/pkgconfig:${LIBPLACEBO_PREFIX}/lib/pkgconfig:${BREW_PREFIX}/lib/pkgconfig:${PKGCONFIG_PREFIX}/lib/pkgconfig:${VCPKG_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
pkg-config --exists mpv ||
    die "pkg-config cannot find libmpv; PKG_CONFIG_PATH=${PKG_CONFIG_PATH}"
pkg-config --exists smbclient ||
    die "pkg-config cannot find Homebrew smbclient; PKG_CONFIG_PATH=${PKG_CONFIG_PATH}"

"${SCRIPT_DIR}/build-poppler-qt5.sh" "${POPPLER_PREFIX}"
[[ -f "${POPPLER_PREFIX}/include/poppler/qt5/poppler-qt5.h" ]] ||
    die "Poppler Qt5 was not built: ${POPPLER_PREFIX}"

OFFICE_OUTPUT="${FILECOMMANDER_OFFICE_OXIDE_OUTPUT:-${SOURCE_DIR}/build/office-oxide}"
if [[ ! -x "${OFFICE_OUTPUT}/office-oxide" ]]; then
    command -v cargo >/dev/null 2>&1 || die "cargo is required to build office-oxide"
    bash "${SOURCE_DIR}/scripts/build-office-oxide.sh" "${OFFICE_OUTPUT}"
fi
[[ -x "${OFFICE_OUTPUT}/office-oxide" ]] ||
    die "pinned office-oxide was not built: ${OFFICE_OUTPUT}/office-oxide"

[[ -x "${VCPKG_ROOT}/vcpkg" ]] ||
    die "vcpkg is missing at ${VCPKG_ROOT}"

cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET="${TRIPLET}" \
    -DVCPKG_HOST_TRIPLET="${TRIPLET}" \
    -DVCPKG_INSTALLED_DIR="${VCPKG_INSTALL_ROOT}" \
    -DFILECOMMANDER_DEPS_ROOT="${VCPKG_PREFIX}" \
    -DFILECOMMANDER_USE_VCPKG=ON \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    -DFILECOMMANDER_POPPLER_QT5_ROOT="${POPPLER_PREFIX}" \
    -DFILECOMMANDER_ENABLE_NETWORK=ON \
    -DFILECOMMANDER_ENABLE_LINUX_INTEGRATION=OFF \
    -DFILECOMMANDER_PREVIEW_PDF=ON \
    -DFILECOMMANDER_PREVIEW_MEDIA=ON \
    -DFILECOMMANDER_MEDIA_BACKEND=mpv \
    -DFILECOMMANDER_PREVIEW_OFFICE=ON \
    -DTTC_BUILD_TESTS=OFF \
    -DTTC_BUILD_BENCH=OFF

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
[[ -d "${APP}" ]] || die "CMake did not produce ${APP}"
[[ -f "${ICON}" ]] || die "macOS app icon was not bundled: ${ICON}"
ICON_FILE="$(plutil -extract CFBundleIconFile raw -o - \
    "${APP}/Contents/Info.plist" 2>/dev/null || true)"
[[ "${ICON_FILE}" == "FileCommander.icns" ]] ||
    die "Info.plist does not reference FileCommander.icns (got: ${ICON_FILE:-<missing>})"
[[ "$(file -b "${ICON}")" == *"Mac OS X icon"* ]] ||
    die "invalid macOS app icon: ${ICON}"

HELPER="${APP}/Contents/Helpers/FileCommander-smb-helper"
if [[ ! -x "${HELPER}" && -x "${BUILD_DIR}/FileCommander-smb-helper" ]]; then
    mkdir -p "$(dirname -- "${HELPER}")"
    cp -f "${BUILD_DIR}/FileCommander-smb-helper" "${HELPER}"
    chmod +x "${HELPER}"
fi
[[ -x "${HELPER}" ]] || die "SMB helper was not built: ${HELPER}"

PRIVILEGED_HELPER="${APP}/Contents/Library/LaunchServices/com.aigutta.FileCommander.privileged-helper"
[[ -x "${PRIVILEGED_HELPER}" ]] ||
    die "privileged helper was not built: ${PRIVILEGED_HELPER}"

cp -f "${OFFICE_OUTPUT}/office-oxide" "${APP}/Contents/MacOS/office-oxide"
chmod +x "${APP}/Contents/MacOS/office-oxide"

# The bundle may have been produced by an earlier packaging attempt. Remove
# only generated runtime directories so stale macdeployqt files cannot survive
# into a later deterministic package.
rm -rf "${APP}/Contents/Frameworks" "${APP}/Contents/PlugIns"

FRAMEWORKS="${APP}/Contents/Frameworks"
mkdir -p "${FRAMEWORKS}"

# Deploy the Qt runtime without macdeployqt. The vcpkg macdeployqt binary can
# spend several minutes inside install_name_tool when it follows Samba's large
# private-library closure. The application only needs a small, known Qt set;
# the recursive Mach-O pass below handles its dependencies in the same way as
# mpv, Poppler, Samba and their transitive libraries.
deploy_qt_runtime() {
    local qt_module source destination plugin_dir plugin_source
    local qt_modules=(
        Qt5Core
        Qt5Gui
        Qt5Widgets
        Qt5Network
        Qt5WebSockets
        Qt5Concurrent
        Qt5OpenGL
        Qt5Svg
        Qt5Xml
        Qt5PrintSupport
        Qt5DBus
    )
    for qt_module in "${qt_modules[@]}"; do
        source="$(find "${VCPKG_PREFIX}/lib" -maxdepth 1 -type f \
            -name "lib${qt_module}*.dylib" -print -quit 2>/dev/null || true)"
        [[ -n "${source}" ]] ||
            die "Qt runtime library is missing: ${qt_module}"
        destination="${FRAMEWORKS}/$(basename -- "${source}")"
        cp -Lf "${source}" "${destination}"
    done

    for plugin_dir in platforms imageformats iconengines printsupport styles; do
        plugin_source="${VCPKG_PREFIX}/plugins/${plugin_dir}"
        [[ -d "${plugin_source}" ]] || continue
        mkdir -p "${APP}/Contents/PlugIns/${plugin_dir}"
        while IFS= read -r -d '' source; do
            cp -Lf "${source}" \
                "${APP}/Contents/PlugIns/${plugin_dir}/$(basename -- "${source}")"
        done < <(find -L "${plugin_source}" -maxdepth 1 -type f -print0)
    done

    mkdir -p "${APP}/Contents/Resources"
    cat > "${APP}/Contents/Resources/qt.conf" <<'EOF'
[Paths]
Plugins = PlugIns
Imports = Resources/qml
Qml2Imports = Resources/qml
EOF
}

deploy_qt_runtime

# Resolve an external dependency by its original install name. Absolute
# Homebrew/vcpkg paths are preferred; @rpath names are looked up by basename
# across the same prefixes. This keeps the closure complete for mpv, which
# pulls in FFmpeg and a sizeable codec stack through Homebrew.
find_dependency_source() {
    local dependency="$1"
    local base="${dependency##*/}"
    if [[ "${dependency}" != @* && -f "${dependency}" ]]; then
        printf '%s\n' "${dependency}"
        return 0
    fi
    local root
    for root in \
        "${MPV_PREFIX}" \
        "${SAMBA_PREFIX}" \
        "${BREW_PREFIX}/opt" \
        "${POPPLER_PREFIX}" \
        "${VCPKG_PREFIX}"; do
        if [[ -f "${root}/lib/${base}" ]]; then
            printf '%s\n' "${root}/lib/${base}"
            return 0
        fi
        local found
        found="$(find -L "${root}" -type f -name "${base}" -print -quit 2>/dev/null || true)"
        if [[ -n "${found}" ]]; then
            printf '%s\n' "${found}"
            return 0
        fi
    done
    return 1
}

bundle_dependency() {
    local dependency="$1"
    local base="${dependency##*/}"
    local destination="${FRAMEWORKS}/${base}"
    if [[ -e "${destination}" ]]; then
        printf '%s\n' "${destination}"
        return 0
    fi
    local source
    source="$(find_dependency_source "${dependency}" || true)"
    [[ -n "${source}" ]] || return 1
    cp -Lf "${source}" "${destination}"
    printf '%s\n' "${destination}"
}

add_rpath() {
    local binary="$1"
    local rpath="$2"
    if ! otool -l "${binary}" 2>/dev/null |
        grep -Fq "path ${rpath} ("; then
        install_name_tool -add_rpath "${rpath}" "${binary}"
    fi
}

remove_absolute_rpaths() {
    local binary="$1"
    local rpath
    while IFS= read -r rpath; do
        [[ "${rpath}" == /* ]] || continue
        install_name_tool -delete_rpath "${rpath}" "${binary}" || true
    done < <(
        otool -l "${binary}" 2>/dev/null |
            awk '
                /LC_RPATH/ { waiting = 1; next }
                waiting && $1 == "path" { print $2; waiting = 0 }
            '
    )
}

macho_file() {
    file -b "$1" 2>/dev/null | grep -q 'Mach-O'
}

dependency_lines() {
    # otool -L prints the inspected file on line one. A dylib then has its own
    # install name on line two, while an executable starts with its first real
    # dependency on line two. Filter the identity by asking otool -D instead
    # of dropping a fixed number of lines.
    local self_id
    self_id="$(otool -D "$1" 2>/dev/null | tail -n 1 || true)"
    otool -L "$1" 2>/dev/null | tail -n +2 |
        sed -E 's/^[[:space:]]+([^[:space:]]+)[[:space:]]+\(.*/\1/' |
        awk -v self_id="${self_id}" '$0 != self_id'
}

queue_contains() {
    local candidate="$1"
    local item
    for item in "${MACHO_QUEUE[@]}"; do
        [[ "${item}" == "${candidate}" ]] && return 0
    done
    return 1
}

MACHO_QUEUE=(
    "${APP}/Contents/MacOS/FileCommander"
    "${HELPER}"
    "${PRIVILEGED_HELPER}"
)
while IFS= read -r -d '' candidate; do
    if macho_file "${candidate}" && ! queue_contains "${candidate}"; then
        MACHO_QUEUE+=("${candidate}")
    fi
done < <(find "${APP}" -type f -print0)

index=0
while ((index < ${#MACHO_QUEUE[@]})); do
    binary="${MACHO_QUEUE[index]}"
    index=$((index + 1))
    [[ -f "${binary}" ]] || continue
    macho_file "${binary}" || continue

    remove_absolute_rpaths "${binary}"
    case "${binary}" in
        "${FRAMEWORKS}"/*)
            add_rpath "${binary}" "@loader_path"
            if [[ "$(dirname -- "${binary}")" == "${FRAMEWORKS}" ]]; then
                install_name_tool -id "@rpath/$(basename -- "${binary}")" "${binary}" || true
            fi
            ;;
        */Contents/Helpers/*)
            add_rpath "${binary}" "@loader_path/../Frameworks"
            ;;
        */Contents/Library/LaunchServices/*)
            add_rpath "${binary}" "@loader_path/../../Frameworks"
            ;;
        */Contents/PlugIns/*)
            add_rpath "${binary}" "@loader_path/../../Frameworks"
            ;;
        *)
            add_rpath "${binary}" "@loader_path/../Frameworks"
            ;;
    esac

    while IFS= read -r dependency; do
        [[ -n "${dependency}" ]] || continue
        base="${dependency##*/}"
        case "${dependency}" in
            /System/*|/usr/lib/*|/usr/libexec/*)
                continue
                ;;
            @loader_path/*|@executable_path/*)
                if [[ -e "${FRAMEWORKS}/${base}" ]]; then
                    destination="${FRAMEWORKS}/${base}"
                else
                    destination="$(bundle_dependency "${dependency}" || true)"
                    if [[ -n "${destination}" ]]; then
                        install_name_tool -change "${dependency}" \
                            "@rpath/${base}" "${binary}"
                    fi
                fi
                ;;
            @rpath/*)
                destination="${FRAMEWORKS}/${dependency#@rpath/}"
                if [[ ! -e "${destination}" ]]; then
                    destination="$(bundle_dependency "${dependency}" || true)"
                fi
                ;;
            *)
                destination="$(bundle_dependency "${dependency}" || true)"
                if [[ -n "${destination}" ]]; then
                    install_name_tool -change "${dependency}" \
                        "@rpath/${base}" "${binary}"
                fi
                ;;
        esac
        if [[ -n "${destination:-}" && -f "${destination}" ]] &&
           macho_file "${destination}" &&
           ! queue_contains "${destination}"; then
            MACHO_QUEUE+=("${destination}")
        fi
        destination=""
    done < <(dependency_lines "${binary}")
done

if [[ "${FILECOMMANDER_ADHOC_SIGN:-1}" == "1" ]]; then
    codesign --force --deep --sign - "${APP}"
    codesign --verify --deep --strict "${APP}"
fi

verify_x86_64() {
    local path="$1"
    local info
    info="$(file -b "${path}")"
    [[ "${info}" == *"x86_64"* ]] ||
        die "expected x86_64 binary, got ${path}: ${info}"
    [[ "${info}" != *"arm64"* ]] ||
        die "unexpected arm64 slice in ${path}: ${info}"
}

verify_macho_dependencies() {
    local binary="$1"
    local rpath
    while IFS= read -r dependency; do
        case "${dependency}" in
            /usr/local/opt/*|/usr/local/Cellar/*|/opt/homebrew/*|\
            "${MPV_PREFIX}"/*|"${SAMBA_PREFIX}"/*|"${VCPKG_PREFIX}"/*|\
            "${POPPLER_PREFIX}"/*)
                die "absolute third-party dependency remains in ${binary}: ${dependency}"
                ;;
        esac
    done < <(dependency_lines "${binary}")
    while IFS= read -r rpath; do
        case "${rpath}" in
            /System/*|/usr/lib/*|/usr/libexec/*)
                ;;
            /*)
                die "absolute third-party rpath remains in ${binary}: ${rpath}"
                ;;
        esac
    done < <(
        otool -l "${binary}" 2>/dev/null |
            awk '
                /LC_RPATH/ { waiting = 1; next }
                waiting && $1 == "path" { print $2; waiting = 0 }
            '
    )
}

verify_x86_64 "${APP}/Contents/MacOS/FileCommander"
verify_x86_64 "${HELPER}"
verify_x86_64 "${PRIVILEGED_HELPER}"
verify_x86_64 "${APP}/Contents/MacOS/office-oxide"

while IFS= read -r -d '' binary; do
    if macho_file "${binary}"; then
        verify_x86_64 "${binary}"
        verify_macho_dependencies "${binary}"
    fi
done < <(find "${APP}" -type f -print0)

printf 'Intel macOS app: %s\n' "${APP}"
