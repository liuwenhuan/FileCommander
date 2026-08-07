[CmdletBinding()]
param(
    [string]$QtRoot = $env:FILECOMMANDER_QT_ROOT,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$PopplerQt5Root = $env:FILECOMMANDER_POPPLER_QT5_ROOT,
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Architecture = 'x64',
    [ValidateSet('windows-portable')]
    [string]$Profile = 'windows-portable',
    [switch]$SkipArchive
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# Single source of truth for the version: the project() line, exactly as
# packaging/build-deb.sh reads it. Keeping this derived rather than hardcoded
# stops the package version from drifting away from the TTC_VERSION compiled
# into the binary -- which is the version the update checker compares against,
# so a drift means the server advertises one number and the client believes
# another.
$cmakeLists = Join-Path $repo 'CMakeLists.txt'
$versionMatch = Select-String -LiteralPath $cmakeLists -Pattern '^project\(FileCommander VERSION ([0-9]+(?:\.[0-9]+)*)' |
    Select-Object -First 1
if (-not $versionMatch) { throw "Could not read the version from the project() line in $cmakeLists" }
$script:ProductVersion = $versionMatch.Matches[0].Groups[1].Value

$profilePath = Join-Path $PSScriptRoot "profiles/$Profile.json"
if (-not (Test-Path -LiteralPath $profilePath)) { throw "Windows package profile not found: $profilePath" }
$packageProfile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
if ($packageProfile.packageType -ne 'portable') { throw "Profile $Profile is not a portable package profile." }
$pdfPreview = [bool]$packageProfile.features.pdfPreview
$mediaPreview = [bool]$packageProfile.features.mediaPreview
$mediaBackend = [string]$packageProfile.features.mediaBackend
if ([string]::IsNullOrWhiteSpace($mediaBackend)) {
    $mediaBackend = if ($mediaPreview) { 'windowsmf' } else { 'none' }
}
$mediaBackend = $mediaBackend.ToLowerInvariant()
if ($mediaBackend -notin @('windowsmf', 'none')) {
    throw "Windows package profile $Profile must use mediaBackend windowsmf or none."
}
if ($mediaBackend -eq 'none') { $mediaPreview = $false }

if (-not $QtRoot -or -not (Test-Path -LiteralPath $QtRoot)) {
    throw 'Set FILECOMMANDER_QT_ROOT or pass -QtRoot with a Qt 5.15 MSVC installation.'
}
if (-not $VcpkgRoot -or -not (Test-Path -LiteralPath $VcpkgRoot)) {
    throw 'Set VCPKG_ROOT or pass -VcpkgRoot.'
}
if (-not $PopplerQt5Root) {
    $PopplerQt5Root = Join-Path (Split-Path -Parent $VcpkgRoot) 'poppler-qt5'
}
$triplet = "$Architecture-windows"
$build = Join-Path $repo "build/windows-msvc-release-$Architecture"
$stage = Join-Path $repo "dist/FileCommander-windows-$Architecture"
$pdfPreviewOption = if ($pdfPreview) { 'ON' } else { 'OFF' }
$mediaPreviewOption = if ($mediaPreview) { 'ON' } else { 'OFF' }
$provenanceEntries = [System.Collections.Generic.List[object]]::new()

function Get-StageRelativePath {
    param([Parameter(Mandatory)][string]$Path)

    $resolvedStage = (Resolve-Path -LiteralPath $stage).Path.TrimEnd('\')
    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    if (-not $resolvedPath.StartsWith($resolvedStage + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Package file is outside the stage: $resolvedPath"
    }
    return $resolvedPath.Substring($resolvedStage.Length + 1).Replace('\', '/')
}

function Add-StageProvenance {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Group)

    [void]$provenanceEntries.Add([pscustomobject]@{
        path = Get-StageRelativePath -Path $Path
        provenance = $Group
    })
}

function Copy-StageFile {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Group
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Package source file not found: $Source" }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $destinationPath = if (Test-Path -LiteralPath $Destination -PathType Container) {
        Join-Path $Destination (Split-Path -Leaf $Source)
    } else {
        $Destination
    }
    Add-StageProvenance -Path $destinationPath -Group $Group
}

function Get-RuntimeProvenanceGroup {
    param([Parameter(Mandatory)][string]$Name)

    if ($Name -like 'Qt5*.dll') { return 'qt' }
    if ($Name -in @('poppler-qt5.dll', 'poppler.dll', 'freetype.dll', 'openjp2.dll', 'libpng16.dll',
                     'jpeg62.dll', 'tiff.dll', 'brotlidec.dll', 'brotlicommon.dll')) { return 'pdf' }
    return 'network'
}

function Get-WindeployProvenanceGroup {
    param([Parameter(Mandatory)][string]$RelativePath)

    if ($RelativePath.StartsWith('platforms/', [System.StringComparison]::OrdinalIgnoreCase)) { return 'platformPlugins' }
    if ($RelativePath.StartsWith('imageformats/', [System.StringComparison]::OrdinalIgnoreCase)) { return 'imagePlugins' }
    return 'qt'
}

$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repo 'build'))
$buildFullPath = [System.IO.Path]::GetFullPath($build)
if (-not $buildFullPath.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean a release build directory outside the repository build root: $buildFullPath"
}
if (Test-Path -LiteralPath $build) {
    Remove-Item -LiteralPath $build -Recurse -Force
}

# Ninja learns MSVC header dependencies by parsing /showIncludes. A localized
# prefix can be mis-decoded by CMake and leave every object with zero recorded
# dependencies, allowing stale class layouts to be linked after a header edit.
$previousVsLang = $env:VSLANG
$env:VSLANG = '1033'
try {
    cmake -S $repo -B $build -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_PREFIX_PATH=$QtRoot" `
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake" `
        "-DVCPKG_TARGET_TRIPLET=$triplet" `
        -DTTC_BUILD_TESTS=OFF `
        -DTTC_BUILD_BENCH=OFF `
        -DFILECOMMANDER_ENABLE_NETWORK=ON `
        "-DFILECOMMANDER_POPPLER_QT5_ROOT=$PopplerQt5Root" `
        "-DFILECOMMANDER_PREVIEW_PDF=$pdfPreviewOption" `
        "-DFILECOMMANDER_PREVIEW_MEDIA=$mediaPreviewOption" `
        "-DFILECOMMANDER_MEDIA_BACKEND=$mediaBackend"
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    cmake --build $build --parallel
    if ($LASTEXITCODE) { throw 'Build failed.' }
} finally {
    if ($null -eq $previousVsLang) {
        Remove-Item Env:VSLANG -ErrorAction SilentlyContinue
    } else {
        $env:VSLANG = $previousVsLang
    }
}

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-StageFile -Source (Join-Path $build 'FileCommander.exe') -Destination $stage -Group 'application'
# GPL-3 section 4: whoever receives the program receives the terms with it. A
# portable zip is often the only thing a user ever sees of this project, so the
# licence ships inside it -- and is listed in the profile's required files, so
# a package built without it fails verification instead of shipping.
Copy-StageFile -Source (Join-Path $repo 'LICENSE') -Destination $stage -Group 'application'

$beforeWindeploy = @{}
Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object { $beforeWindeploy[(Get-StageRelativePath -Path $_.FullName)] = $true }
& (Join-Path $QtRoot 'bin/windeployqt.exe') --release --no-translations (Join-Path $stage 'FileCommander.exe')
if ($LASTEXITCODE) { throw 'windeployqt failed.' }
Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    $relativePath = Get-StageRelativePath -Path $_.FullName
    if (-not $beforeWindeploy.ContainsKey($relativePath)) {
        Add-StageProvenance -Path $_.FullName -Group (Get-WindeployProvenanceGroup -RelativePath $relativePath)
    }
}

# CMake's runtime-dependency step has already copied the exact vcpkg DLL
# closure beside the executable. Copying every DLL from vcpkg also pulled test
# and compatibility runtimes into the package and could change DLL resolution.
Get-ChildItem -LiteralPath $build -File -Filter '*.dll' | ForEach-Object {
    $destination = Join-Path $stage $_.Name
    if (-not (Test-Path -LiteralPath $destination)) {
        Copy-StageFile -Source $_.FullName -Destination $stage -Group (Get-RuntimeProvenanceGroup -Name $_.Name)
    }
}

if ($pdfPreview) {
    Copy-StageFile -Source (Join-Path $QtRoot 'bin/Qt5Xml.dll') -Destination $stage -Group 'qt'
    if (-not (Test-Path -LiteralPath (Join-Path $PopplerQt5Root 'bin/poppler-qt5.dll'))) {
        throw "Poppler Qt5 runtime not found at $PopplerQt5Root."
    }
    Get-ChildItem -LiteralPath (Join-Path $PopplerQt5Root 'bin') -Filter '*.dll' | ForEach-Object {
        Copy-StageFile -Source $_.FullName -Destination $stage -Group 'pdf'
    }
    # No second source. The loop above takes the whole of poppler's bin, and
    # build-poppler-qt5.ps1 vendors poppler's vcpkg runtime DLLs into that same
    # directory, resolved from the real import tables.
    #
    # There used to be a hand-written list of vcpkg DLLs here, and it was wrong
    # in both directions at once: it demanded tiff.dll, which nothing installs
    # and poppler is built without, and it never mentioned z.dll, which poppler
    # does import. Both survived because a developer vcpkg has those ports
    # pulled in by something else.
    #
    # It could not have been made right by editing the names, either. The
    # prefix is cached between CI runs, and a cache hit skips that script
    # entirely -- vcpkg install included -- so on the run after a hit there is
    # no vcpkg bin to copy from at all.
}
if (Test-Path -LiteralPath (Join-Path $stage 'libmpv-2.dll')) {
    throw "Profile $Profile forbids libmpv-2.dll."
}

$officeBinary = Join-Path $repo 'build/office-oxide/office-oxide.exe'
if (Test-Path -LiteralPath $officeBinary) {
    Copy-StageFile -Source $officeBinary -Destination $stage -Group 'office'
}
$runtime = & (Join-Path $PSScriptRoot 'collect-msvc-runtime.ps1') `
    -Stage $stage -Architecture $Architecture -Mode Portable
if (-not $runtime.CopiedCrtDllPaths) { throw 'MSVC runtime collection did not copy any CRT DLLs.' }
foreach ($runtimePath in $runtime.CopiedCrtDllPaths) {
    Add-StageProvenance -Path $runtimePath -Group 'msvcRuntime'
}

# The installer is a prerequisite for bootstrapper-style installers, never a
# portable app-local dependency. Keep the direct result directory runnable.
Get-ChildItem -LiteralPath $stage -Recurse -File -Filter 'vc_redist*.exe' |
    Remove-Item -Force

$manifest = [ordered]@{
    product = 'FileCommander'
    version = $script:ProductVersion
    platform = "windows-$Architecture"
    networkProtocols = @('sftp', 'smb', 'ftp', 'webdav', 'webdavs')
    officePreview = Test-Path -LiteralPath (Join-Path $stage 'office-oxide.exe')
    pdfPreview = $pdfPreview
    mediaPreview = $mediaPreview
    mediaBackend = $mediaBackend
    runtime = [ordered]@{
        provenance = $runtime.Provenance
        files = @($runtime.CopiedCrtDllPaths | ForEach-Object { Split-Path -Leaf $_ })
    }
}
$legacyManifestPath = Join-Path $stage 'manifest.json'
$manifest | ConvertTo-Json | Set-Content -LiteralPath $legacyManifestPath -Encoding UTF8
Add-StageProvenance -Path $legacyManifestPath -Group 'application'

& (Join-Path $PSScriptRoot 'write-windows-manifest.ps1') -Stage $stage -ProfilePath $profilePath `
    -Architecture $Architecture -BuildType Release -ProvenanceEntries $provenanceEntries.ToArray()

& (Join-Path $PSScriptRoot 'verify-windows-package.ps1') -Stage $stage -Architecture $Architecture
if ($LASTEXITCODE) { throw 'Package verification failed.' }
if ($SkipArchive) {
    Write-Host "Prepared runnable directory $stage"
} else {
    $zip = Join-Path $repo "dist/FileCommander-$script:ProductVersion-windows-$Architecture.zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    Write-Host "Created $zip"
}
