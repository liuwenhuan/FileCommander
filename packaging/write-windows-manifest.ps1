[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$ProfilePath,
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Architecture = 'x64',
    [string]$BuildType = 'Release',
    [object[]]$ProvenanceEntries = @()
)

$ErrorActionPreference = 'Stop'
$stageRoot = (Resolve-Path -LiteralPath $Stage).Path.TrimEnd('\')
$profile = Get-Content -LiteralPath $ProfilePath -Raw | ConvertFrom-Json
$groupNames = @('application', 'qt', 'platformPlugins', 'imagePlugins', 'network', 'pdf', 'media', 'office', 'msvcRuntime')

if ([string]::IsNullOrWhiteSpace($profile.profile)) {
    throw "Package profile $ProfilePath does not define a profile name."
}
foreach ($groupName in $groupNames) {
    if ($null -eq $profile.groups.$groupName) {
        throw "Package profile $($profile.profile) does not define group $groupName."
    }
}

function Get-NormalizedRelativePath {
    param([Parameter(Mandatory)][string]$Path)

    $normalized = $Path.Replace('\', '/').TrimStart('/')
    if ([string]::IsNullOrWhiteSpace($normalized) -or
        [System.IO.Path]::IsPathRooted($Path) -or
        $normalized.Split('/') -contains '..') {
        throw "Package provenance path must be a stage-relative path: $Path"
    }
    return $normalized
}

function Get-StageRelativePath {
    param([Parameter(Mandatory)][string]$Path)

    if (-not $Path.StartsWith($stageRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "File is outside the package stage: $Path"
    }
    return $Path.Substring($stageRoot.Length + 1).Replace('\', '/')
}

$provenanceByPath = @{}
foreach ($entry in $ProvenanceEntries) {
    if ($null -eq $entry -or [string]::IsNullOrWhiteSpace([string]$entry.path) -or
        [string]::IsNullOrWhiteSpace([string]$entry.provenance)) {
        throw 'Every package provenance entry must include path and provenance.'
    }
    $relativePath = Get-NormalizedRelativePath -Path ([string]$entry.path)
    $group = [string]$entry.provenance
    if ($groupNames -notcontains $group) {
        throw "Unknown package provenance group '$group' for $relativePath."
    }
    if ($provenanceByPath.ContainsKey($relativePath) -and $provenanceByPath[$relativePath] -ne $group) {
        throw "Package file $relativePath has conflicting provenance."
    }
    $provenanceByPath[$relativePath] = $group
}

$files = @(
    Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
        Where-Object { (Get-StageRelativePath -Path $_.FullName) -ne 'release-manifest.json' } |
        ForEach-Object {
            $relativePath = Get-StageRelativePath -Path $_.FullName
            if (-not $provenanceByPath.ContainsKey($relativePath)) {
                throw "Package file $relativePath has no recorded provenance."
            }
            $version = ''
            try {
                $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($_.FullName)
                if ($versionInfo.FileVersion) { $version = $versionInfo.FileVersion }
            } catch {
                $version = ''
            }
            [pscustomobject][ordered]@{
                path = $relativePath
                bytes = [int64]$_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
                provenance = $provenanceByPath[$relativePath]
                version = $version
            }
        } |
        Sort-Object path
)

$packagePaths = @($files | ForEach-Object { $_.path })
foreach ($groupName in $groupNames) {
    $group = $profile.groups.$groupName
    foreach ($required in @($group.required)) {
        $path = Get-NormalizedRelativePath -Path ([string]$required)
        if ($packagePaths -notcontains $path) {
            throw "Profile $($profile.profile) requires package file $path."
        }
    }
    foreach ($forbidden in @($group.forbidden)) {
        $path = Get-NormalizedRelativePath -Path ([string]$forbidden)
        if ($packagePaths -contains $path) {
            throw "Profile $($profile.profile) forbids package file $path."
        }
    }
}

$manifest = [ordered]@{
    profile = [string]$profile.profile
    architecture = $Architecture
    buildType = $BuildType
    files = $files
}
$outputPath = Join-Path $stageRoot 'release-manifest.json'
$json = $manifest | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText($outputPath, $json + "`n", [System.Text.UTF8Encoding]::new($false))
