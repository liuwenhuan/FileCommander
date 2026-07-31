[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [string]$Profile,
    [string]$Manifest,
    [string]$PreviousManifest,
    [switch]$AcceptSizeChange,
    [switch]$SkipSmoke,
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $Stage).Path
$repo = Split-Path -Parent $PSScriptRoot
$pendingBaselinePath = $null
[byte[]]$pendingBaselineBytes = $null

function Get-PeArchitecture {
    param(
        [Parameter(Mandatory)][string]$Path,
        [string]$DisplayPath = $Path
    )

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $DisplayPath" }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Not a PE file: $DisplayPath" }
        switch ($reader.ReadUInt16()) {
            0x8664 { return 'x64' }
            0x014C { return 'x86' }
            0xAA64 { return 'arm64' }
            default { throw "Unsupported PE architecture in $DisplayPath" }
        }
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-PeSubsystem {
    param(
        [Parameter(Mandatory)][string]$Path,
        [string]$DisplayPath = $Path
    )

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $DisplayPath" }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Not a PE file: $DisplayPath" }
        $stream.Position = $peOffset + 24
        $magic = $reader.ReadUInt16()
        if ($magic -notin @(0x010B, 0x020B)) { throw "Unsupported PE optional header in $DisplayPath" }
        $stream.Position = $peOffset + 24 + 68
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-NormalizedRelativePath {
    param([Parameter(Mandatory)][string]$Path)

    $normalized = $Path.Replace('\', '/').TrimStart('/')
    if ([string]::IsNullOrWhiteSpace($normalized) -or
        [System.IO.Path]::IsPathRooted($Path) -or
        $normalized.Split('/') -contains '..') {
        throw "Package manifest path must be stage-relative: $Path"
    }
    return $normalized
}

function Get-StageRelativePath {
    param([Parameter(Mandatory)][string]$Path)

    if (-not $Path.StartsWith($resolved + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Package file is outside the stage: $Path"
    }
    return $Path.Substring($resolved.Length + 1).Replace('\', '/')
}

function Get-ManifestFileMap {
    param(
        [Parameter(Mandatory)]$ReleaseManifest,
        [Parameter(Mandatory)][string]$ManifestLabel
    )

    if ($null -eq $ReleaseManifest.files) {
        throw "$ManifestLabel does not declare package files."
    }
    $filesByPath = @{}
    foreach ($file in @($ReleaseManifest.files)) {
        if ($null -eq $file -or [string]::IsNullOrWhiteSpace([string]$file.path)) {
            throw "$ManifestLabel contains a package file without a path."
        }
        $path = Get-NormalizedRelativePath -Path ([string]$file.path)
        if ($filesByPath.ContainsKey($path)) {
            throw "$ManifestLabel declares package file more than once: $path"
        }
        if ($file.bytes -lt 0) {
            throw "$ManifestLabel declares a negative size for $path."
        }
        $filesByPath[$path] = $file
    }
    return $filesByPath
}

function Get-ProfilePath {
    param([string]$RequestedProfile, $ReleaseManifest)

    $profilePath = $RequestedProfile
    if (-not $profilePath) {
        if (-not $ReleaseManifest.profile) { throw 'Release manifest does not declare a package profile.' }
        $profilePath = Join-Path $PSScriptRoot "profiles/$($ReleaseManifest.profile).json"
    } elseif (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
        $profilePath = Join-Path $PSScriptRoot "profiles/$RequestedProfile.json"
    }
    if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
        throw "Windows package profile was not found: $profilePath"
    }
    return (Resolve-Path -LiteralPath $profilePath).Path
}

function Set-BaselineAtomically {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][byte[]]$Bytes
    )

    $directory = Split-Path -Parent $Path
    $temporaryPath = Join-Path $directory ('.' + [System.IO.Path]::GetFileName($Path) + '.' + [guid]::NewGuid() + '.tmp')
    $backupPath = Join-Path $directory ('.' + [System.IO.Path]::GetFileName($Path) + '.' + [guid]::NewGuid() + '.bak')
    try {
        [System.IO.File]::WriteAllBytes($temporaryPath, $Bytes)
        [System.IO.File]::Replace($temporaryPath, $Path, $backupPath)
    } finally {
        Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
    }
}

$releaseManifestPath = if ($Manifest) {
    $Manifest
} else {
    Join-Path $resolved 'release-manifest.json'
}
$hasReleaseManifest = Test-Path -LiteralPath $releaseManifestPath -PathType Leaf
if (($Manifest -or $Profile -or $PreviousManifest -or $AcceptSizeChange) -and -not $hasReleaseManifest) {
    throw "Release manifest was not found: $releaseManifestPath"
}
if ($hasReleaseManifest) {
    $releaseManifestPath = (Resolve-Path -LiteralPath $releaseManifestPath).Path
    $releaseManifest = Get-Content -LiteralPath $releaseManifestPath -Raw | ConvertFrom-Json
    $profilePath = Get-ProfilePath -RequestedProfile $Profile -ReleaseManifest $releaseManifest
    $packageProfile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
    if ($releaseManifest.profile -ne $packageProfile.profile) {
        throw "Release manifest profile $($releaseManifest.profile) does not match selected profile $($packageProfile.profile)."
    }
    if ($releaseManifest.architecture -and $releaseManifest.architecture -ne $Architecture) {
        throw "Release manifest architecture $($releaseManifest.architecture) does not match expected $Architecture."
    }
    if ($releaseManifest.buildType -and $releaseManifest.buildType -ne 'Release') {
        throw "Release manifest build type must be Release, found $($releaseManifest.buildType)."
    }

    $groupNames = @('application', 'qt', 'platformPlugins', 'imagePlugins', 'network', 'pdf', 'media', 'office', 'msvcRuntime')
    foreach ($groupName in $groupNames) {
        if ($null -eq $packageProfile.groups.$groupName) {
            throw "Package profile $($packageProfile.profile) does not define group $groupName."
        }
    }
    $releaseFilesByPath = Get-ManifestFileMap -ReleaseManifest $releaseManifest -ManifestLabel 'Release manifest'
    foreach ($file in @($releaseManifest.files)) {
        $relativePath = Get-NormalizedRelativePath -Path ([string]$file.path)
        if ($groupNames -notcontains [string]$file.provenance) {
            throw "Release manifest declares an unknown provenance group for $relativePath."
        }
    }

    $manifestStagePath = $null
    if ($releaseManifestPath.StartsWith($resolved + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        $manifestStagePath = Get-StageRelativePath -Path $releaseManifestPath
    }
    $stageFilesByPath = @{}
    $stageFiles = @(Get-ChildItem -LiteralPath $resolved -Recurse -File | ForEach-Object {
        $relativePath = Get-StageRelativePath -Path $_.FullName
        if ($relativePath -ne $manifestStagePath) {
            $stageFilesByPath[$relativePath] = $_
            $_
        }
    })
    foreach ($relativePath in $stageFilesByPath.Keys) {
        if (-not $releaseFilesByPath.ContainsKey($relativePath)) {
            throw "Undeclared package file: $relativePath"
        }
    }
    foreach ($relativePath in $releaseFilesByPath.Keys) {
        if (-not $stageFilesByPath.ContainsKey($relativePath)) {
            throw "Manifest-declared package file is missing: $relativePath"
        }
        $actual = $stageFilesByPath[$relativePath]
        $recorded = $releaseFilesByPath[$relativePath]
        if ([int64]$actual.Length -ne [int64]$recorded.bytes) {
            throw "Package file size differs from release manifest: $relativePath"
        }
        if ([string]$recorded.sha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
            (Get-FileHash -LiteralPath $actual.FullName -Algorithm SHA256).Hash -ne [string]$recorded.sha256) {
            throw "Package file hash differs from release manifest: $relativePath"
        }
    }
    foreach ($groupName in $groupNames) {
        $group = $packageProfile.groups.$groupName
        foreach ($required in @($group.required)) {
            $relativePath = Get-NormalizedRelativePath -Path ([string]$required)
            if (-not $releaseFilesByPath.ContainsKey($relativePath)) {
                throw "Profile $($packageProfile.profile) requires package file: $relativePath"
            }
        }
        foreach ($forbidden in @($group.forbidden)) {
            $relativePath = Get-NormalizedRelativePath -Path ([string]$forbidden)
            if ($releaseFilesByPath.ContainsKey($relativePath)) {
                throw "Profile $($packageProfile.profile) forbids package file: $relativePath"
            }
        }
    }
    foreach ($file in $stageFiles) {
        $relativePath = Get-StageRelativePath -Path $file.FullName
        if ($file.Extension -ieq '.pdb') {
            throw "Debug symbol file is not allowed in a release package: $relativePath"
        }
        if ($file.Name -match '^(Qt5.*d|gtestd|gtest_maind|zd|archived|concrt.*d|msvcp.*d|vccorlib.*d|vcruntime.*d)\.dll$') {
            throw "Debug DLL is not allowed in a release package: $relativePath"
        }
        if ($file.Name -match '^(ffmpeg|ffprobe)\.exe$|^(avcodec|avformat|avutil|swscale|swresample|avfilter|avdevice)-?\d*\.dll$') {
            throw "Windows packages must not include FFmpeg runtime files: $relativePath"
        }
        if ($file.Extension -in @('.exe', '.dll')) {
            $actualArchitecture = Get-PeArchitecture -Path $file.FullName -DisplayPath $relativePath
            if ($actualArchitecture -ne $Architecture) {
                throw "Package PE architecture mismatch: $relativePath is $actualArchitecture, expected $Architecture."
            }
        }
    }
    if ($PreviousManifest) {
        if (-not (Test-Path -LiteralPath $PreviousManifest -PathType Leaf)) {
            throw "Previous release manifest was not found: $PreviousManifest"
        }
        $previousManifestPath = (Resolve-Path -LiteralPath $PreviousManifest).Path
        $previousReleaseManifest = Get-Content -LiteralPath $previousManifestPath -Raw | ConvertFrom-Json
        if ($previousReleaseManifest.profile -ne $releaseManifest.profile) {
            throw "Previous release manifest profile $($previousReleaseManifest.profile) does not match $($releaseManifest.profile)."
        }
        $previousFilesByPath = Get-ManifestFileMap -ReleaseManifest $previousReleaseManifest -ManifestLabel 'Previous release manifest'
        $sizeChanges = @()
        foreach ($relativePath in $releaseFilesByPath.Keys) {
            if ($previousFilesByPath.ContainsKey($relativePath)) {
                $growth = [int64]$releaseFilesByPath[$relativePath].bytes - [int64]$previousFilesByPath[$relativePath].bytes
                if ($growth -gt 20MB) {
                    $sizeChanges += "Package file grew by more than 20 MiB: $relativePath"
                }
            }
        }
        $currentTotal = [int64](($releaseFilesByPath.Values | Measure-Object -Property bytes -Sum).Sum)
        $previousTotal = [int64](($previousFilesByPath.Values | Measure-Object -Property bytes -Sum).Sum)
        if ($previousTotal -gt 0 -and $currentTotal -gt [int64][Math]::Floor($previousTotal * 1.15)) {
            $sizeChanges += "Release manifest total package size grew by more than 15%: $previousTotal bytes to $currentTotal bytes."
        }
        if ($sizeChanges.Count -gt 0 -and -not $AcceptSizeChange) {
            throw ($sizeChanges -join "`n")
        }
        if ($AcceptSizeChange) {
            $pendingBaselinePath = $previousManifestPath
            $pendingBaselineBytes = [System.IO.File]::ReadAllBytes($releaseManifestPath)
        }
    } elseif ($AcceptSizeChange) {
        throw 'AcceptSizeChange requires PreviousManifest so the verified baseline can be recorded.'
    }
}

$installer = Get-ChildItem -LiteralPath $resolved -Recurse -File -Filter 'vc_redist*.exe' |
    Select-Object -First 1
if ($installer) {
    $relativeInstaller = $installer.FullName.Substring($resolved.TrimEnd('\').Length + 1)
    throw "$relativeInstaller does not satisfy app-local MSVC runtime dependencies. Deploy the CRT DLLs beside FileCommander.exe instead."
}
$runtimeFiles = @(Get-ChildItem -LiteralPath $resolved -File |
    Where-Object { $_.Name -match '^(concrt|msvcp|vccorlib|vcruntime)140.*\.dll$' })
if ($runtimeFiles | Where-Object { $_.Name -match 'd\.dll$' }) {
    throw 'Debug MSVC runtime detected in release package.'
}
$requiredRuntimeNames = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')
foreach ($runtime in $requiredRuntimeNames) {
    $runtimePath = Join-Path $resolved $runtime
    if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
        throw "Missing app-local MSVC runtime dependency: $runtime"
    }
}

$executablePath = Join-Path $resolved 'FileCommander.exe'
$manifestPath = Join-Path $resolved 'manifest.json'
foreach ($required in @('FileCommander.exe', 'manifest.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $resolved $required) -PathType Leaf)) {
        throw "Missing package file: $required"
    }
}
$legacyManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$mediaBackend = [string]$legacyManifest.mediaBackend
if ([string]::IsNullOrWhiteSpace($mediaBackend)) {
    $mediaBackend = if ($legacyManifest.mediaPreview) { 'mpv' } else { 'none' }
}
$mediaBackend = $mediaBackend.ToLowerInvariant()
if ($mediaBackend -notin @('mpv', 'windowsmf', 'none')) {
    throw "Unsupported media backend in manifest: $mediaBackend"
}
if ($legacyManifest.runtime.provenance -ne 'msvc-runtime') {
    throw "MSVC runtime provenance must be msvc-runtime, found '$($legacyManifest.runtime.provenance)'."
}
$declaredRuntimeNames = @($legacyManifest.runtime.files)
if ($declaredRuntimeNames.Count -eq 0) {
    throw 'MSVC runtime manifest must declare collected CRT files.'
}
foreach ($requiredRuntimeName in $requiredRuntimeNames) {
    if ($declaredRuntimeNames -notcontains $requiredRuntimeName) {
        throw "MSVC runtime manifest is missing required declaration: $requiredRuntimeName"
    }
}
foreach ($declaredRuntimeName in $declaredRuntimeNames) {
    if ($declaredRuntimeName -isnot [string] -or
        [string]::IsNullOrWhiteSpace($declaredRuntimeName) -or
        [System.IO.Path]::GetFileName($declaredRuntimeName) -ne $declaredRuntimeName -or
        $declaredRuntimeName -notmatch '^(concrt140|msvcp140(?:_[A-Za-z0-9_]+)?|vccorlib140|vcruntime140(?:_[A-Za-z0-9_]+)?)\.dll$') {
        throw "Manifest-declared MSVC runtime is not a release VC143 CRT DLL: $declaredRuntimeName"
    }
    $declaredRuntimePath = Join-Path $resolved $declaredRuntimeName
    if (-not (Test-Path -LiteralPath $declaredRuntimePath -PathType Leaf)) {
        throw "Manifest-declared MSVC runtime is missing: $declaredRuntimeName"
    }
    $actualArchitecture = Get-PeArchitecture -Path $declaredRuntimePath
    if ($actualArchitecture -ne $Architecture) {
        throw "MSVC runtime architecture mismatch: $declaredRuntimeName is $actualArchitecture, expected $Architecture."
    }
}
$executableArchitecture = Get-PeArchitecture -Path $executablePath
if ($executableArchitecture -ne $Architecture) {
    throw "Package executable architecture mismatch: FileCommander.exe is $executableArchitecture, expected $Architecture."
}

foreach ($required in @('Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'platforms/qwindows.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $resolved $required))) {
        throw "Missing package file: $required"
    }
}
$executableSubsystem = Get-PeSubsystem -Path $executablePath -DisplayPath 'FileCommander.exe'
if ($executableSubsystem -ne 2) {
    throw "FileCommander.exe must use the Windows GUI subsystem; found subsystem value $executableSubsystem."
}
if ($legacyManifest.officePreview -and
    -not (Test-Path -LiteralPath (Join-Path $resolved 'office-oxide.exe'))) {
    throw 'Office preview is enabled but office-oxide.exe is missing.'
}
if ($legacyManifest.pdfPreview) {
    foreach ($required in @('Qt5Xml.dll', 'poppler-qt5.dll', 'poppler.dll', 'freetype.dll',
                            'openjp2.dll', 'libpng16.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $resolved $required))) {
            throw "PDF preview runtime is missing: $required"
        }
    }
}
if ($legacyManifest.mediaPreview -and $mediaBackend -eq 'mpv' -and
    -not (Test-Path -LiteralPath (Join-Path $resolved 'libmpv-2.dll'))) {
    throw 'mpv media preview is enabled but libmpv-2.dll is missing.'
}
if ($mediaBackend -eq 'windowsmf' -and
    (Test-Path -LiteralPath (Join-Path $resolved 'libmpv-2.dll'))) {
    throw 'Windows Media Foundation media preview must not package libmpv-2.dll.'
}
if (Get-ChildItem -LiteralPath $resolved -Recurse -Filter '*.dll' |
        Where-Object { $_.BaseName -match '^(Qt5.*d|gtestd|gtest_maind|zd|archived)$' }) {
    throw 'Debug DLL detected in release package.'
}

function Add-ZipText {
    param($Zip, [string]$Name, [string]$Text)
    $entry = $Zip.CreateEntry($Name)
    $writer = [System.IO.StreamWriter]::new($entry.Open(), [System.Text.UTF8Encoding]::new($false))
    try { $writer.Write($Text) } finally { $writer.Dispose() }
}

function New-PackageSmokeFixtures {
    param([string]$Root, $PackageManifest)
    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $Root 'smoke.png'),
        [System.Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL9OwAAAABJRU5ErkJggg=='))

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipSmoke = [System.IO.Compression.ZipFile]::Open((Join-Path $Root 'smoke.zip'),
        [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        Add-ZipText $zipSmoke 'hello.txt' 'FileCommander package archive smoke test'
    } finally { $zipSmoke.Dispose() }

    if ($PackageManifest.pdfPreview) {
        $encoding = [System.Text.Encoding]::ASCII
        $objects = @(
            "1 0 obj`n<< /Type /Catalog /Pages 2 0 R >>`nendobj`n",
            "2 0 obj`n<< /Type /Pages /Kids [3 0 R] /Count 1 >>`nendobj`n",
            "3 0 obj`n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << >> /Contents 4 0 R >>`nendobj`n",
            "4 0 obj`n<< /Length 0 >>`nstream`n`nendstream`nendobj`n"
        )
        $body = [System.Text.StringBuilder]::new("%PDF-1.4`n")
        $offsets = @()
        foreach ($object in $objects) {
            $offsets += $encoding.GetByteCount($body.ToString())
            [void]$body.Append($object)
        }
        $xref = $encoding.GetByteCount($body.ToString())
        [void]$body.Append("xref`n0 5`n0000000000 65535 f `n")
        foreach ($offset in $offsets) { [void]$body.AppendFormat('{0:D10} 00000 n {1}', $offset, "`n") }
        [void]$body.Append("trailer`n<< /Size 5 /Root 1 0 R >>`nstartxref`n$xref`n%%EOF`n")
        [System.IO.File]::WriteAllText((Join-Path $Root 'smoke.pdf'), $body.ToString(), $encoding)
    }

    if ($PackageManifest.officePreview) {
        $archive = [System.IO.Compression.ZipFile]::Open((Join-Path $Root 'smoke.docx'),
            [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            Add-ZipText $archive '[Content_Types].xml' '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>'
            Add-ZipText $archive '_rels/.rels' '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'
            Add-ZipText $archive 'word/document.xml' '<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>FileCommander package smoke test</w:t></w:r></w:p><w:sectPr/></w:body></w:document>'
        } finally { $archive.Dispose() }
    }

    if ($PackageManifest.mediaPreview) {
        $stream = [System.IO.File]::Open((Join-Path $Root 'smoke.wav'), [System.IO.FileMode]::Create)
        $writer = [System.IO.BinaryWriter]::new($stream)
        try {
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
            $writer.Write([int]40); $writer.Write([System.Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
            $writer.Write([int]16); $writer.Write([int16]1); $writer.Write([int16]1)
            $writer.Write([int]8000); $writer.Write([int]16000); $writer.Write([int16]2); $writer.Write([int16]16)
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([int]4); $writer.Write([int]0)
        } finally { $writer.Dispose() }

        foreach ($candidate in @(
            (Join-Path $repo 'build/wmf-fixtures-local/video-h264.mp4'),
            (Join-Path $repo 'build/wmf-fixtures-local-av/video-h264.mp4')
        )) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                Copy-Item -LiteralPath $candidate -Destination (Join-Path $Root 'smoke.mp4') -Force
                break
            }
        }
    }
}

if (-not $SkipSmoke) {
    $fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-package-smoke-" + [guid]::NewGuid())
    New-PackageSmokeFixtures -Root $fixtureRoot -PackageManifest $legacyManifest
    $oldPlatform = $env:QT_QPA_PLATFORM
    $oldPath = $env:PATH
    Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    try {
        # Keep the smoke test honest: no Qt, vcpkg, Poppler, or media SDK
        # directory may satisfy a missing runtime dependency.
        $env:PATH = "$resolved;$env:SystemRoot\System32;$env:SystemRoot"
        $process = Start-Process -FilePath (Join-Path $resolved 'FileCommander.exe') `
            -ArgumentList @('--package-smoke', $fixtureRoot) -WorkingDirectory $resolved -PassThru -WindowStyle Hidden
        if (-not $process.WaitForExit(45000)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'Package preview smoke test timed out.'
        }
        if ($process.ExitCode -ne 0) {
            throw "Package preview smoke test failed with code $($process.ExitCode)."
        }
    } finally {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
        $env:QT_QPA_PLATFORM = $oldPlatform
        $env:PATH = $oldPath
    }
}
if ($pendingBaselinePath) {
    Set-BaselineAtomically -Path $pendingBaselinePath -Bytes $pendingBaselineBytes
}
Write-Host 'Windows portable package verification passed.'
