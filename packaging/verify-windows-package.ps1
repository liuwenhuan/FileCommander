[CmdletBinding()]
param([Parameter(Mandatory)][string]$Stage)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $Stage).Path
foreach ($required in @('FileCommander.exe', 'Qt5Core.dll', 'Qt5Gui.dll',
                        'Qt5Widgets.dll', 'platforms/qwindows.dll', 'manifest.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $resolved $required))) {
        throw "Missing package file: $required"
    }
}
if (Get-ChildItem -LiteralPath $resolved -Recurse -Filter '*.dll' |
        Where-Object { $_.BaseName -match '^(Qt5.*d|gtestd|gtest_maind|zd|archived)$' }) {
    throw 'Debug DLL detected in release package.'
}
$oldPlatform = $env:QT_QPA_PLATFORM
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
try {
    $process = Start-Process -FilePath (Join-Path $resolved 'FileCommander.exe') `
        -WorkingDirectory $resolved -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    if ($process.HasExited) {
        throw "Smoke startup exited early with code $($process.ExitCode)."
    }
    $process.Kill()
    $process.WaitForExit()
} finally {
    $env:QT_QPA_PLATFORM = $oldPlatform
}
Write-Host 'Windows portable package verification passed.'
