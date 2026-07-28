[CmdletBinding()]
param(
    [string]$QtRoot = $env:FILECOMMANDER_QT_ROOT,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$PopplerQt5Root = $env:FILECOMMANDER_POPPLER_QT5_ROOT,
    [string]$MpvRoot = $env:FILECOMMANDER_MPV_ROOT,
    [switch]$WithFullPreviews
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $QtRoot -or -not (Test-Path -LiteralPath $QtRoot)) {
    throw 'Set FILECOMMANDER_QT_ROOT or pass -QtRoot with a Qt 5.15 MSVC installation.'
}
if (-not $VcpkgRoot -or -not (Test-Path -LiteralPath $VcpkgRoot)) {
    throw 'Set VCPKG_ROOT or pass -VcpkgRoot.'
}
if (-not $PopplerQt5Root) {
    $PopplerQt5Root = Join-Path (Split-Path -Parent $VcpkgRoot) 'poppler-qt5'
}
if (-not $MpvRoot) {
    $MpvRoot = Join-Path $repo 'build/mpv-windows/sdk'
}

$build = Join-Path $repo 'build/windows-msvc-release'
$stage = Join-Path $repo 'dist/FileCommander-windows-x64'
$previewArgs = if ($WithFullPreviews) {
    @('-DFILECOMMANDER_PREVIEW_PDF=ON', '-DFILECOMMANDER_PREVIEW_MEDIA=ON')
} else {
    @('-DFILECOMMANDER_PREVIEW_PDF=OFF', '-DFILECOMMANDER_PREVIEW_MEDIA=OFF')
}

cmake -S $repo -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_PREFIX_PATH=$QtRoot" `
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DTTC_BUILD_TESTS=OFF `
    -DTTC_BUILD_BENCH=OFF `
    -DFILECOMMANDER_ENABLE_NETWORK=ON `
    "-DFILECOMMANDER_POPPLER_QT5_ROOT=$PopplerQt5Root" `
    "-DFILECOMMANDER_MPV_ROOT=$MpvRoot" @previewArgs
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
cmake --build $build --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'FileCommander.exe') -Destination $stage -Force
& (Join-Path $QtRoot 'bin/windeployqt.exe') --release --no-translations (Join-Path $stage 'FileCommander.exe')
if ($LASTEXITCODE) { throw 'windeployqt failed.' }

# CMake's runtime-dependency step has already copied the exact vcpkg DLL
# closure beside the executable. Copying every DLL from vcpkg also pulled test
# and compatibility runtimes into the package and could change DLL resolution.
Get-ChildItem -LiteralPath $build -Filter '*.dll' |
    Copy-Item -Destination $stage -Force
if ($WithFullPreviews) {
    if (-not (Test-Path -LiteralPath (Join-Path $PopplerQt5Root 'bin/poppler-qt5.dll'))) {
        throw "Poppler Qt5 runtime not found at $PopplerQt5Root."
    }
    Get-ChildItem -LiteralPath (Join-Path $PopplerQt5Root 'bin') -Filter '*.dll' |
        Copy-Item -Destination $stage -Force
    $vcpkgBin = Join-Path $VcpkgRoot 'installed/x64-windows/bin'
    foreach ($runtime in @('jpeg62.dll', 'openjp2.dll', 'libpng16.dll',
                            'tiff.dll', 'freetype.dll', 'brotlidec.dll',
                            'brotlicommon.dll')) {
        Copy-Item -LiteralPath (Join-Path $vcpkgBin $runtime) -Destination $stage -Force
    }
    if (-not (Test-Path -LiteralPath (Join-Path $MpvRoot 'libmpv-2.dll'))) {
        throw "libmpv runtime not found at $MpvRoot."
    }
    Copy-Item -LiteralPath (Join-Path $MpvRoot 'libmpv-2.dll') -Destination $stage -Force
}
$officeBinary = Join-Path $repo 'build/office-oxide/office-oxide.exe'
if (Test-Path -LiteralPath $officeBinary) {
    Copy-Item -LiteralPath $officeBinary -Destination $stage -Force
}
$manifest = [ordered]@{
    product = 'FileCommander'
    version = '0.2.0-phase2-test'
    platform = 'windows-x64'
    networkProtocols = @('sftp', 'smb', 'ftp', 'webdav', 'webdavs')
    officePreview = Test-Path -LiteralPath (Join-Path $stage 'office-oxide.exe')
    pdfPreview = [bool]$WithFullPreviews
    mediaPreview = [bool]$WithFullPreviews
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8

& (Join-Path $PSScriptRoot 'verify-windows-package.ps1') -Stage $stage
if ($LASTEXITCODE) { throw 'Package verification failed.' }
$zip = Join-Path $repo 'dist/FileCommander-0.2.0-phase2-test-windows-x64.zip'
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
Write-Host "Created $zip"
