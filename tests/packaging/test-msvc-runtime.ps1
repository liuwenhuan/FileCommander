$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$verifier = Join-Path $repo 'packaging/verify-windows-package.ps1'
$collector = Join-Path $repo 'packaging/collect-msvc-runtime.ps1'
$msixBuilder = Join-Path $repo 'packaging/build-windows-msix.ps1'
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-runtime-" + [guid]::NewGuid())
$nestedInstallerStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-nested-installer-" + [guid]::NewGuid())
$missingRuntimeStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-missing-" + [guid]::NewGuid())
$wrongArchitectureStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-wrong-arch-" + [guid]::NewGuid())
$mixedArchitectureStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-mixed-arch-" + [guid]::NewGuid())
$wrongExecutableStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-wrong-exe-" + [guid]::NewGuid())
$invalidProvenanceStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-invalid-provenance-" + [guid]::NewGuid())
$invalidRuntimeNameStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-invalid-name-" + [guid]::NewGuid())
$missingDeclaredRuntimeStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-missing-declared-" + [guid]::NewGuid())
$nonCrtDStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-non-crt-d-" + [guid]::NewGuid())
$debugCrtStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-debug-crt-" + [guid]::NewGuid())
$discoveryStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-discovery-" + [guid]::NewGuid())
$fakeVsRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-fake-vs-" + [guid]::NewGuid())
$specialPathRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander review [paths] & spaces " + [guid]::NewGuid())
$specialVsRoot = Join-Path $specialPathRoot 'Visual Studio [fake] & tools'
$specialStage = Join-Path $specialPathRoot 'portable stage [x64] & output'
$msixMismatchStage = Join-Path $repo 'dist/FileCommander-windows-x86'
$msixMismatchPackage = Join-Path $repo 'dist/FileCommander-0.2.0-phase2-test-windows-x86.msix'

function Assert-Matches {
    param([string]$Actual, [string]$Pattern, [string]$Message)
    if ($Actual -notmatch $Pattern) {
        throw "$Message Expected pattern '$Pattern', got: $Actual"
    }
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function New-PeFile {
    param([string]$Path, [UInt16]$Machine)

    [byte[]]$bytes = New-Object byte[] 512
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([UInt32]0x80).CopyTo($bytes, 0x3C)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes($Machine).CopyTo($bytes, 0x84)
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

function Write-TestManifest {
    param(
        [string]$Stage,
        [string[]]$RuntimeFiles,
        [string]$Provenance = 'msvc-runtime'
    )

    [ordered]@{
        product = 'FileCommander'
        platform = 'windows-x64'
        officePreview = $false
        pdfPreview = $false
        mediaPreview = $false
        runtime = [ordered]@{
            provenance = $Provenance
            sourceRedistDirectory = 'C:\Test\Microsoft.VC143.CRT'
            files = @($RuntimeFiles)
        }
    } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $Stage 'manifest.json') -Encoding UTF8
}

function Add-RequiredRuntimeFiles {
    param([string]$Stage, [UInt16]$Machine)

    $requiredRuntimeNames = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')
    New-PeFile -Path (Join-Path $Stage 'FileCommander.exe') -Machine $Machine
    foreach ($runtimeName in $requiredRuntimeNames) {
        New-PeFile -Path (Join-Path $Stage $runtimeName) -Machine $Machine
    }
    Write-TestManifest -Stage $Stage -RuntimeFiles $requiredRuntimeNames
}

try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    New-Item -ItemType File -Path (Join-Path $stage 'FileCommander.exe') | Out-Null
    New-Item -ItemType File -Path (Join-Path $stage 'vc_redist.x64.exe') | Out-Null

    try {
        & $verifier -Stage $stage
        throw 'Expected portable verification to reject vc_redist.x64.exe.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'vc_redist\.x64\.exe does not satisfy app-local MSVC runtime dependencies' `
            -Message 'Portable verification did not explain the invalid runtime layout.'
    }

    $nestedInstaller = Join-Path $nestedInstallerStage 'plugins/runtime/vc_redist.x64.exe'
    New-Item -ItemType Directory -Path (Split-Path -Parent $nestedInstaller) -Force | Out-Null
    New-Item -ItemType File -Path (Join-Path $nestedInstallerStage 'FileCommander.exe') | Out-Null
    New-Item -ItemType File -Path $nestedInstaller | Out-Null
    try {
        & $verifier -Stage $nestedInstallerStage
        throw 'Expected portable verification to reject a nested vc_redist installer.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'plugins\\runtime\\vc_redist\.x64\.exe does not satisfy app-local MSVC runtime dependencies' `
            -Message 'Portable verification did not report the nested installer relative path.'
    }

    New-Item -ItemType Directory -Path $missingRuntimeStage | Out-Null
    New-Item -ItemType File -Path (Join-Path $missingRuntimeStage 'FileCommander.exe') | Out-Null
    try {
        & $verifier -Stage $missingRuntimeStage -Architecture x64
        throw 'Expected portable verification to reject a stage with no MSVC runtime DLLs.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message -Pattern 'Missing app-local MSVC runtime dependency: vcruntime140\.dll' `
            -Message 'Portable verification did not reject a missing CRT runtime.'
    }

    New-Item -ItemType Directory -Path $wrongArchitectureStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $wrongArchitectureStage -Machine 0x014C
    try {
        & $verifier -Stage $wrongArchitectureStage -Architecture x64
        throw 'Expected portable verification to reject x86 CRT DLLs in an x64 stage.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'MSVC runtime architecture mismatch: .* is x86, expected x64' `
            -Message 'Portable verification did not reject a wrong-architecture CRT runtime.'
    }

    New-Item -ItemType Directory -Path $mixedArchitectureStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $mixedArchitectureStage -Machine 0x8664
    New-PeFile -Path (Join-Path $mixedArchitectureStage 'msvcp140_2.dll') -Machine 0x014C
    Write-TestManifest -Stage $mixedArchitectureStage `
        -RuntimeFiles @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_2.dll')
    try {
        & $verifier -Stage $mixedArchitectureStage -Architecture x64
        throw 'Expected portable verification to reject a mixed-architecture CRT set.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'MSVC runtime architecture mismatch: msvcp140_2\.dll is x86, expected x64' `
            -Message 'Portable verification did not check every deployed CRT DLL architecture.'
    }

    New-Item -ItemType Directory -Path $wrongExecutableStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $wrongExecutableStage -Machine 0x8664
    New-PeFile -Path (Join-Path $wrongExecutableStage 'FileCommander.exe') -Machine 0x014C
    try {
        & $verifier -Stage $wrongExecutableStage -Architecture x64
        throw 'Expected portable verification to reject a wrong-architecture executable.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'Package executable architecture mismatch: FileCommander\.exe is x86, expected x64' `
            -Message 'Portable verification did not validate FileCommander.exe architecture.'
    }

    if ((Test-Path -LiteralPath $msixMismatchStage) -or
        (Test-Path -LiteralPath $msixMismatchPackage)) {
        throw 'MSIX mismatch test fixture paths must not exist before the test.'
    }
    New-Item -ItemType Directory -Path $msixMismatchStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $msixMismatchStage -Machine 0x014C
    New-PeFile -Path (Join-Path $msixMismatchStage 'FileCommander.exe') -Machine 0x8664
    try {
        & $msixBuilder -SkipPortableBuild -Architecture x86
        throw 'Expected the MSIX skip path to reject a mismatched staged executable.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'Package executable architecture mismatch: FileCommander\.exe is x64, expected x86' `
            -Message 'MSIX skip-path packaging did not verify the staged executable architecture.'
    }

    New-Item -ItemType Directory -Path $invalidProvenanceStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $invalidProvenanceStage -Machine 0x8664
    Write-TestManifest -Stage $invalidProvenanceStage `
        -RuntimeFiles @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') `
        -Provenance 'unknown'
    try {
        & $verifier -Stage $invalidProvenanceStage -Architecture x64
        throw 'Expected portable verification to reject invalid runtime provenance.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'MSVC runtime provenance must be msvc-runtime' `
            -Message 'Portable verification did not validate runtime provenance.'
    }

    New-Item -ItemType Directory -Path $invalidRuntimeNameStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $invalidRuntimeNameStage -Machine 0x8664
    New-PeFile -Path (Join-Path $invalidRuntimeNameStage 'helper140.dll') -Machine 0x8664
    Write-TestManifest -Stage $invalidRuntimeNameStage `
        -RuntimeFiles @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'helper140.dll')
    try {
        & $verifier -Stage $invalidRuntimeNameStage -Architecture x64
        throw 'Expected portable verification to reject a non-CRT manifest runtime name.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'Manifest-declared MSVC runtime is not a release VC143 CRT DLL: helper140\.dll' `
            -Message 'Portable verification did not validate declared runtime release names.'
    }

    New-Item -ItemType Directory -Path $missingDeclaredRuntimeStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $missingDeclaredRuntimeStage -Machine 0x8664
    Write-TestManifest -Stage $missingDeclaredRuntimeStage `
        -RuntimeFiles @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_2.dll')
    try {
        & $verifier -Stage $missingDeclaredRuntimeStage -Architecture x64
        throw 'Expected portable verification to reject a missing manifest-declared CRT DLL.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'Manifest-declared MSVC runtime is missing: msvcp140_2\.dll' `
            -Message 'Portable verification did not require every manifest-declared runtime file.'
    }

    New-Item -ItemType Directory -Path $nonCrtDStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $nonCrtDStage -Machine 0x8664
    New-Item -ItemType File -Path (Join-Path $nonCrtDStage 'zstd.dll') | Out-Null
    try {
        & $verifier -Stage $nonCrtDStage -Architecture x64
        throw 'Expected portable verification to continue to its ordinary package checks.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message -Pattern 'Missing package file: Qt5Core\.dll' `
            -Message 'Portable verification treated a non-CRT DLL ending in d.dll as a debug MSVC runtime.'
    }

    New-Item -ItemType Directory -Path $debugCrtStage | Out-Null
    Add-RequiredRuntimeFiles -Stage $debugCrtStage -Machine 0x8664
    New-Item -ItemType File -Path (Join-Path $debugCrtStage 'vcruntime140d.dll') | Out-Null
    try {
        & $verifier -Stage $debugCrtStage -Architecture x64
        throw 'Expected portable verification to reject a debug CRT DLL.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message -Pattern 'Debug MSVC runtime detected' `
            -Message 'Portable verification did not reject a debug CRT runtime.'
    }

    New-Item -ItemType Directory -Path $discoveryStage | Out-Null
    $runtime = & $collector -Stage $discoveryStage -Architecture x64 -Mode Portable
    Assert-True -Condition ($runtime.SourceRedistDirectory -and (Test-Path -LiteralPath $runtime.SourceRedistDirectory)) `
        -Message 'MSVC runtime collection did not report the discovered redist directory.'
    Assert-True -Condition ($runtime.CopiedCrtDllPaths.Count -gt 0) `
        -Message 'MSVC runtime collection did not report copied CRT DLLs.'
    foreach ($copied in $runtime.CopiedCrtDllPaths) {
        Assert-True -Condition (Test-Path -LiteralPath $copied) `
            -Message "MSVC runtime collection did not copy $copied."
        Assert-True -Condition ($copied -notmatch 'd\.dll$') `
            -Message "MSVC runtime collection copied a debug DLL: $copied"
    }
    Assert-True -Condition (-not (Get-ChildItem -LiteralPath $discoveryStage -Filter 'vc_redist*.exe')) `
        -Message 'MSVC runtime collection copied a vc_redist installer into the portable stage.'

    $specialVersion = '14.88.12345'
    $specialVersionFile = Join-Path $specialVsRoot 'VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt'
    $specialRedist = Join-Path $specialVsRoot "VC/Redist/MSVC/$specialVersion/x64/Microsoft.VC143.CRT"
    $specialVsWhere = Join-Path $specialPathRoot 'installer [tools] & scripts/vswhere.ps1'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $specialVersionFile) | Out-Null
    New-Item -ItemType Directory -Force -Path $specialRedist | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $specialVsWhere) | Out-Null
    New-Item -ItemType Directory -Force -Path $specialStage | Out-Null
    $nestedCollectedInstaller = Join-Path $specialStage 'plugins/runtime/vc_redist.x64.exe'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $nestedCollectedInstaller) | Out-Null
    New-Item -ItemType File -Path $nestedCollectedInstaller | Out-Null
    Set-Content -LiteralPath $specialVersionFile -Value $specialVersion -NoNewline
    foreach ($runtimeName in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')) {
        New-PeFile -Path (Join-Path $specialRedist $runtimeName) -Machine 0x8664
    }
    New-PeFile -Path (Join-Path $specialRedist 'vcruntime140d.dll') -Machine 0x8664
    $vsWhereFixture = @'
[CmdletBinding()]
param(
    [switch]$latest,
    [string[]]$products,
    [string[]]$requires,
    [string]$property
)
Write-Output '__INSTALLATION_PATH__'
'@.Replace('__INSTALLATION_PATH__', $specialVsRoot.Replace("'", "''"))
    Set-Content -LiteralPath $specialVsWhere -Value $vsWhereFixture -Encoding UTF8

    $specialRuntime = & $collector -Stage $specialStage -Architecture x64 -Mode Portable `
        -VsWherePath $specialVsWhere
    Assert-True -Condition ($specialRuntime.SourceRedistDirectory -eq $specialRedist) `
        -Message 'MSVC runtime discovery changed a special-character source path.'
    Assert-True -Condition ($specialRuntime.CopiedCrtDllPaths.Count -eq 3) `
        -Message 'MSVC runtime collection did not copy the expected release DLLs from the special-character path.'
    foreach ($runtimeName in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')) {
        Assert-True -Condition (Test-Path -LiteralPath (Join-Path $specialStage $runtimeName)) `
            -Message "MSVC runtime collection missed $runtimeName in the special-character stage."
    }
    Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $specialStage 'vcruntime140d.dll'))) `
        -Message 'MSVC runtime collection copied a debug DLL from the special-character redist path.'
    Assert-True -Condition (-not (Test-Path -LiteralPath $nestedCollectedInstaller)) `
        -Message 'Portable runtime collection did not remove a nested vc_redist installer.'

    $redistVersionFile = Join-Path $fakeVsRoot 'VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $redistVersionFile) | Out-Null
    Set-Content -LiteralPath $redistVersionFile -Value '14.99.0' -NoNewline
    $fakeVsWhere = Join-Path $fakeVsRoot 'vswhere.cmd'
    Set-Content -LiteralPath $fakeVsWhere -Value "@echo $fakeVsRoot" -NoNewline
    try {
        & $collector -Stage $discoveryStage -Architecture x64 -Mode Portable -VsWherePath $fakeVsWhere
        throw 'Expected MSVC runtime collection to fail for an absent redist directory.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message -Pattern 'MSVC redistributable runtime.*14\.99\.0.*x64' `
            -Message 'MSVC runtime collection did not give an actionable missing-runtime error.'
    }

    $emptyVsWhere = Join-Path $fakeVsRoot 'empty-vswhere.cmd'
    Set-Content -LiteralPath $emptyVsWhere -Value '@exit /b 0' -NoNewline
    try {
        & $collector -Stage $discoveryStage -Architecture x64 -Mode Portable -VsWherePath $emptyVsWhere
        throw 'Expected MSVC runtime collection to fail when vswhere finds no MSVC toolchain.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message -Pattern 'vswhere\.exe did not return a Visual Studio installation' `
            -Message 'MSVC runtime collection did not explain an empty vswhere result.'
    }

    Write-Host 'MSVC runtime packaging tests passed.'
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $nestedInstallerStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $missingRuntimeStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $wrongArchitectureStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $mixedArchitectureStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $wrongExecutableStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $invalidProvenanceStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $invalidRuntimeNameStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $missingDeclaredRuntimeStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $nonCrtDStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $debugCrtStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $discoveryStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fakeVsRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $specialPathRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $msixMismatchStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $msixMismatchPackage -Force -ErrorAction SilentlyContinue
}
