param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable not found: $Executable"
}

$dumpbin = Get-ChildItem -Path 'C:\Program Files (x86)\Microsoft Visual Studio\2022' -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin) {
    throw 'dumpbin.exe was not found.'
}

$headers = & $dumpbin /headers $Executable 2>&1 | Out-String
if ($headers -notmatch 'subsystem \(Windows GUI\)') {
    throw "FileCommander must be linked as a Windows GUI application. Detected headers:`n$headers"
}
