[CmdletBinding()]
param(
    # The SDK directory that already hosts Qt, vcpkg and poppler. The two OpenSSL
    # 1.1.1 DLLs land in <SdkRoot>\openssl-1.1.1\bin\.
    [string]$SdkRoot = (Join-Path $env:LOCALAPPDATA 'FileCommanderSDK'),
    # Pinned to the final 1.1.1 release. This is deliberately NOT moved to 3.x:
    # Qt 5.15.2's QSslSocket/QWebSocket are built against the 1.1.1 ABI and load
    # libssl-1_1-x64.dll / libcrypto-1_1-x64.dll by name -- they do not recognise
    # the OpenSSL 3.x DLL names, and OpenSSL 3.x is already linked directly by
    # the app for ShareIdentity's certificate generation.
    [string]$Version = '1.1.1w'
)

$ErrorActionPreference = 'Stop'

# Requires a Windows-native Perl (Strawberry Perl) on PATH: OpenSSL's Configure
# cannot run under Cygwin/MSYS Perl, which rewrites Windows paths. This machine
# only has Cygwin Perl, so the two 1.1.1 DLLs are currently sourced pre-built
# instead -- this script exists for the CI/provenance path where a native Perl
# is available.

function Find-VsDevCmd {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $candidate = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
        }
    }
    $fallback = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    if (Test-Path -LiteralPath $fallback -PathType Leaf) { return (Resolve-Path -LiteralPath $fallback).Path }
    throw 'VsDevCmd.bat not found'
}

$prefix = Join-Path $SdkRoot 'openssl-1.1.1'
$bin = Join-Path $prefix 'bin'

$sslDll = Join-Path $bin 'libssl-1_1-x64.dll'
$cryptoDll = Join-Path $bin 'libcrypto-1_1-x64.dll'
if ((Test-Path -LiteralPath $sslDll -PathType Leaf) -and
    (Test-Path -LiteralPath $cryptoDll -PathType Leaf)) {
    Write-Host "==> OpenSSL $Version already present at $bin"
    exit 0
}

$work = Join-Path $SdkRoot 'openssl-111-src'
New-Item -ItemType Directory -Force -Path $work | Out-Null

$archive = Join-Path $work "openssl-$Version.tar.gz"
if (-not (Test-Path -LiteralPath $archive)) {
    $url = "https://www.openssl.org/source/old/1.1.1/openssl-$Version.tar.gz"
    Write-Host "==> Fetching $url"
    Invoke-WebRequest -Uri $url -OutFile $archive
}

$sourceDir = Join-Path $work "openssl-$Version"
if (-not (Test-Path -LiteralPath $sourceDir)) {
    Write-Host "==> Extracting openssl $Version"
    & (Join-Path $env:SystemRoot 'system32\tar.exe') -xzf $archive -C $work
    if ($LASTEXITCODE -ne 0) { throw "Could not extract $archive" }
    if (-not (Test-Path -LiteralPath $sourceDir)) {
        throw "Extraction reported success but $sourceDir is not there"
    }
}

# Build the 1.1.1 DLLs with nmake. no-asm skips the assembly code, so the build
# needs no NASM -- only a Perl (for Configure) and the MSVC toolchain.
$vsDevCmd = Find-VsDevCmd
$envLines = & cmd.exe /s /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set"
foreach ($line in $envLines) {
    $eq = $line.IndexOf('=')
    if ($eq -le 0) { continue }
    Set-Item -Path "Env:$($line.Substring(0, $eq))" -Value $line.Substring($eq + 1)
}

Write-Host '==> Configuring OpenSSL (VC-WIN64A, shared, no-asm)'
Push-Location $sourceDir
try {
    perl Configure VC-WIN64A no-asm shared "--prefix=$prefix" "--openssldir=$prefix\ssl"
    if ($LASTEXITCODE -ne 0) { throw 'OpenSSL Configure failed' }
    nmake
    if ($LASTEXITCODE -ne 0) { throw 'OpenSSL nmake failed' }
} finally {
    Pop-Location
}

# Only the two runtime DLLs matter here: Qt loads them by name. Take them from
# the build root rather than nmake install, so the import libs and headers never
# leak into the SDK (the app must keep linking OpenSSL 3.x for ShareIdentity).
$builtSsl = Join-Path $sourceDir 'libssl-1_1-x64.dll'
$builtCrypto = Join-Path $sourceDir 'libcrypto-1_1-x64.dll'
foreach ($dll in @($builtSsl, $builtCrypto)) {
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
        throw "Build produced no $dll -- did nmake run?"
    }
}

New-Item -ItemType Directory -Force -Path $bin | Out-Null
Copy-Item -LiteralPath $builtSsl -Destination $sslDll -Force
Copy-Item -LiteralPath $builtCrypto -Destination $cryptoDll -Force

Write-Host "==> OpenSSL $Version runtime DLLs installed to $bin"
Get-ChildItem -LiteralPath $bin -Filter '*.dll' | ForEach-Object { Write-Host "    $($_.Name)" }
