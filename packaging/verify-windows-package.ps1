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
& (Join-Path $PSScriptRoot '..\tests\packaging\test-windows-gui-subsystem.ps1') `
    -Executable (Join-Path $resolved 'FileCommander.exe')
if ($LASTEXITCODE) { throw 'Windows GUI subsystem check failed.' }
$manifest = Get-Content -LiteralPath (Join-Path $resolved 'manifest.json') -Raw |
    ConvertFrom-Json
if ($manifest.officePreview -and
    -not (Test-Path -LiteralPath (Join-Path $resolved 'office-oxide.exe'))) {
    throw 'Office preview is enabled but office-oxide.exe is missing.'
}
if ($manifest.pdfPreview) {
    foreach ($required in @('Qt5Xml.dll', 'poppler-qt5.dll', 'poppler.dll', 'freetype.dll',
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

function Add-ZipText {
    param($Zip, [string]$Name, [string]$Text)
    $entry = $Zip.CreateEntry($Name)
    $writer = [System.IO.StreamWriter]::new($entry.Open(), [System.Text.UTF8Encoding]::new($false))
    try { $writer.Write($Text) } finally { $writer.Dispose() }
}

function New-PackageSmokeFixtures {
    param([string]$Root, $PackageManifest)
    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $Root 'smoke.png'),
        [System.Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL9OwAAAABJRU5ErkJggg=='))

    if ($PackageManifest.pdfPreview) {
        $encoding = [System.Text.Encoding]::ASCII
        $objects = @(
            "1 0 obj`n<< /Type /Catalog /Pages 2 0 R >>`nendobj`n",
            "2 0 obj`n<< /Type /Pages /Kids [3 0 R] /Count 1 >>`nendobj`n",
            "3 0 obj`n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << >> /Contents 4 0 R >>`nendobj`n",
            "4 0 obj`n<< /Length 0 >>`nstream`n`nendstream`nendobj`n"
        )
        $body = [System.Text.StringBuilder]::new("%PDF-1.4`n")
        $offsets = @()
        foreach ($object in $objects) {
            $offsets += $encoding.GetByteCount($body.ToString())
            [void]$body.Append($object)
        }
        $xref = $encoding.GetByteCount($body.ToString())
        [void]$body.Append("xref`n0 5`n0000000000 65535 f `n")
        foreach ($offset in $offsets) { [void]$body.AppendFormat('{0:D10} 00000 n {1}', $offset, "`n") }
        [void]$body.Append("trailer`n<< /Size 5 /Root 1 0 R >>`nstartxref`n$xref`n%%EOF`n")
        [System.IO.File]::WriteAllText((Join-Path $Root 'smoke.pdf'), $body.ToString(), $encoding)
    }

    if ($PackageManifest.officePreview) {
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [System.IO.Compression.ZipFile]::Open((Join-Path $Root 'smoke.docx'),
            [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            Add-ZipText $archive '[Content_Types].xml' '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>'
            Add-ZipText $archive '_rels/.rels' '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'
            Add-ZipText $archive 'word/document.xml' '<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>FileCommander package smoke test</w:t></w:r></w:p><w:sectPr/></w:body></w:document>'
        } finally { $archive.Dispose() }
    }

    if ($PackageManifest.mediaPreview) {
        $stream = [System.IO.File]::Open((Join-Path $Root 'smoke.wav'), [System.IO.FileMode]::Create)
        $writer = [System.IO.BinaryWriter]::new($stream)
        try {
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
            $writer.Write([int]40); $writer.Write([System.Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
            $writer.Write([int]16); $writer.Write([int16]1); $writer.Write([int16]1)
            $writer.Write([int]8000); $writer.Write([int]16000); $writer.Write([int16]2); $writer.Write([int16]16)
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([int]4); $writer.Write([int]0)
        } finally { $writer.Dispose() }
    }
}

$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("FileCommander-package-smoke-" + [guid]::NewGuid())
New-PackageSmokeFixtures -Root $fixtureRoot -PackageManifest $manifest
$oldPlatform = $env:QT_QPA_PLATFORM
$oldPath = $env:PATH
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
try {
    # Keep the smoke test honest: no Qt, vcpkg, Poppler, or mpv development
    # directory may satisfy a missing runtime dependency.
    $env:PATH = "$resolved;$env:SystemRoot\System32;$env:SystemRoot"
    $process = Start-Process -FilePath (Join-Path $resolved 'FileCommander.exe') `
        -ArgumentList @('--package-smoke', $fixtureRoot) -WorkingDirectory $resolved -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit(45000)) {
        $process.Kill()
        $process.WaitForExit()
        throw 'Package preview smoke test timed out.'
    }
    if ($process.ExitCode -ne 0) {
        throw "Package preview smoke test failed with code $($process.ExitCode)."
    }
} finally {
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
    $env:QT_QPA_PLATFORM = $oldPlatform
    $env:PATH = $oldPath
}
Write-Host 'Windows portable package verification passed.'
