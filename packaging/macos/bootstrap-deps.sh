#!/usr/bin/env bash
#
# Download and build the native macOS dependencies used by FileCommander.
#
# This script deliberately keeps the dependency tree outside the source
# checkout. It can be run on an Intel Mac with --arch arm64: vcpkg invokes
# Apple's arm64 cross compiler while its host tools continue to run as x86_64.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
DEPS_ROOT="${FILECOMMANDER_DEPS_ROOT:-${SOURCE_DIR}/.deps/macos}"
VCPKG_REF="${FILECOMMANDER_VCPKG_REF:-11ace808cc8a3a941f386e33726b992b22ba9e5a}"
VCPKG_URL="${FILECOMMANDER_VCPKG_URL:-https://github.com/microsoft/vcpkg.git}"
ARCH=""

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat >&2 <<'EOF'
Usage: packaging/macos/bootstrap-deps.sh --arch <x86_64|arm64>

Environment:
  FILECOMMANDER_DEPS_ROOT  Dependency cache root
  FILECOMMANDER_VCPKG_REF  vcpkg commit to use
  FILECOMMANDER_VCPKG_URL  vcpkg Git URL
EOF
    exit 2
}

while (($#)); do
    case "$1" in
        --arch)
            (($# >= 2)) || usage
            ARCH="$2"
            shift 2
            ;;
        --help|-h)
            usage
            ;;
        *)
            usage
            ;;
    esac
done

case "${ARCH}" in
    x86_64)
        TRIPLET="filecommander-x64-osx-release"
        ;;
    arm64)
        TRIPLET="filecommander-arm64-osx-release"
        ;;
    *)
        usage
        ;;
esac

command -v git >/dev/null 2>&1 || die "git is required"
command -v xcodebuild >/dev/null 2>&1 || die "Xcode command line tools are required"

prepare_host_build_tools() {
    local brew_command
    brew_command="$(command -v brew || true)"
    if [[ -n "${brew_command}" ]]; then
        local brew_bin
        local automake_bin
        local libtool_gnubin
        brew_bin="$("${brew_command}" --prefix)/bin"
        automake_bin="$("${brew_command}" --prefix automake 2>/dev/null || true)/bin"
        libtool_gnubin="$("${brew_command}" --prefix libtool 2>/dev/null || true)/libexec/gnubin"
        export PATH="${automake_bin}:${libtool_gnubin}:${brew_bin}:${PATH}"
    fi

    local missing=()
    for tool in autoconf automake libtoolize; do
        command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}")
    done
    if ((${#missing[@]})) && [[ -n "${brew_command}" ]]; then
        "${brew_command}" install autoconf autoconf-archive automake libtool
        export PATH="$("${brew_command}" --prefix automake)/bin:$("${brew_command}" --prefix libtool)/libexec/gnubin:${PATH}"
    fi

    for tool in autoconf automake libtoolize; do
        command -v "${tool}" >/dev/null 2>&1 ||
            die "host build tool '${tool}' is required; install autoconf, autoconf-archive, automake, and libtool"
    done
}

prepare_host_build_tools

VCPKG_ROOT="${DEPS_ROOT}/vcpkg"
VCPKG_INSTALL_ROOT="${VCPKG_ROOT}/installed"
VCPKG_DOWNLOADS="${DEPS_ROOT}/downloads"
VCPKG_BINARY_CACHE="${DEPS_ROOT}/binary-cache"
TRIPLET_DIR="${SCRIPT_DIR}/vcpkg-triplets"
QT_CROSS_PATCH="${SCRIPT_DIR}/vcpkg-patches/qt5-base-configure-cross.patch"
QT_PORT_CROSS_PATCH="${SCRIPT_DIR}/vcpkg-patches/qt5-base-port-cross.patch"
QT_INSTALL_CROSS_PATCH="${SCRIPT_DIR}/vcpkg-patches/qt5-base-install-cross.patch"
QT_HOST_PREFIX_PATCH="${SCRIPT_DIR}/vcpkg-patches/qt5-base-host-prefix.patch"
VCPKG_QMAKE_CROSS_PATCH="${SCRIPT_DIR}/vcpkg-patches/vcpkg-configure-qmake-cross.patch"
QT_SUBMODULE_CROSS_PATCH="${SCRIPT_DIR}/vcpkg-patches/qt5-base-submodule-cross.patch"
QT_EXTERNAL_QMAKE_PATCH="${SCRIPT_DIR}/vcpkg-patches/qt5-base-external-qmake.patch"
PREFIX="${VCPKG_INSTALL_ROOT}/${TRIPLET}"
mkdir -p "${DEPS_ROOT}" "${VCPKG_DOWNLOADS}" "${VCPKG_BINARY_CACHE}"

HOST_QT_TOOLS_ROOT="${VCPKG_INSTALL_ROOT}/filecommander-x64-osx-release/tools/qt5"

if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
    if [[ -e "${VCPKG_ROOT}" ]]; then
        die "${VCPKG_ROOT} exists but is not a vcpkg checkout"
    fi
    git clone --filter=blob:none --no-checkout --no-tags \
        "${VCPKG_URL}" "${VCPKG_ROOT}"
fi

# A failed arm64 attempt may have left temporary Qt port patches in the
# ignored vcpkg checkout. Restore only those generated files before the
# cleanliness check; source-tree changes are never touched by this script.
git -C "${VCPKG_ROOT}" restore -- \
    ports/qt5-base/cmake/configure_qt.cmake \
    ports/qt5-base/cmake/install_qt.cmake \
    ports/qt5-base/portfile.cmake \
    ports/qt5-base/cmake/qt_build_submodule.cmake \
    scripts/cmake/vcpkg_configure_qmake.cmake 2>/dev/null || true

if [[ -f "${VCPKG_ROOT}/bootstrap-vcpkg.sh" ]] &&
   [[ -n "$(git -C "${VCPKG_ROOT}" status --porcelain)" ]]; then
    die "vcpkg checkout has local changes: ${VCPKG_ROOT}"
fi

git -C "${VCPKG_ROOT}" fetch --depth=1 origin "${VCPKG_REF}"
git -C "${VCPKG_ROOT}" checkout --detach "${VCPKG_REF}"
[[ -z "$(git -C "${VCPKG_ROOT}" status --porcelain)" ]] ||
    die "vcpkg checkout has local changes after checkout: ${VCPKG_ROOT}"

if [[ ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
    (
        cd "${VCPKG_ROOT}"
        ./bootstrap-vcpkg.sh -disableMetrics
    )
fi

QT_CROSS_PATCH_APPLIED=0
QT_PORT_CROSS_PATCH_APPLIED=0
QT_INSTALL_CROSS_PATCH_APPLIED=0
QT_HOST_PREFIX_PATCH_APPLIED=0
VCPKG_QMAKE_CROSS_PATCH_APPLIED=0
QT_SUBMODULE_CROSS_PATCH_APPLIED=0
restore_qt_cross_patch() {
    if ((QT_CROSS_PATCH_APPLIED)); then
        git -C "${VCPKG_ROOT}" restore -- ports/qt5-base/cmake/configure_qt.cmake
    fi
    if ((QT_HOST_PREFIX_PATCH_APPLIED)); then
        git -C "${VCPKG_ROOT}" restore -- ports/qt5-base/cmake/configure_qt.cmake
    fi
    if ((QT_INSTALL_CROSS_PATCH_APPLIED)); then
        git -C "${VCPKG_ROOT}" restore -- ports/qt5-base/cmake/install_qt.cmake
    fi
    if ((QT_PORT_CROSS_PATCH_APPLIED)); then
        git -C "${VCPKG_ROOT}" restore -- ports/qt5-base/portfile.cmake
    fi
    if ((VCPKG_QMAKE_CROSS_PATCH_APPLIED)); then
        git -C "${VCPKG_ROOT}" restore -- scripts/cmake/vcpkg_configure_qmake.cmake
    fi
    if ((QT_SUBMODULE_CROSS_PATCH_APPLIED)); then
        git -C "${VCPKG_ROOT}" restore -- ports/qt5-base/cmake/qt_build_submodule.cmake
    fi
}
trap restore_qt_cross_patch EXIT

prepare_arm64_qt_host_tools() {
    # The Qt 5 port needs host executables such as moc and rcc while it is
    # cross-compiling the target libraries. Pass the already-built Intel Qt
    # tools explicitly because this vcpkg revision does not reliably discover
    # a custom host triplet on macOS.
    local qt_cross_tools_root="${DEPS_ROOT}/qt-host-tools-arm64"
    local target_qmodule_pri="${VCPKG_ROOT}/buildtrees/qt5-base/${TRIPLET}-rel/mkspecs/qmodule.pri"
    local preserved_qmodule_pri="${qt_cross_tools_root}/qmodule.pri.arm64-preserved"
    mkdir -p "${qt_cross_tools_root}/bin"

    if [[ ! -x "${HOST_QT_TOOLS_ROOT}/bin/qmake" ]]; then
        printf 'Preparing x86_64 Qt host tools for the arm64 cross build\n'
        "${VCPKG_ROOT}/vcpkg" install \
            qt5-base \
            --triplet=filecommander-x64-osx-release \
            --overlay-triplets="${TRIPLET_DIR}" \
            --x-install-root="${VCPKG_INSTALL_ROOT}"
    fi

    [[ -x "${HOST_QT_TOOLS_ROOT}/bin/qmake" ]] ||
        die "x86_64 Qt host tools were not built at ${HOST_QT_TOOLS_ROOT}"

    cp "${HOST_QT_TOOLS_ROOT}/bin/qmake" \
        "${qt_cross_tools_root}/bin/qmake.bin"
    if [[ -f "${qt_cross_tools_root}/mkspecs/qmodule.pri" ]] &&
       grep -q 'QT_CPU_FEATURES\.arm64[[:space:]]*=' \
           "${qt_cross_tools_root}/mkspecs/qmodule.pri"; then
        cp "${qt_cross_tools_root}/mkspecs/qmodule.pri" \
            "${preserved_qmodule_pri}"
    else
        rm -f "${preserved_qmodule_pri}"
    fi
    rm -rf "${qt_cross_tools_root}/mkspecs"
    cp -R "${HOST_QT_TOOLS_ROOT}/mkspecs" \
        "${qt_cross_tools_root}/mkspecs"
    if [[ -f "${target_qmodule_pri}" ]] &&
       grep -q 'QT_CPU_FEATURES\.arm64[[:space:]]*=' \
           "${target_qmodule_pri}"; then
        # A retry may reuse an installed qt5-base and skip its configure
        # phase. Reuse the target metadata from the previous cross build
        # instead of replacing it with the x86_64 host metadata above.
        cp "${target_qmodule_pri}" \
            "${qt_cross_tools_root}/mkspecs/qmodule.pri"
    elif [[ -f "${preserved_qmodule_pri}" ]]; then
        cp "${preserved_qmodule_pri}" \
            "${qt_cross_tools_root}/mkspecs/qmodule.pri"
    fi
    rm -f "${preserved_qmodule_pri}"

    {
        cat <<'EOF'
host_build {
    QT_ARCH = x86_64
    QT_BUILDABI = x86_64-little_endian-lp64
    QT_TARGET_ARCH = arm64
    QT_TARGET_BUILDABI = arm64-little_endian-lp64
} else {
    QT_ARCH = arm64
    QT_BUILDABI = arm64-little_endian-lp64
}
host_build {
    QT_ARCHS = $$QT_ARCH
} else {
    QT_ARCHS = $$QT_ARCH
}
EOF
        sed \
            -e '/^QT_ARCH = /d' \
            -e '/^QT_BUILDABI = /d' \
            -e '/^QT_ARCHS = /d' \
            -e 's/^QT.global.enabled_features = /QT.global.enabled_features = cross_compile /' \
            -e 's/^QT.global.disabled_features = cross_compile /QT.global.disabled_features = /' \
            -e 's/^QT_CONFIG += /QT_CONFIG += cross_compile /' \
            -e 's/^CONFIG += /CONFIG += cross_compile /' \
            "${HOST_QT_TOOLS_ROOT}/mkspecs/qconfig.pri"
    } > "${qt_cross_tools_root}/mkspecs/qconfig.pri"

    cat > "${qt_cross_tools_root}/bin/qt.conf" <<EOF
[DevicePaths]
Prefix=${PREFIX}
Headers=include/qt5
LibraryExecutables=${qt_cross_tools_root}/bin
Imports=${qt_cross_tools_root}/imports
ArchData=${qt_cross_tools_root}
Data=${qt_cross_tools_root}
Translations=${qt_cross_tools_root}/translations
Examples=${qt_cross_tools_root}/examples
[Paths]
Prefix=${PREFIX}
Headers=include/qt5
Libraries=lib
LibraryExecutables=${qt_cross_tools_root}/bin
Imports=${qt_cross_tools_root}/imports
ArchData=${qt_cross_tools_root}
Data=${qt_cross_tools_root}
Translations=${qt_cross_tools_root}/translations
Examples=${qt_cross_tools_root}/examples
HostPrefix=${qt_cross_tools_root}
TargetSpec=macx-clang
HostSpec=macx-clang
EOF

    # qmake itself remains an x86_64 host executable. The target architecture
    # comes from the cross qconfig and the -xplatform argument in the patched
    # Qt port, so no arm64 build flags may be appended to this wrapper.
    cat > "${qt_cross_tools_root}/bin/qmake" <<EOF
#!/usr/bin/env bash
exec "${qt_cross_tools_root}/bin/qmake.bin" "\$@"
EOF
    chmod +x "${qt_cross_tools_root}/bin/qmake"
    for tool in moc rcc uic lrelease lupdate tracegen qdbusxml2cpp qdbuscpp2xml qlalr; do
        ln -sfn "${HOST_QT_TOOLS_ROOT}/bin/${tool}" \
            "${qt_cross_tools_root}/bin/${tool}"
    done
    export FILECOMMANDER_QT_HOST_TOOLS_ROOT="${qt_cross_tools_root}"
}

normalize_arm64_qt_configs() {
    [[ "${ARCH}" == "arm64" ]] || return 0
    [[ -n "${FILECOMMANDER_QT_HOST_TOOLS_ROOT:-}" ]] || return 0

    for qt_config in \
        "${PREFIX}/tools/qt5/qt_release.conf" \
        "${PREFIX}/tools/qt5/qt_debug.conf"; do
        if [[ -f "${qt_config}" ]]; then
            sed -i '' -E \
                "s|^HostPrefix=.*$|HostPrefix=${FILECOMMANDER_QT_HOST_TOOLS_ROOT}|" \
                "${qt_config}"
        fi
    done
}

if [[ "${ARCH}" == "arm64" ]]; then
    prepare_arm64_qt_host_tools
    [[ -s "${QT_CROSS_PATCH}" ]] ||
        die "missing Qt cross-build patch: ${QT_CROSS_PATCH}"
    [[ -s "${QT_PORT_CROSS_PATCH}" ]] ||
        die "missing Qt port cross-build patch: ${QT_PORT_CROSS_PATCH}"
    [[ -s "${QT_INSTALL_CROSS_PATCH}" ]] ||
        die "missing Qt install cross-build patch: ${QT_INSTALL_CROSS_PATCH}"
    [[ -s "${QT_HOST_PREFIX_PATCH}" ]] ||
        die "missing Qt host-prefix patch: ${QT_HOST_PREFIX_PATCH}"
    [[ -s "${VCPKG_QMAKE_CROSS_PATCH}" ]] ||
        die "missing vcpkg qmake cross-build patch: ${VCPKG_QMAKE_CROSS_PATCH}"
    [[ -s "${QT_SUBMODULE_CROSS_PATCH}" ]] ||
        die "missing Qt submodule cross-build patch: ${QT_SUBMODULE_CROSS_PATCH}"
    [[ -s "${QT_EXTERNAL_QMAKE_PATCH}" ]] ||
        die "missing Qt external qmake patch: ${QT_EXTERNAL_QMAKE_PATCH}"
    git -C "${VCPKG_ROOT}" apply --check "${QT_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply --check "${QT_PORT_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply --check "${QT_INSTALL_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply --check "${QT_HOST_PREFIX_PATCH}"
    git -C "${VCPKG_ROOT}" apply --check "${VCPKG_QMAKE_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply --check "${QT_SUBMODULE_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply "${QT_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply "${QT_PORT_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply "${QT_INSTALL_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply "${QT_HOST_PREFIX_PATCH}"
    git -C "${VCPKG_ROOT}" apply "${VCPKG_QMAKE_CROSS_PATCH}"
    git -C "${VCPKG_ROOT}" apply "${QT_SUBMODULE_CROSS_PATCH}"
    QT_CROSS_PATCH_APPLIED=1
    QT_PORT_CROSS_PATCH_APPLIED=1
    QT_INSTALL_CROSS_PATCH_APPLIED=1
    QT_HOST_PREFIX_PATCH_APPLIED=1
    VCPKG_QMAKE_CROSS_PATCH_APPLIED=1
    QT_SUBMODULE_CROSS_PATCH_APPLIED=1
    export FILECOMMANDER_QMAKE_AUX_PATCH="${QT_EXTERNAL_QMAKE_PATCH}"
    if [[ -f "${PREFIX}/share/qt5/qt_build_submodule.cmake" ]]; then
        cp "${VCPKG_ROOT}/ports/qt5-base/cmake/qt_build_submodule.cmake" \
            "${PREFIX}/share/qt5/qt_build_submodule.cmake"
    fi
else
    unset FILECOMMANDER_QMAKE_AUX_PATCH
    export FILECOMMANDER_QT_HOST_TOOLS_ROOT="${HOST_QT_TOOLS_ROOT}"
fi

export VCPKG_ROOT
export VCPKG_DOWNLOADS
export VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_BINARY_CACHE}"
export MACOSX_DEPLOYMENT_TARGET=12.0

PACKAGES=(
    qt5-base
    qt5-websockets
    qt5-tools
    libarchive
    libssh2
    curl
    openssl
    zlib
    gtest
)

printf 'Installing macOS %s dependencies into %s\n' "${ARCH}" "${PREFIX}"
"${VCPKG_ROOT}/vcpkg" install \
    "${PACKAGES[@]}" \
    --recurse \
    --triplet="${TRIPLET}" \
    --host-triplet=filecommander-x64-osx-release \
    --overlay-triplets="${TRIPLET_DIR}" \
    --x-install-root="${VCPKG_INSTALL_ROOT}"

normalize_arm64_qt_configs

expected_arch="${ARCH}"
for candidate in \
    "${PREFIX}/lib/QtCore.framework/QtCore" \
    "${PREFIX}/lib/libarchive.dylib" \
    "${PREFIX}/lib/libcurl.dylib" \
    "${PREFIX}/lib/libssl.dylib" \
    "${PREFIX}/lib/libcrypto.dylib"; do
    if [[ -e "${candidate}" ]]; then
        arch_info="$(/usr/bin/lipo -info "${candidate}" 2>&1)"
        [[ "${arch_info}" == *"${expected_arch}"* ]] ||
            die "dependency has the wrong architecture: ${candidate} (${arch_info})"
    fi
done

mkdir -p "${PREFIX}"
cat > "${PREFIX}/.filecommander-dependencies" <<EOF
filecommander_vcpkg_ref=${VCPKG_REF}
filecommander_triplet=${TRIPLET}
filecommander_arch=${ARCH}
filecommander_macos_deployment_target=12.0
EOF

printf 'Dependencies ready: %s\n' "${PREFIX}"
