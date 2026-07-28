[CmdletBinding()]
param(
    [string]$OutputDir
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'build/office-oxide' }
$pin = @{}
Get-Content (Join-Path $repoRoot 'third_party/office_oxide.version') | ForEach-Object {
    $key, $value = $_ -split '=', 2
    $pin[$key] = $value
}
$source = Join-Path $OutputDir 'source'
if (-not (Test-Path -LiteralPath (Join-Path $source '.git'))) {
    git clone $pin.repository $source
}
git -C $source fetch origin $pin.commit --depth 1
git -C $source checkout --detach $pin.commit
$actual = (git -C $source rev-parse HEAD).Trim()
if ($actual -ne $pin.commit) { throw "office_oxide pin mismatch: $actual" }
cargo build --manifest-path (Join-Path $source 'Cargo.toml') --release -p $pin.package
if ($LASTEXITCODE) { throw 'office_oxide build failed.' }
$binary = Join-Path $source 'target/release/office-oxide.exe'
if (-not (Test-Path -LiteralPath $binary)) {
    $binary = Join-Path $source 'target/release/office_oxide.exe'
}
if (-not (Test-Path -LiteralPath $binary)) { throw 'office-oxide executable not found.' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
Copy-Item -LiteralPath $binary -Destination (Join-Path $OutputDir 'office-oxide.exe') -Force
Write-Host "Built pinned office-oxide at $OutputDir"
