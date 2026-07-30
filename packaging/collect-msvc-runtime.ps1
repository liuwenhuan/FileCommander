[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Architecture = 'x64',
    [ValidateSet('Portable')]
    [string]$Mode = 'Portable',
    [string]$VsWherePath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Stage -PathType Container)) {
    throw "Runtime stage directory does not exist: $Stage"
}

if (-not $VsWherePath) {
    $VsWherePath = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/Installer/vswhere.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $VsWherePath -or -not (Test-Path -LiteralPath $VsWherePath -PathType Leaf)) {
    throw 'vswhere.exe was not found. Install Visual Studio Build Tools with the MSVC C++ workload.'
}

$installationPath = & $VsWherePath -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1
if (-not $installationPath) {
    throw 'vswhere.exe did not return a Visual Studio installation with the MSVC C++ toolchain.'
}
$installationPath = $installationPath.Trim()
if (-not (Test-Path -LiteralPath $installationPath -PathType Container)) {
    throw 'vswhere.exe did not return a Visual Studio installation with the MSVC C++ toolchain.'
}

$versionFile = Join-Path $installationPath 'VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt'
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
    throw "MSVC redistributable version file was not found: $versionFile"
}
$redistVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if (-not $redistVersion) {
    throw "MSVC redistributable version file is empty: $versionFile"
}

$sourceRedistDirectory = Join-Path $installationPath "VC/Redist/MSVC/$redistVersion/$Architecture/Microsoft.VC143.CRT"
if (-not (Test-Path -LiteralPath $sourceRedistDirectory -PathType Container)) {
    throw "MSVC redistributable runtime version $redistVersion for $Architecture was not found at $sourceRedistDirectory. Repair or install the matching Visual Studio C++ redistributable components."
}

$runtimeFiles = Get-ChildItem -LiteralPath $sourceRedistDirectory -File -Filter '*.dll' |
    Where-Object { $_.Name -notmatch 'd\.dll$' }
if (-not $runtimeFiles) {
    throw "No release CRT DLLs were found at $sourceRedistDirectory."
}

$copiedCrtDllPaths = foreach ($runtimeFile in $runtimeFiles) {
    $destination = Join-Path $Stage $runtimeFile.Name
    Copy-Item -LiteralPath $runtimeFile.FullName -Destination $destination -Force
    (Resolve-Path -LiteralPath $destination).Path
}

[pscustomobject]@{
    CopiedCrtDllPaths = @($copiedCrtDllPaths)
    SourceRedistDirectory = (Resolve-Path -LiteralPath $sourceRedistDirectory).Path
    Provenance = 'msvc-runtime'
    Mode = $Mode
}
