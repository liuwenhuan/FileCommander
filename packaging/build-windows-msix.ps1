[CmdletBinding()]
param(
    [string]$QtRoot = $env:FILECOMMANDER_QT_ROOT,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$PopplerQt5Root = $env:FILECOMMANDER_POPPLER_QT5_ROOT,
    [string]$MpvRoot = $env:FILECOMMANDER_MPV_ROOT,
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Architecture = 'x64',
    [string]$IdentityName = 'FileCommander',
    [string]$Publisher = 'CN=FileCommander',
    [string]$CertificatePath,
    [string]$CertificatePassword,
    [switch]$SkipPortableBuild,
    [switch]$WithFullPreviews
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $SkipPortableBuild) {
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -QtRoot $QtRoot -VcpkgRoot $VcpkgRoot `
        -PopplerQt5Root $PopplerQt5Root -MpvRoot $MpvRoot -Architecture $Architecture `
        -Profile windows-full-portable -WithFullPreviews:$WithFullPreviews
    if ($LASTEXITCODE) { throw 'Portable package build failed.' }
}

$stage = Join-Path $repo "dist/FileCommander-windows-$Architecture"
& (Join-Path $PSScriptRoot 'verify-windows-package.ps1') `
    -Stage $stage -Architecture $Architecture
if ($LASTEXITCODE) { throw 'Staged package verification failed.' }

$portableReleaseManifestPath = Join-Path $stage 'release-manifest.json'
if (-not (Test-Path -LiteralPath $portableReleaseManifestPath)) {
    throw 'Portable stage is missing release-manifest.json.'
}
$portableReleaseManifest = Get-Content -LiteralPath $portableReleaseManifestPath -Raw | ConvertFrom-Json
$provenanceEntries = [System.Collections.Generic.List[object]]::new()
foreach ($file in @($portableReleaseManifest.files)) {
    [void]$provenanceEntries.Add([pscustomobject]@{ path = $file.path; provenance = $file.provenance })
}
function Add-MsixProvenance {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Group)

    $root = (Resolve-Path -LiteralPath $stage).Path.TrimEnd('\')
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    [void]$provenanceEntries.Add([pscustomobject]@{
        path = $resolved.Substring($root.Length + 1).Replace('\', '/')
        provenance = $Group
    })
}

$assets = Join-Path $stage 'Assets'
New-Item -ItemType Directory -Force -Path $assets | Out-Null
$icon = [System.Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL9OwAAAABJRU5ErkJggg==')
foreach ($name in @('Square44x44Logo.png', 'Square150x150Logo.png', 'Wide310x150Logo.png')) {
    $iconPath = Join-Path $assets $name
    [System.IO.File]::WriteAllBytes($iconPath, $icon)
    Add-MsixProvenance -Path $iconPath -Group 'application'
}

$version = '0.2.0.0'
$manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
         IgnorableNamespaces="uap rescap">
  <Identity Name="$IdentityName" Publisher="$Publisher" Version="$version" ProcessorArchitecture="$Architecture" />
  <Properties><DisplayName>FileCommander</DisplayName><PublisherDisplayName>FileCommander</PublisherDisplayName><Logo>Assets\Square150x150Logo.png</Logo></Properties>
  <Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0" /><PackageDependency Name="Microsoft.VCLibs.140.00.UWPDesktop" Publisher="CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" MinVersion="14.0.0.0" /></Dependencies>
  <Resources><Resource Language="en-us" /></Resources>
  <Applications><Application Id="App" Executable="FileCommander.exe" EntryPoint="Windows.FullTrustApplication"><uap:VisualElements DisplayName="FileCommander" Description="FileCommander" BackgroundColor="transparent" Square150x150Logo="Assets\Square150x150Logo.png" Square44x44Logo="Assets\Square44x44Logo.png" /></Application></Applications>
  <Capabilities><Capability Name="internetClient" /><rescap:Capability Name="runFullTrust" /></Capabilities>
</Package>
"@
$appxManifestPath = Join-Path $stage 'AppxManifest.xml'
Set-Content -LiteralPath $appxManifestPath -Value $manifest -Encoding UTF8
Add-MsixProvenance -Path $appxManifestPath -Group 'application'

& (Join-Path $PSScriptRoot 'write-windows-manifest.ps1') -Stage $stage `
    -ProfilePath (Join-Path $PSScriptRoot 'profiles/windows-full-msix.json') `
    -Architecture $Architecture -BuildType Release -ProvenanceEntries $provenanceEntries.ToArray()

$makeAppx = Get-Command MakeAppx.exe -ErrorAction SilentlyContinue
if (-not $makeAppx) {
    $makeAppx = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Filter MakeAppx.exe -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
}
if (-not $makeAppx) { throw 'MakeAppx.exe was not found. Install the Windows SDK packaging tools.' }
$makeAppxPath = if ($makeAppx -is [System.Management.Automation.CommandInfo]) { $makeAppx.Source } else { $makeAppx.FullName }
$msix = Join-Path $repo "dist/FileCommander-0.2.0-phase2-test-windows-$Architecture.msix"
& $makeAppxPath pack /d $stage /p $msix /o
if ($LASTEXITCODE) { throw 'MakeAppx packaging failed.' }

if ($CertificatePath) {
    $signTool = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if (-not $signTool) { throw 'signtool.exe was not found. Install the Windows SDK signing tools.' }
    $arguments = @('sign', '/fd', 'SHA256', '/f', $CertificatePath)
    if ($CertificatePassword) { $arguments += @('/p', $CertificatePassword) }
    $arguments += $msix
    & $signTool.Source @arguments
    if ($LASTEXITCODE) { throw 'MSIX signing failed.' }
} else {
    Write-Warning 'Created an unsigned MSIX. Pass -CertificatePath to create an installable signed package.'
}
Write-Host "Created $msix"
