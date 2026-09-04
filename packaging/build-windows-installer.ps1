[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Architecture = 'x64',
    [string]$MakensisPath,
    [switch]$SkipPortableBuild,
    [switch]$Smoke
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-hash.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$portableBuilder = Join-Path $PSScriptRoot 'build-windows.ps1'

if (-not $SkipPortableBuild) {
    & $portableBuilder -Profile windows-portable -Architecture $Architecture
    if ($LASTEXITCODE) { throw 'Portable package build failed.' }
}

$cmakeLists = Join-Path $repo 'CMakeLists.txt'
$versionMatch = Select-String -LiteralPath $cmakeLists -Pattern '^project\(FileCommander VERSION ([0-9]+(?:\.[0-9]+)*)' |
    Select-Object -First 1
if (-not $versionMatch) { throw "Could not read the version from the project() line in $cmakeLists" }
$version = $versionMatch.Matches[0].Groups[1].Value

$stage = Join-Path $repo "dist/FileCommander-windows-$Architecture"
if (-not (Test-Path -LiteralPath $stage -PathType Container)) {
    throw "Portable package stage not found: $stage"
}
& (Join-Path $PSScriptRoot 'verify-windows-package.ps1') -Stage $stage -Architecture $Architecture
if ($LASTEXITCODE) { throw 'Portable package verification failed.' }

$manifest = Get-Content -LiteralPath (Join-Path $stage 'manifest.json') -Raw | ConvertFrom-Json
if ([string]$manifest.version -ne $version) {
    throw "Stage version $($manifest.version) does not match project version $version."
}

$makensis = $null
if ($MakensisPath) {
    if (-not (Test-Path -LiteralPath $MakensisPath -PathType Leaf)) {
        throw "NSIS compiler not found: $MakensisPath"
    }
    $makensis = (Resolve-Path -LiteralPath $MakensisPath).Path
} else {
    $command = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($command) {
        $makensis = $command.Source
    } else {
        foreach ($candidate in @(
            "$env:ProgramFiles\NSIS\makensis.exe",
            "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
        )) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $makensis = $candidate
                break
            }
        }
    }
}
if (-not $makensis) {
    throw 'NSIS compiler not found. Install NSIS or pass -MakensisPath to makensis.exe.'
}

$output = Join-Path $repo "dist/FileCommander-$version-windows-$Architecture-setup.exe"
$definition = Join-Path $PSScriptRoot 'FileCommander.nsi'
& $makensis "/DPRODUCT_VERSION=$version" "/DSTAGE_DIR=$stage" "/DOUTFILE=$output" $definition
if ($LASTEXITCODE) { throw 'NSIS installer build failed.' }
if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
    throw "NSIS did not create the installer: $output"
}

if ($Smoke) {
    $smokeRoot = Join-Path ([System.IO.Path]::GetTempPath()) "FileCommander-installer-smoke-$([guid]::NewGuid())"
    $smokeInstall = Join-Path $smokeRoot 'install'
    $smokeInstallArg = '/D=' + $smokeInstall
    New-Item -ItemType Directory -Force -Path $smokeRoot | Out-Null
    try {
        $install = Start-Process -FilePath $output -ArgumentList @('/S', $smokeInstallArg) -Wait -PassThru
        if ($install.ExitCode) { throw "Installer smoke install failed with exit code $($install.ExitCode)." }

        $releaseManifest = Get-Content -LiteralPath (Join-Path $stage 'release-manifest.json') -Raw | ConvertFrom-Json
        foreach ($file in @($releaseManifest.files)) {
            $relativePath = ([string]$file.path) -replace '/', '\\'
            $installedPath = Join-Path $smokeInstall $relativePath
            if (-not (Test-Path -LiteralPath $installedPath -PathType Leaf)) {
                throw "Installer smoke install is missing: $($file.path)"
            }
            $hash = (Get-Sha256Hash -LiteralPath $installedPath).ToLowerInvariant()
            if ($hash -ne ([string]$file.sha256).ToLowerInvariant()) {
                throw "Installer smoke hash mismatch: $($file.path)"
            }
        }
        $uninstaller = Join-Path $smokeInstall 'Uninstall.exe'
        if (-not (Test-Path -LiteralPath $uninstaller -PathType Leaf)) {
            throw 'Installer smoke install is missing Uninstall.exe.'
        }

        $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @('/S') -Wait -PassThru
        if ($uninstall.ExitCode) { throw "Installer smoke uninstall failed with exit code $($uninstall.ExitCode)." }
        if (Test-Path -LiteralPath $smokeInstall) {
            throw "Installer smoke uninstall left files at $smokeInstall"
        }
    } finally {
        if (Test-Path -LiteralPath $smokeRoot) {
            Remove-Item -LiteralPath $smokeRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

Write-Host "Created $output"
