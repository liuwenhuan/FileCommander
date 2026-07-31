[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath
)

$ErrorActionPreference = 'Stop'

$executable = (Resolve-Path -LiteralPath $ExecutablePath -ErrorAction Stop).Path
$runDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("filecommander-startup-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $runDirectory | Out-Null

try {
    $results = @()
    foreach ($run in 1..7) {
        $outputPath = Join-Path $runDirectory ("startup-$run.json")
        $process = Start-Process -FilePath $executable -ArgumentList "--startup-probe `"$outputPath`"" `
            -PassThru -Wait
        if ($process.ExitCode -ne 0) {
            throw "Startup probe run $run exited with code $($process.ExitCode)."
        }
        if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
            throw "Startup probe run $run did not create $outputPath."
        }

        $result = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
        foreach ($field in @('visibleMs', 'panelsLoadedMs', 'interactiveMs')) {
            $value = $result.$field
            if ($value -is [string] -or $null -eq $value -or
                [math]::Truncate([double]$value) -ne [double]$value -or $value -lt 0) {
                throw "Startup probe run $run returned invalid ${field}: $value"
            }
        }

        $results += [pscustomobject]@{
            Run = $run
            visibleMs = [int]$result.visibleMs
            panelsLoadedMs = [int]$result.panelsLoadedMs
            interactiveMs = [int]$result.interactiveMs
        }
        Write-Host ("Run {0}: visibleMs={1} panelsLoadedMs={2} interactiveMs={3}" -f
            $run, $result.visibleMs, $result.panelsLoadedMs, $result.interactiveMs)
    }

    $interactive = @($results | ForEach-Object interactiveMs | Sort-Object)
    $median = $interactive[3]
    $p90 = $interactive[[math]::Ceiling($interactive.Count * 0.90) - 1]
    $maximum = $interactive[-1]
    Write-Host "interactiveMs median=$median p90=$p90 max=$maximum"

    if ($median -gt 1000 -or $p90 -gt 1200 -or $maximum -gt 1500) {
        Write-Error "Windows startup budget exceeded: median=$median p90=$p90 max=$maximum"
        exit 1
    }
} finally {
    Remove-Item -LiteralPath $runDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
