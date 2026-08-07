[CmdletBinding()]
param(
    # Matches the SDK this project is developed against. Poppler's Qt5 bindings
    # are frozen upstream (Qt5 support is in maintenance), so this moves rarely
    # -- which is what makes caching the result worthwhile.
    [string]$Version = '26.04.0',
    [string]$QtRoot = $env:FILECOMMANDER_QT_ROOT,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Triplet = 'x64-windows',
    # Where the finished poppler-qt5 install tree goes. This is the value to
    # hand to FILECOMMANDER_POPPLER_QT5_ROOT.
    [string]$Prefix
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# Why this script exists at all:
#
# vcpkg carries poppler and even a `qt` feature, but that feature depends on
# `qtbase`, which in vcpkg means Qt 6 -- it produces poppler-qt6 and links
# against a Qt this project does not use. There is no packaged poppler-qt5 for
# Windows, so the only way to have PDF preview in a hosted CI build is to build
# it here, from source, against the same Qt 5.15 the application uses.
#
# It is slow (minutes), which is why the caller caches $Prefix. Poppler releases
# often but the Qt5 binding does not change, so a cache keyed on this version
# plus the Qt version is stable for months at a time.

if (-not $Prefix) { $Prefix = Join-Path $repo 'build/poppler-qt5' }
if (-not $QtRoot -or -not (Test-Path -LiteralPath $QtRoot)) {
    throw 'Set FILECOMMANDER_QT_ROOT or pass -QtRoot with a Qt 5.15 MSVC installation.'
}
if (-not $VcpkgRoot -or -not (Test-Path -LiteralPath $VcpkgRoot)) {
    throw 'Set VCPKG_ROOT or pass -VcpkgRoot.'
}
# vcvars64.bat -- and ilammy/msvc-dev-cmd, which runs it -- sets VCPKG_ROOT to
# the vcpkg bundled with Visual Studio. That one has no ports tree, so an
# install from it fails with "Could not locate a manifest (vcpkg.json) above the
# current working directory", which says nothing about the actual problem. Cost
# an afternoon once; caught here instead.
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'ports'))) {
    throw ("$VcpkgRoot has no ports directory, so it is not a usable vcpkg checkout. " +
           'Visual Studio sets VCPKG_ROOT to its own bundled copy; pass -VcpkgRoot ' +
           'explicitly, or set VCPKG_ROOT after loading the MSVC environment.')
}

# Already built (a warm cache): say so and stop, so the caller can call this
# unconditionally.
$marker = Join-Path $Prefix 'bin/poppler-qt5.dll'
if (Test-Path -LiteralPath $marker) {
    Write-Host "==> poppler-qt5 $Version already present at $Prefix"
    exit 0
}

$work = Join-Path $repo 'build/poppler-src'
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Poppler's own release tarballs, which are the canonical source and are served
# over https with a stable URL shape.
$archive = Join-Path $work "poppler-$Version.tar.xz"
if (-not (Test-Path -LiteralPath $archive)) {
    $url = "https://poppler.freedesktop.org/poppler-$Version.tar.xz"
    Write-Host "==> Fetching $url"
    Invoke-WebRequest -Uri $url -OutFile $archive
}

$sourceDir = Join-Path $work "poppler-$Version"
if (-not (Test-Path -LiteralPath $sourceDir)) {
    Write-Host "==> Extracting poppler $Version"
    # The FULL path to Windows' own bsdtar, never a bare `tar`.
    #
    # A bare `tar` resolves through PATH, and a GitHub Windows runner has Git's
    # GNU tar on PATH as well as this one. GNU tar reads `-f D:\a\...` as the
    # `host:path` remote-archive syntax -- the drive letter becomes a hostname --
    # and goes off to contact a machine called "D". Locally that fails at once
    # ("Cannot connect to C: resolve failed"); inside Azure, where the runners
    # live, the lookup does not fail fast, and the step simply hangs. Measured:
    # four CI runs in a row died here, the last one after 4h36m, and not one of
    # them ever reached the compile the step is named for.
    #
    # bsdtar has no remote-archive syntax at all, so a drive letter is just a
    # drive letter. It ships with Windows 10 and Server 2019 onward.
    $bsdtar = Join-Path $env:SystemRoot 'system32\tar.exe'
    if (-not (Test-Path -LiteralPath $bsdtar)) {
        throw "Windows' bundled tar is missing at $bsdtar"
    }
    & $bsdtar -xf $archive -C $work
    if ($LASTEXITCODE -ne 0) { throw "Could not extract $archive" }
    if (-not (Test-Path -LiteralPath $sourceDir)) {
        throw "Extraction reported success but $sourceDir is not there"
    }
}

# The dependencies poppler needs that are NOT part of the Qt5 install. Kept in
# step with the `pdf` group in packaging/profiles/windows-portable.json, which
# is what the packager copies next to the executable -- if this list grows, that
# one has to grow with it or the package will be missing a runtime DLL.
Write-Host '==> Installing poppler build dependencies through vcpkg'
# --vcpkg-root, rather than relying on the working directory. A recent vcpkg
# (2026-07-13) invoked from anywhere but its own root refuses the classic
# `install <ports>` form with "Could not locate a manifest (vcpkg.json) above
# the current working directory". Push-Location does NOT fix that: it moves
# PowerShell's location, while a native process reads the real working
# directory, which is unchanged. Measured, all three ways, on this machine.
# libiconv is here because poppler does find_package(Iconv REQUIRED) and the
# MSVC C library supplies none, so CMake's FindIconv comes up empty and the
# configure step dies with "Could NOT find Iconv". It is easy to miss on a
# developer machine, where some other port has usually pulled it in already; a
# fresh CI vcpkg has not.
& "$VcpkgRoot/vcpkg.exe" install --triplet $Triplet --vcpkg-root="$VcpkgRoot" `
    freetype libiconv libjpeg-turbo libpng openjpeg zlib
if ($LASTEXITCODE -ne 0) { throw 'vcpkg failed to install the poppler dependencies' }

$build = Join-Path $work "build-$Triplet"
# Qt AND the vcpkg install tree, as one list. Setting CMAKE_PREFIX_PATH to Qt
# alone on the command line replaces what the vcpkg toolchain would otherwise
# contribute, and poppler then cannot find the dependencies vcpkg just built --
# measured, it fails at "Could NOT find Freetype" with freetype sitting in
# vcpkg's installed tree the whole time.
Write-Host "==> Configuring poppler $Version (Qt5 bindings only)"
cmake -S $sourceDir -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_INSTALL_PREFIX="$Prefix" `
    -DCMAKE_TOOLCHAIN_FILE="$VcpkgRoot/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=$Triplet `
    -DCMAKE_PREFIX_PATH="$QtRoot;$VcpkgRoot/installed/$Triplet" `
    -DENABLE_QT5=ON `
    -DENABLE_QT6=OFF `
    -DENABLE_GLIB=OFF `
    -DENABLE_GOBJECT_INTROSPECTION=OFF `
    -DENABLE_CPP=ON `
    -DENABLE_UTILS=OFF `
    -DBUILD_GTK_TESTS=OFF `
    -DBUILD_QT5_TESTS=OFF `
    -DBUILD_QT6_TESTS=OFF `
    -DBUILD_CPP_TESTS=OFF `
    -DBUILD_MANUAL_TESTS=OFF `
    -DENABLE_BOOST=OFF `
    -DENABLE_LIBCURL=OFF `
    -DENABLE_LCMS=OFF `
    -DENABLE_NSS3=OFF `
    -DENABLE_GPGME=OFF `
    -DENABLE_LIBTIFF=OFF
if ($LASTEXITCODE -ne 0) { throw 'poppler configure failed' }

Write-Host '==> Building poppler'
cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw 'poppler build failed' }

cmake --install $build
if ($LASTEXITCODE -ne 0) { throw 'poppler install failed' }

if (-not (Test-Path -LiteralPath $marker)) {
    throw "poppler installed but $marker is missing -- the Qt5 bindings did not build."
}

# The application links against the Qt5 binding; the packager then copies these
# and the vcpkg runtime DLLs beside the executable. Report what landed so a CI
# log shows the version that went into the package.
Write-Host "==> poppler-qt5 $Version installed to $Prefix"
Get-ChildItem -LiteralPath (Join-Path $Prefix 'bin') -Filter '*.dll' |
    ForEach-Object { Write-Host "    $($_.Name)" }
