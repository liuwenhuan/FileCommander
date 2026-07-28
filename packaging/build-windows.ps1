[CmdletBinding()]
param(
    [string]$QtRoot = $env:FILECOMMANDER_QT_ROOT,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
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
    -DFILECOMMANDER_ENABLE_NETWORK=OFF @previewArgs
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
cmake --build $build --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }

New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'FileCommander.exe') -Destination $stage -Force
& (Join-Path $QtRoot 'bin/windeployqt.exe') --release --no-translations (Join-Path $stage 'FileCommander.exe')
if ($LASTEXITCODE) { throw 'windeployqt failed.' }

$vcpkgBin = Join-Path $VcpkgRoot 'installed/x64-windows/bin'
Get-ChildItem -LiteralPath $vcpkgBin -Filter '*.dll' | Copy-Item -Destination $stage -Force
$officeBinary = Join-Path $repo 'build/office-oxide/office-oxide.exe'
if (Test-Path -LiteralPath $officeBinary) {
    Copy-Item -LiteralPath $officeBinary -Destination $stage -Force
}
$manifest = [ordered]@{
    product = 'FileCommander'
    version = '0.1.0'
    platform = 'windows-x64'
    officePreview = Test-Path -LiteralPath (Join-Path $stage 'office-oxide.exe')
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8

& (Join-Path $PSScriptRoot 'verify-windows-package.ps1') -Stage $stage
if ($LASTEXITCODE) { throw 'Package verification failed.' }
$zip = Join-Path $repo 'dist/FileCommander-0.1.0-windows-x64.zip'
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
Write-Host "Created $zip"
