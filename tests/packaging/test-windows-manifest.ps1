$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$writer = Join-Path $repo 'packaging/write-windows-manifest.ps1'
$profiles = Join-Path $repo 'packaging/profiles'
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-windows-manifest-" + [guid]::NewGuid())
$stageA = Join-Path $root 'first-absolute-stage'
$stageB = Join-Path $root 'second-absolute-stage'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Matches {
    param([string]$Actual, [string]$Pattern, [string]$Message)
    if ($Actual -notmatch $Pattern) { throw $Message }
}

function Get-ProfileGroup {
    param($Profile, [string]$Name)
    return $Profile.groups.$Name
}

function New-Stage {
    param([string]$Stage)

    New-Item -ItemType Directory -Force -Path (Join-Path $Stage 'platforms') | Out-Null
    $contents = [ordered]@{
        'FileCommander.exe' = 'filecommander'
        'Qt5Core.dll' = 'qt-core'
        'Qt5Gui.dll' = 'qt-gui'
        'Qt5Widgets.dll' = 'qt-widgets'
        'Qt5Xml.dll' = 'qt-xml'
        'poppler-qt5.dll' = 'poppler-qt5'
        'poppler.dll' = 'poppler'
        'freetype.dll' = 'freetype'
        'openjp2.dll' = 'openjpeg'
        'libpng16.dll' = 'png'
        'vcruntime140.dll' = 'vcruntime'
        'vcruntime140_1.dll' = 'vcruntime-1'
        'msvcp140.dll' = 'msvcp'
        'platforms/qwindows.dll' = 'qwindows'
    }
    foreach ($entry in $contents.GetEnumerator()) {
        $path = Join-Path $Stage $entry.Key
        [System.IO.File]::WriteAllText($path, $entry.Value, [System.Text.UTF8Encoding]::new($false))
    }
}

function New-ProvenanceEntries {
    return @(
        [pscustomobject]@{ path = 'FileCommander.exe'; provenance = 'application' }
        [pscustomobject]@{ path = 'Qt5Core.dll'; provenance = 'qt' }
        [pscustomobject]@{ path = 'Qt5Gui.dll'; provenance = 'qt' }
        [pscustomobject]@{ path = 'Qt5Widgets.dll'; provenance = 'qt' }
        [pscustomobject]@{ path = 'Qt5Xml.dll'; provenance = 'qt' }
        [pscustomobject]@{ path = 'poppler-qt5.dll'; provenance = 'pdf' }
        [pscustomobject]@{ path = 'poppler.dll'; provenance = 'pdf' }
        [pscustomobject]@{ path = 'freetype.dll'; provenance = 'pdf' }
        [pscustomobject]@{ path = 'openjp2.dll'; provenance = 'pdf' }
        [pscustomobject]@{ path = 'libpng16.dll'; provenance = 'pdf' }
        [pscustomobject]@{ path = 'vcruntime140.dll'; provenance = 'msvcRuntime' }
        [pscustomobject]@{ path = 'vcruntime140_1.dll'; provenance = 'msvcRuntime' }
        [pscustomobject]@{ path = 'msvcp140.dll'; provenance = 'msvcRuntime' }
        [pscustomobject]@{ path = 'platforms/qwindows.dll'; provenance = 'platformPlugins' }
    )
}

try {
    New-Stage -Stage $stageA
    New-Stage -Stage $stageB
    $provenanceA = New-ProvenanceEntries
    $provenanceB = New-ProvenanceEntries
    $portableProfile = Join-Path $profiles 'windows-portable.json'

    & $writer -Stage $stageA -ProfilePath $portableProfile -Architecture x64 -BuildType Release -ProvenanceEntries $provenanceA
    & $writer -Stage $stageB -ProfilePath $portableProfile -Architecture x64 -BuildType Release -ProvenanceEntries $provenanceB

    $first = [System.IO.File]::ReadAllBytes((Join-Path $stageA 'release-manifest.json'))
    $second = [System.IO.File]::ReadAllBytes((Join-Path $stageB 'release-manifest.json'))
    Assert-True -Condition ([System.Linq.Enumerable]::SequenceEqual($first, $second)) `
        -Message 'Release manifests changed when only the absolute stage directory changed.'

    $manifestText = [System.Text.Encoding]::UTF8.GetString($first)
    Assert-True -Condition ($manifestText -notlike "*$stageA*") `
        -Message 'Release manifest leaked the first absolute stage directory.'
    Assert-True -Condition ($manifestText -notlike "*$stageB*") `
        -Message 'Release manifest leaked the second absolute stage directory.'
    Assert-True -Condition ($manifestText -notmatch '[A-Za-z]:[\\/]') `
        -Message 'Release manifest contained an absolute Windows source path.'

    $manifest = $manifestText | ConvertFrom-Json
    Assert-True -Condition ($manifest.profile -eq 'windows-portable') `
        -Message 'Release manifest did not record its profile.'
    Assert-True -Condition ($manifest.mediaBackend -eq 'windowsmf') `
        -Message 'Release manifest did not record the Windows Media Foundation backend.'
    Assert-True -Condition ($manifest.architecture -eq 'x64') `
        -Message 'Release manifest did not record its architecture.'
    Assert-True -Condition ($manifest.buildType -eq 'Release') `
        -Message 'Release manifest did not record its build type.'
    $paths = @($manifest.files | ForEach-Object { $_.path })
    Assert-True -Condition (($paths -join "`n") -ceq (($paths | Sort-Object) -join "`n")) `
        -Message 'Release manifest files were not sorted by normalized relative path.'
    Assert-True -Condition ($paths -contains 'platforms/qwindows.dll') `
        -Message 'Release manifest did not normalize a nested path with forward slashes.'
    Assert-True -Condition ($paths -notcontains 'libmpv-2.dll') `
        -Message 'Windows package manifest must not contain libmpv.'

    $portable = Get-Content -LiteralPath $portableProfile -Raw | ConvertFrom-Json
    $msix = Get-Content -LiteralPath (Join-Path $profiles 'windows-msix.json') -Raw | ConvertFrom-Json

    foreach ($profile in @($portable, $msix)) {
        Assert-True -Condition ($profile.features.mediaPreview -and $profile.features.mediaBackend -eq 'windowsmf') `
            -Message "$($profile.profile) must use the Windows Media Foundation media backend."
        Assert-True -Condition ((Get-ProfileGroup $profile 'media').forbidden -contains 'libmpv-2.dll') `
            -Message "$($profile.profile) must forbid libmpv-2.dll."
        Assert-True -Condition ((Get-ProfileGroup $profile 'pdf').required -contains 'poppler-qt5.dll') `
            -Message "$($profile.profile) must require Poppler."
        Assert-True -Condition ((Get-ProfileGroup $profile 'qt').required -contains 'Qt5Xml.dll') `
            -Message "$($profile.profile) must require Qt5Xml."
    }
    foreach ($legacyProfile in @('windows-full-portable.json', 'windows-lite-portable.json', 'windows-full-msix.json', 'windows-lite-msix.json')) {
        Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $profiles $legacyProfile))) `
            -Message "$legacyProfile must not exist after collapsing Windows packaging to one profile."
    }

    $portableBuilder = Get-Content -LiteralPath (Join-Path $repo 'packaging/build-windows.ps1') -Raw
    Assert-Matches -Actual $portableBuilder -Pattern "windows-portable" `
        -Message 'Portable builder must select the single Windows profile.'
    Assert-True -Condition ($portableBuilder -notmatch 'WithFullPreviews|windows-full|windows-lite|MpvRoot|FILECOMMANDER_MPV_ROOT') `
        -Message 'Portable builder must not carry legacy Full/Lite or mpv SDK policy.'
    Assert-True -Condition ($portableBuilder -notmatch 'Copy-StageFile[^\r\n]+libmpv') `
        -Message 'Portable builder must not copy libmpv.'
    Assert-Matches -Actual $portableBuilder -Pattern 'write-windows-manifest\.ps1' `
        -Message 'Portable builder must write a release manifest.'
    Assert-Matches -Actual $portableBuilder -Pattern 'beforeWindeploy' `
        -Message 'Portable builder must record windeployqt output by stage diff.'

    $msixBuilder = Get-Content -LiteralPath (Join-Path $repo 'packaging/build-windows-msix.ps1') -Raw
    Assert-Matches -Actual $msixBuilder -Pattern 'windows-portable' `
        -Message 'MSIX builder must build the single portable stage.'
    Assert-Matches -Actual $msixBuilder -Pattern 'windows-msix' `
        -Message 'MSIX builder must emit the single MSIX release manifest.'
    Assert-True -Condition ($msixBuilder -notmatch 'WithFullPreviews|windows-full|windows-lite|MpvRoot|FILECOMMANDER_MPV_ROOT') `
        -Message 'MSIX builder must not carry legacy Full/Lite or mpv SDK policy.'

    Write-Host 'Windows package profile and manifest tests passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
