param(
    [string]$Executable,
    [string]$DisplayPath = $Executable
)

$ErrorActionPreference = 'Stop'

function Get-PeSubsystem {
    param([Parameter(Mandatory)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $DisplayPath" }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Not a PE file: $DisplayPath" }
        $stream.Position = $peOffset + 24
        $magic = $reader.ReadUInt16()
        if ($magic -notin @(0x010B, 0x020B)) { throw "Unsupported PE optional header in $DisplayPath" }
        $stream.Position = $peOffset + 24 + 68
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Assert-WindowsGuiSubsystem {
    param([Parameter(Mandatory)][string]$Path, [string]$PathLabel = $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Executable not found: $PathLabel"
    }
    $subsystem = Get-PeSubsystem -Path $Path
    if ($subsystem -ne 2) {
        throw "$PathLabel must use the Windows GUI subsystem; found subsystem value $subsystem."
    }
}

function New-TestPeFile {
    param([string]$Path, [UInt16]$Subsystem)

    [byte[]]$bytes = New-Object byte[] 512
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([UInt32]0x80).CopyTo($bytes, 0x3C)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([UInt16]0x8664).CopyTo($bytes, 0x84)
    [BitConverter]::GetBytes([UInt16]0x20B).CopyTo($bytes, 0x98)
    [BitConverter]::GetBytes($Subsystem).CopyTo($bytes, 0xDC)
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

if ($Executable) {
    Assert-WindowsGuiSubsystem -Path $Executable -PathLabel $DisplayPath
    Write-Host 'Windows GUI subsystem check passed.'
    return
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-gui-subsystem-" + [guid]::NewGuid())
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $guiExecutable = Join-Path $root 'FileCommander.exe'
    $consoleExecutable = Join-Path $root 'console.exe'
    New-TestPeFile -Path $guiExecutable -Subsystem 2
    Assert-WindowsGuiSubsystem -Path $guiExecutable -PathLabel 'FileCommander.exe'
    New-TestPeFile -Path $consoleExecutable -Subsystem 3
    try {
        Assert-WindowsGuiSubsystem -Path $consoleExecutable -PathLabel 'console.exe'
        throw 'GUI subsystem check accepted a console executable.'
    } catch {
        if ($_.Exception.Message -notmatch 'console\.exe') { throw }
    }
    Write-Host 'Windows GUI subsystem tests passed.'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
