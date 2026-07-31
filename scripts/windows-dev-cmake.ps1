param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CMakeArgs
)

$ErrorActionPreference = 'Stop'

if (-not $CMakeArgs -or $CMakeArgs.Count -eq 0) {
    throw 'Usage: scripts/windows-dev-cmake.ps1 <cmake arguments>'
}

function Find-VsDevCmd {
    $candidates = @()
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $candidates += (Join-Path $installPath 'Common7\Tools\VsDevCmd.bat')
        }
    }
    $candidates += @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'Visual Studio Build Tools with the MSVC x64 toolchain were not found.'
}

function Import-VsDevEnvironment([string]$VsDevCmd) {
    $envLines = & cmd.exe /s /c "`"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd failed with exit code $LASTEXITCODE."
    }
    foreach ($line in $envLines) {
        $equals = $line.IndexOf('=')
        if ($equals -le 0) { continue }
        $name = $line.Substring(0, $equals)
        $value = $line.Substring($equals + 1)
        Set-Item -Path "Env:$name" -Value $value
    }
}

function Resolve-DefaultPath([string]$Current, [string]$Fallback, [string]$Name) {
    if ($Current -and (Test-Path -LiteralPath $Current)) {
        return (Resolve-Path -LiteralPath $Current).Path
    }
    if (Test-Path -LiteralPath $Fallback) {
        return (Resolve-Path -LiteralPath $Fallback).Path
    }
    throw "$Name was not set and the default path was not found: $Fallback"
}

$requestedQtRoot = $env:FILECOMMANDER_QT_ROOT
$requestedVcpkgRoot = $env:VCPKG_ROOT
$requestedMpvRoot = $env:FILECOMMANDER_MPV_ROOT
$requestedPopplerQt5Root = $env:FILECOMMANDER_POPPLER_QT5_ROOT

Import-VsDevEnvironment (Find-VsDevCmd)

$sdkRoot = Join-Path $env:LOCALAPPDATA 'FileCommanderSDK'
$env:FILECOMMANDER_QT_ROOT = Resolve-DefaultPath $requestedQtRoot `
    (Join-Path $sdkRoot 'Qt\5.15.2\msvc2019_64') 'FILECOMMANDER_QT_ROOT'
$env:VCPKG_ROOT = Resolve-DefaultPath $requestedVcpkgRoot `
    (Join-Path $sdkRoot 'vcpkg') 'VCPKG_ROOT'
if ($requestedMpvRoot -or (Test-Path -LiteralPath (Join-Path $sdkRoot 'mpv-x64'))) {
    $env:FILECOMMANDER_MPV_ROOT = Resolve-DefaultPath $requestedMpvRoot `
        (Join-Path $sdkRoot 'mpv-x64') 'FILECOMMANDER_MPV_ROOT'
}
if ($requestedPopplerQt5Root -or (Test-Path -LiteralPath (Join-Path $sdkRoot 'poppler-qt5'))) {
    $env:FILECOMMANDER_POPPLER_QT5_ROOT = Resolve-DefaultPath $requestedPopplerQt5Root `
        (Join-Path $sdkRoot 'poppler-qt5') 'FILECOMMANDER_POPPLER_QT5_ROOT'
}

if ($CMakeArgs[0] -notin @('--build', '--install', '-E')) {
    if (-not ($CMakeArgs | Where-Object { $_ -like '-DCMAKE_PREFIX_PATH=*' })) {
        $CMakeArgs += "-DCMAKE_PREFIX_PATH=$env:FILECOMMANDER_QT_ROOT"
    }
    if (-not ($CMakeArgs | Where-Object { $_ -like '-DCMAKE_TOOLCHAIN_FILE=*' })) {
        $CMakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
    }
    if (-not ($CMakeArgs | Where-Object { $_ -like '-DVCPKG_TARGET_TRIPLET=*' })) {
        $CMakeArgs += '-DVCPKG_TARGET_TRIPLET=x64-windows'
    }
}

& cmake @CMakeArgs
exit $LASTEXITCODE
