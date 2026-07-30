$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$verifier = Join-Path $repo 'packaging/verify-windows-package.ps1'
$guiVerifier = Join-Path $repo 'tests/packaging/test-windows-gui-subsystem.ps1'
$profiles = Join-Path $repo 'packaging/profiles'
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-windows-allowlist-" + [guid]::NewGuid())

function Assert-True {
    param([bool]$Condition, [string]$Message)

    if (-not $Condition) { throw $Message }
}

function Assert-Matches {
    param([string]$Actual, [string]$Pattern, [string]$Message)

    if ($Actual -notmatch $Pattern) {
        throw "$Message Expected pattern '$Pattern', got: $Actual"
    }
}

function Assert-BytesEqual {
    param([byte[]]$Expected, [byte[]]$Actual, [string]$Message)

    if (-not [System.Linq.Enumerable]::SequenceEqual($Expected, $Actual)) {
        throw $Message
    }
}

function New-PeFile {
    param(
        [string]$Path,
        [UInt16]$Machine = 0x8664,
        [UInt16]$Subsystem = 3
    )

    [byte[]]$bytes = New-Object byte[] 512
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([UInt32]0x80).CopyTo($bytes, 0x3C)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes($Machine).CopyTo($bytes, 0x84)
    [BitConverter]::GetBytes([UInt16]0x20B).CopyTo($bytes, 0x98)
    [BitConverter]::GetBytes($Subsystem).CopyTo($bytes, 0xDC)
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

function Get-RelativePath {
    param([string]$Stage, [string]$Path)

    return $Path.Substring($Stage.TrimEnd('\').Length + 1).Replace('\', '/')
}

function Add-File {
    param(
        [string]$Stage,
        [string]$RelativePath,
        [hashtable]$Provenance,
        [string]$Group,
        [UInt16]$Machine = 0x8664,
        [UInt16]$Subsystem = 3
    )

    $path = Join-Path $Stage $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    if ($RelativePath -match '\.(exe|dll)$') {
        New-PeFile -Path $path -Machine $Machine -Subsystem $Subsystem
    } else {
        [System.IO.File]::WriteAllText($path, $RelativePath, [System.Text.UTF8Encoding]::new($false))
    }
    $Provenance[$RelativePath.Replace('\', '/')] = $Group
}

function Write-LegacyManifest {
    param([string]$Stage)

    [ordered]@{
        product = 'FileCommander'
        platform = 'windows-x64'
        officePreview = $false
        pdfPreview = $true
        mediaPreview = $true
        runtime = [ordered]@{
            provenance = 'msvc-runtime'
            files = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')
        }
    } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $Stage 'manifest.json') -Encoding UTF8
}

function Write-ReleaseManifest {
    param(
        [string]$Stage,
        [string]$Profile,
        [hashtable]$Provenance
    )

    $files = @(
        Get-ChildItem -LiteralPath $Stage -Recurse -File |
            Where-Object { $_.Name -ne 'release-manifest.json' } |
            ForEach-Object {
                $relativePath = Get-RelativePath -Stage $Stage -Path $_.FullName
                if (-not $Provenance.ContainsKey($relativePath)) {
                    throw "Fixture file lacks provenance: $relativePath"
                }
                [ordered]@{
                    path = $relativePath
                    bytes = [int64]$_.Length
                    sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
                    provenance = $Provenance[$relativePath]
                    version = ''
                }
            } |
            Sort-Object path
    )
    [ordered]@{
        profile = $Profile
        architecture = 'x64'
        buildType = 'Release'
        files = $files
    } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $Stage 'release-manifest.json') -Encoding UTF8
}

function New-ValidStage {
    param(
        [string]$Name,
        [string]$Profile = 'windows-full-portable'
    )

    $stage = Join-Path $root $Name
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    $provenance = @{}
    Add-File -Stage $stage -RelativePath 'FileCommander.exe' -Provenance $provenance -Group 'application' -Subsystem 2
    Add-File -Stage $stage -RelativePath 'Qt5Core.dll' -Provenance $provenance -Group 'qt'
    Add-File -Stage $stage -RelativePath 'Qt5Gui.dll' -Provenance $provenance -Group 'qt'
    Add-File -Stage $stage -RelativePath 'Qt5Widgets.dll' -Provenance $provenance -Group 'qt'
    Add-File -Stage $stage -RelativePath 'Qt5Xml.dll' -Provenance $provenance -Group 'qt'
    Add-File -Stage $stage -RelativePath 'platforms/qwindows.dll' -Provenance $provenance -Group 'platformPlugins'
    foreach ($name in @('poppler-qt5.dll', 'poppler.dll', 'freetype.dll', 'openjp2.dll', 'libpng16.dll')) {
        Add-File -Stage $stage -RelativePath $name -Provenance $provenance -Group 'pdf'
    }
    if ($Profile -eq 'windows-full-portable') {
        Add-File -Stage $stage -RelativePath 'libmpv-2.dll' -Provenance $provenance -Group 'media'
    }
    foreach ($name in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')) {
        Add-File -Stage $stage -RelativePath $name -Provenance $provenance -Group 'msvcRuntime'
    }
    Write-LegacyManifest -Stage $stage
    $provenance['manifest.json'] = 'application'
    Write-ReleaseManifest -Stage $stage -Profile $Profile -Provenance $provenance
    return [pscustomobject]@{ Stage = $stage; Provenance = $provenance }
}

function Invoke-ExpectedRejection {
    param(
        [string]$Stage,
        [string]$Profile,
        [string]$Pattern,
        [string]$PreviousManifest,
        [switch]$AcceptSizeChange
    )

    $arguments = @{
        Stage = $Stage
        Profile = $Profile
        Manifest = (Join-Path $Stage 'release-manifest.json')
    }
    if ($PreviousManifest) { $arguments.PreviousManifest = $PreviousManifest }
    if ($AcceptSizeChange) { $arguments.AcceptSizeChange = $true }
    try {
        & $verifier @arguments
        throw "Expected package verification to reject $Pattern."
    } catch {
        Assert-Matches -Actual $_.Exception.Message -Pattern $Pattern `
            -Message 'Package verification did not report the offending package entry.'
    }
}

function Write-SizedFile {
    param([string]$Path, [int64]$Bytes)

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try { $stream.SetLength($Bytes) } finally { $stream.Dispose() }
}

try {
    $fullProfile = Join-Path $profiles 'windows-full-portable.json'
    $liteProfile = Join-Path $profiles 'windows-lite-portable.json'

    foreach ($case in @(
        @{ name = 'probe'; path = 'probe.exe' },
        @{ name = 'object'; path = 'leftover.obj' },
        @{ name = 'test-executable'; path = 'ui_tests.exe' },
        @{ name = 'large-unknown'; path = 'undeclared-50MiB.bin' }
    )) {
        $fixture = New-ValidStage -Name $case.name
        $path = Join-Path $fixture.Stage $case.path
        if ($case.name -eq 'large-unknown') {
            Write-SizedFile -Path $path -Bytes (50MB)
        } elseif ($case.path -match '\.exe$') {
            New-PeFile -Path $path -Subsystem 3
        } else {
            [System.IO.File]::WriteAllText($path, 'build residue')
        }
        Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -Pattern ([regex]::Escape($case.path))
    }

    $fixture = New-ValidStage -Name 'accepted-size-change-still-unknown'
    $previous = Join-Path $root 'accepted-size-change-previous-manifest.json'
    Copy-Item -LiteralPath (Join-Path $fixture.Stage 'release-manifest.json') -Destination $previous
    New-PeFile -Path (Join-Path $fixture.Stage 'probe.exe') -Subsystem 3
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -PreviousManifest $previous `
        -AcceptSizeChange -Pattern 'probe\.exe'

    $fixture = New-ValidStage -Name 'debug-qt'
    Add-File -Stage $fixture.Stage -RelativePath 'Qt5Cored.dll' -Provenance $fixture.Provenance -Group 'qt'
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-full-portable' -Provenance $fixture.Provenance
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -Pattern 'Qt5Cored\.dll'

    $fixture = New-ValidStage -Name 'debug-symbols'
    Add-File -Stage $fixture.Stage -RelativePath 'debug.pdb' -Provenance $fixture.Provenance -Group 'application'
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-full-portable' -Provenance $fixture.Provenance
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -Pattern 'debug\.pdb'

    $fixture = New-ValidStage -Name 'wrong-architecture'
    Add-File -Stage $fixture.Stage -RelativePath 'plugins/wrong-arch.dll' -Provenance $fixture.Provenance -Group 'network' -Machine 0x014C
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-full-portable' -Provenance $fixture.Provenance
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -Pattern 'plugins/wrong-arch\.dll'

    $fixture = New-ValidStage -Name 'console-subsystem'
    New-PeFile -Path (Join-Path $fixture.Stage 'FileCommander.exe') -Subsystem 3
    $consoleBytes = [System.IO.File]::ReadAllBytes((Join-Path $fixture.Stage 'FileCommander.exe'))
    Assert-True -Condition ([BitConverter]::ToUInt16($consoleBytes, 0xDC) -eq 3) `
        -Message 'Console subsystem fixture did not write the PE subsystem field.'
    try {
        & $guiVerifier -Executable (Join-Path $fixture.Stage 'FileCommander.exe') -DisplayPath 'FileCommander.exe'
        throw 'GUI subsystem verifier accepted a console subsystem fixture.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message -Pattern 'FileCommander\.exe' `
            -Message 'GUI subsystem verifier did not identify the console executable.'
    }
    $legacyManifest = Get-Content -LiteralPath (Join-Path $fixture.Stage 'manifest.json') -Raw | ConvertFrom-Json
    Assert-True -Condition ($legacyManifest.runtime.provenance -eq 'msvc-runtime') `
        -Message 'Console subsystem fixture unexpectedly changed the legacy runtime provenance.'
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-full-portable' -Provenance $fixture.Provenance
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -Pattern 'FileCommander\.exe'

    $fixture = New-ValidStage -Name 'missing-profile-requirement'
    Remove-Item -LiteralPath (Join-Path $fixture.Stage 'libmpv-2.dll')
    $fixture.Provenance.Remove('libmpv-2.dll')
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-full-portable' -Provenance $fixture.Provenance
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -Pattern 'libmpv-2\.dll'

    $fixture = New-ValidStage -Name 'forbidden-profile-file' -Profile 'windows-lite-portable'
    Add-File -Stage $fixture.Stage -RelativePath 'libmpv-2.dll' -Provenance $fixture.Provenance -Group 'media'
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-lite-portable' -Provenance $fixture.Provenance
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $liteProfile -Pattern 'libmpv-2\.dll'

    $fixture = New-ValidStage -Name 'runtime-rejection-preserves-baseline'
    $previous = Join-Path $root 'runtime-rejection-previous-manifest.json'
    $previousManifest = Get-Content -LiteralPath (Join-Path $fixture.Stage 'release-manifest.json') -Raw | ConvertFrom-Json
    foreach ($file in $previousManifest.files) { $file.bytes = 1 }
    $previousManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $previous -Encoding UTF8
    $expectedBaselineBytes = [System.IO.File]::ReadAllBytes($previous)
    $legacyManifest = Get-Content -LiteralPath (Join-Path $fixture.Stage 'manifest.json') -Raw | ConvertFrom-Json
    $legacyManifest.runtime.provenance = 'invalid'
    $legacyManifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $fixture.Stage 'manifest.json') -Encoding UTF8
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-full-portable' -Provenance $fixture.Provenance
    try {
        & $verifier -Stage $fixture.Stage -Profile $fullProfile `
            -Manifest (Join-Path $fixture.Stage 'release-manifest.json') `
            -PreviousManifest $previous -AcceptSizeChange -SkipSmoke
        throw 'Expected invalid runtime provenance to reject the package.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message -Pattern 'MSVC runtime provenance must be msvc-runtime' `
            -Message 'Runtime-manifest fixture did not reach the retained runtime validation.'
    }
    Assert-BytesEqual -Expected $expectedBaselineBytes -Actual ([System.IO.File]::ReadAllBytes($previous)) `
        -Message 'AcceptSizeChange modified the previous baseline before runtime validation succeeded.'

    $fixture = New-ValidStage -Name 'per-file-size-growth'
    Add-File -Stage $fixture.Stage -RelativePath 'network/known-large.bin' -Provenance $fixture.Provenance -Group 'network'
    Write-SizedFile -Path (Join-Path $fixture.Stage 'network/known-large.bin') -Bytes (21MB)
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-full-portable' -Provenance $fixture.Provenance
    $previous = Join-Path $root 'per-file-previous-manifest.json'
    $previousManifest = Get-Content -LiteralPath (Join-Path $fixture.Stage 'release-manifest.json') -Raw | ConvertFrom-Json
    ($previousManifest.files | Where-Object { $_.path -eq 'network/known-large.bin' }).bytes = 0
    $previousManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $previous -Encoding UTF8
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -PreviousManifest $previous -Pattern 'network/known-large\.bin'

    $fixture = New-ValidStage -Name 'total-size-growth'
    foreach ($name in @('network/one.bin', 'network/two.bin', 'network/three.bin')) {
        Add-File -Stage $fixture.Stage -RelativePath $name -Provenance $fixture.Provenance -Group 'network'
        Write-SizedFile -Path (Join-Path $fixture.Stage $name) -Bytes (4MB)
    }
    Write-ReleaseManifest -Stage $fixture.Stage -Profile 'windows-full-portable' -Provenance $fixture.Provenance
    $previous = Join-Path $root 'total-size-previous-manifest.json'
    $previousManifest = Get-Content -LiteralPath (Join-Path $fixture.Stage 'release-manifest.json') -Raw | ConvertFrom-Json
    foreach ($file in $previousManifest.files) { $file.bytes = 1 }
    $previousManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $previous -Encoding UTF8
    Invoke-ExpectedRejection -Stage $fixture.Stage -Profile $fullProfile -PreviousManifest $previous -Pattern 'total package size'
    & $verifier -Stage $fixture.Stage -Profile $fullProfile -Manifest (Join-Path $fixture.Stage 'release-manifest.json') `
        -PreviousManifest $previous -AcceptSizeChange -SkipSmoke
    Assert-BytesEqual -Expected ([System.IO.File]::ReadAllBytes((Join-Path $fixture.Stage 'release-manifest.json'))) `
        -Actual ([System.IO.File]::ReadAllBytes($previous)) `
        -Message 'AcceptSizeChange did not atomically record the verified release manifest as the new baseline.'

    Write-Host 'Windows package allowlist tests passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
