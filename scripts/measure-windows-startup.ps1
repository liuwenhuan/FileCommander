[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath,
    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 15,
    [switch]$UseRealSession
)

$ErrorActionPreference = 'Stop'

$executable = (Resolve-Path -LiteralPath $ExecutablePath -ErrorAction Stop).Path
$runDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("filecommander-startup-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $runDirectory | Out-Null

try {
    $results = @()
    $phaseFields = @(
        'qApplicationConstructedMs',
        'systemFontCapturedMs',
        'applicationIdentityReadyMs',
        'settingsReadyMs',
        'applicationFontReadyMs',
        'translationReadyMs',
        'applicationSetupMs',
        'mainWindowBodyStartedMs',
        'panelsConstructionStartedMs',
        'leftPanelConstructedMs',
        'panelsConstructedMs',
        'operationQueueConstructedMs',
        'panelPreferencesRestoredMs',
        'interfaceTypographyAppliedMs',
        'panelVisibilityRestoredMs',
        'viewSettingsRestoredMs',
        'sessionDataLoadedMs',
        'sessionNavigationDispatchedMs',
        'shortcutsTitleBarReadyMs',
        'startupThemeApplyStartedMs',
        'startupThemeApplyFinishedMs',
        'mainWindowConstructedMs',
        'folderArgumentsProcessedMs',
        'firstShowMs',
        'showReturnedMs',
        'readinessMs'
    )
    $warmupRuns = 2
    $measuredRuns = 7
    foreach ($attempt in 1..($warmupRuns + $measuredRuns)) {
        $isWarmup = $attempt -le $warmupRuns
        $run = $attempt - $warmupRuns
        $label = if ($isWarmup) { "warmup $attempt" } else { "run $run" }
        $outputPath = Join-Path $runDirectory ("startup-$attempt.json")
        $leftDirectory = Join-Path $runDirectory ("left-$attempt")
        $rightDirectory = Join-Path $runDirectory ("right-$attempt")
        if ($UseRealSession) {
            $arguments = "--startup-probe `"$outputPath`""
        } else {
            New-Item -ItemType Directory -Path $leftDirectory, $rightDirectory | Out-Null
            [System.IO.File]::WriteAllText((Join-Path $leftDirectory 'first.txt'), 'first')
            [System.IO.File]::WriteAllText((Join-Path $leftDirectory 'second.txt'), 'second')
            [System.IO.File]::WriteAllText((Join-Path $rightDirectory 'first.txt'), 'first')
            [System.IO.File]::WriteAllText((Join-Path $rightDirectory 'second.txt'), 'second')
            $arguments = "--startup-probe `"$outputPath`" `"$leftDirectory`" `"$rightDirectory`""
        }
        $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Startup probe $label timed out after $TimeoutSeconds seconds."
        }
        if ($process.ExitCode -ne 0) {
            throw "Startup probe $label exited with code $($process.ExitCode)."
        }
        if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
            throw "Startup probe $label did not create $outputPath."
        }

        $result = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
        foreach ($field in @('visibleMs', 'panelsLoadedMs', 'interactiveMs')) {
            $value = $result.$field
            if ($value -is [string] -or $null -eq $value -or
                [math]::Truncate([double]$value) -ne [double]$value -or $value -lt 0) {
                throw "Startup probe $label returned invalid ${field}: $value"
            }
        }
        $validatedPrevious = -1
        foreach ($field in $phaseFields) {
            $value = $result.$field
            if ($value -is [string] -or $null -eq $value -or
                [math]::Truncate([double]$value) -ne [double]$value -or $value -lt 0) {
                throw "Startup probe $label returned invalid ${field}: $value"
            }
            if ([int]$value -lt $validatedPrevious) {
                throw "Startup probe $label returned a non-monotonic ${field}: $value after $validatedPrevious"
            }
            $validatedPrevious = [int]$value
        }

        if ($isWarmup) {
            Write-Host ("Warmup {0}: interactiveMs={1}" -f $attempt, $result.interactiveMs)
            continue
        }

        $results += [pscustomobject]@{
            Run = $run
            visibleMs = [int]$result.visibleMs
            panelsLoadedMs = [int]$result.panelsLoadedMs
            interactiveMs = [int]$result.interactiveMs
        }
        Write-Host ("Run {0}: visibleMs={1} panelsLoadedMs={2} interactiveMs={3}" -f
            $run, $result.visibleMs, $result.panelsLoadedMs, $result.interactiveMs)
        $previous = 0
        $segments = foreach ($field in $phaseFields) {
            $value = [int]$result.$field
            $delta = $value - $previous
            $previous = $value
            "${field}=${value}(+${delta})"
        }
        Write-Host ("  phases: " + ($segments -join ' '))
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
