$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$verifier = Join-Path $repo 'packaging/verify-windows-package.ps1'
$collector = Join-Path $repo 'packaging/collect-msvc-runtime.ps1'
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-runtime-" + [guid]::NewGuid())
$missingRuntimeStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-missing-" + [guid]::NewGuid())
$wrongArchitectureStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-wrong-arch-" + [guid]::NewGuid())
$mixedArchitectureStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-mixed-arch-" + [guid]::NewGuid())
$nonCrtDStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-non-crt-d-" + [guid]::NewGuid())
$debugCrtStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-debug-crt-" + [guid]::NewGuid())
$discoveryStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-msvc-discovery-" + [guid]::NewGuid())
$fakeVsRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-fake-vs-" + [guid]::NewGuid())

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

function Add-RequiredRuntimeFiles {
    param([string]$Stage, [UInt16]$Machine)

    New-Item -ItemType File -Path (Join-Path $Stage 'FileCommander.exe') | Out-Null
    foreach ($runtimeName in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')) {
        New-PeFile -Path (Join-Path $Stage $runtimeName) -Machine $Machine
    }
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
    try {
        & $verifier -Stage $mixedArchitectureStage -Architecture x64
        throw 'Expected portable verification to reject a mixed-architecture CRT set.'
    } catch {
        Assert-Matches -Actual $_.Exception.Message `
            -Pattern 'MSVC runtime architecture mismatch: msvcp140_2\.dll is x86, expected x64' `
            -Message 'Portable verification did not check every deployed CRT DLL architecture.'
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
    Remove-Item -LiteralPath $missingRuntimeStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $wrongArchitectureStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $mixedArchitectureStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $nonCrtDStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $debugCrtStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $discoveryStage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fakeVsRoot -Recurse -Force -ErrorAction SilentlyContinue
}
