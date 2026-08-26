[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string[]]$Executables,
    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'

function Stop-ProcessTree {
    param([Parameter(Mandatory)][int]$ProcessId)
    & "$env:SystemRoot\System32\taskkill.exe" /PID $ProcessId /T /F 2>$null | Out-Null
}

function Invoke-GTest {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][string]$Label
    )

    $path = (Resolve-Path -LiteralPath $Executable -ErrorAction Stop).Path
    Write-Host "==> $Label"

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $path
    $startInfo.Arguments = $Arguments -join ' '
    $startInfo.WorkingDirectory = Split-Path -Parent $path
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not start GTest: $Label"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $completed = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $completed) {
        Stop-ProcessTree -ProcessId $process.Id
        $process.WaitForExit(5000)
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        Write-Host "--- stdout ($Label) ---"
        if ($stdout) { Write-Host $stdout }
        Write-Host "--- stderr ($Label) ---"
        if ($stderr) { Write-Host $stderr }
        throw "GTest timed out after ${TimeoutSeconds}s: $Label (pid $($process.Id))"
    }

    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $exitCode = $process.ExitCode
    $process.Dispose()
    if ($exitCode -ne 0) {
        Write-Host "--- stdout ($Label) ---"
        if ($stdout) { Write-Host $stdout }
        Write-Host "--- stderr ($Label) ---"
        if ($stderr) { Write-Host $stderr }
        throw "GTest exited with code ${exitCode}: $Label"
    }
    return $stdout
}

foreach ($executable in $Executables) {
    $listOutput = Invoke-GTest -Executable $executable `
        -Arguments @('--gtest_list_tests') `
        -Label "$executable list"
    $currentSuite = $null
    $cases = foreach ($line in ($listOutput -split "`r?`n")) {
        if ($line -match '^[^\s].*\.$') {
            $currentSuite = $line.TrimEnd('.')
            continue
        }
        if ($currentSuite -and $line -match '^\s+(\S+)\s*$') {
            "$currentSuite.$($Matches[1])"
        }
    }
    if (-not $cases) {
        throw "GTest listed no test cases: $executable"
    }
    foreach ($testCase in $cases) {
        Invoke-GTest -Executable $executable `
            -Arguments @('--gtest_color=no', "--gtest_filter=$testCase") `
            -Label "$executable [$testCase]" | Out-Null
    }
}

Write-Host 'All Windows GTest suites passed.'
