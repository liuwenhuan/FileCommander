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
$manifest = Get-Content -LiteralPath (Join-Path $resolved 'manifest.json') -Raw |
    ConvertFrom-Json
if ($manifest.officePreview -and
    -not (Test-Path -LiteralPath (Join-Path $resolved 'office-oxide.exe'))) {
    throw 'Office preview is enabled but office-oxide.exe is missing.'
}
if ($manifest.pdfPreview) {
    foreach ($required in @('poppler-qt5.dll', 'poppler.dll', 'freetype.dll',
                            'openjp2.dll', 'libpng16.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $resolved $required))) {
            throw "PDF preview runtime is missing: $required"
        }
    }
}
if ($manifest.mediaPreview -and
    -not (Test-Path -LiteralPath (Join-Path $resolved 'libmpv-2.dll'))) {
    throw 'Media preview is enabled but libmpv-2.dll is missing.'
}
if (Get-ChildItem -LiteralPath $resolved -Recurse -Filter '*.dll' |
        Where-Object { $_.BaseName -match '^(Qt5.*d|gtestd|gtest_maind|zd|archived)$' }) {
    throw 'Debug DLL detected in release package.'
}
$oldPlatform = $env:QT_QPA_PLATFORM
$oldPath = $env:PATH
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
try {
    # Keep the smoke test honest: no Qt, vcpkg, Poppler, or mpv development
    # directory may satisfy a missing runtime dependency.
    $env:PATH = "$resolved;$env:SystemRoot\System32;$env:SystemRoot"
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
    $env:PATH = $oldPath
}
Write-Host 'Windows portable package verification passed.'
