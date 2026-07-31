$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$measure = Join-Path $repo 'scripts/measure-windows-startup.ps1'
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-startup-timeout-" + [guid]::NewGuid())

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $hangingProbe = Join-Path $root 'hanging-probe.cmd'
    [System.IO.File]::WriteAllText($hangingProbe, "@echo off`r`ntimeout /t 30 /nobreak >nul`r`n")

    $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $measure `
        -ExecutablePath $hangingProbe -TimeoutSeconds 1 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    $elapsed.Stop()

    Assert-True -Condition ($exitCode -ne 0) -Message 'Hung startup probe must fail.'
    Assert-True -Condition (($output | Out-String) -match 'timed out after 1 seconds') `
        -Message 'Hung startup probe did not report its timeout.'
    Assert-True -Condition ($elapsed.ElapsedMilliseconds -lt 5000) `
        -Message 'Hung startup probe was not terminated promptly.'
    Write-Host 'Windows startup benchmark timeout test passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
