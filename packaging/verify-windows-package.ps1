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
if (Get-ChildItem -LiteralPath $resolved -Recurse -Filter '*d.dll') {
    throw 'Debug DLL detected in release package.'
}
$oldPlatform = $env:QT_QPA_PLATFORM
$env:QT_QPA_PLATFORM = 'offscreen'
try {
    $process = Start-Process -FilePath (Join-Path $resolved 'FileCommander.exe') `
        -WorkingDirectory $resolved -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit(3000)) {
        $process.Kill()
    } elseif ($process.ExitCode -ne 0) {
        throw "Smoke startup exited with code $($process.ExitCode)."
    }
} finally {
    $env:QT_QPA_PLATFORM = $oldPlatform
}
Write-Host 'Windows portable package verification passed.'
