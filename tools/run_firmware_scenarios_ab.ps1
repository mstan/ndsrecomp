# Firmware-scenario gate (G1) for a runner build in an arbitrary worktree,
# on caller-chosen ports, with a caller-chosen env toggle applied to the
# native side.
#
# The workspace-root run_firmware_scenario.ps1 hardcodes both the exe path and
# the shared 19842/19843 oracle ports, so it cannot gate a second worktree
# while another session is using the oracle. This driver takes the exe, the
# ports and the env pairs, and only ever stops the PIDs it started itself.
#
# Example:
#   tools\run_firmware_scenarios_ab.ps1 `
#     -NativeExe F:\...\build-dp\nds_runner.exe `
#     -NativePort 19980 -OraclePort 19981 `
#     -Env @{ NDS_GPU2D_THREADED = '1' }
param(
    [Parameter(Mandatory = $true)][string]$NativeExe,
    [string]$OracleExe = "F:\Projects\ndsrecomp\ndsref\build-native\ndsref.exe",
    [string]$BiosDir = "F:\Projects\ndsrecomp\ndsrecomp\bios",
    [int]$NativePort = 19980,
    [int]$OraclePort = 19981,
    [hashtable]$Env = @{},
    [string[]]$Scenarios = @(
        "calibration_save", "date_alarm_save", "profile_save",
        "system_options_save", "main_menu_controls",
        "download_play_shutdown", "pictochat_room_a", "shutdown"),
    [string]$Framework = $PSScriptRoot + "\..",
    # Diagnostic only: downgrades an audio divergence to a warning so the
    # framebuffer and event-counter checkpoints are still reached. A run with
    # this set is NOT a G1 pass.
    [switch]$IgnoreAudio
)

$ErrorActionPreference = "Stop"
$Framework = (Resolve-Path $Framework).Path
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$results = @()

foreach ($scenario in $Scenarios) {
    $nativeOut = Join-Path $env:TEMP "g1-$scenario-$stamp-native.log"
    $oracleOut = Join-Path $env:TEMP "g1-$scenario-$stamp-oracle.log"
    $native = $null
    $oracle = $null
    # Apply the env pairs to this process so the child inherits them, and
    # restore afterwards.
    $saved = @{}
    foreach ($k in $Env.Keys) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k)
        [Environment]::SetEnvironmentVariable($k, $Env[$k])
    }
    try {
        $native = Start-Process -FilePath $NativeExe `
            -ArgumentList @("`"$BiosDir`"", "--serve", "--port", "$NativePort") `
            -WorkingDirectory $Framework -WindowStyle Hidden `
            -RedirectStandardOutput $nativeOut `
            -RedirectStandardError "$nativeOut.err" -PassThru
        $oracle = Start-Process -FilePath $OracleExe `
            -ArgumentList @(
                "--bios9", "`"$(Join-Path $BiosDir 'biosnds9.rom')`"",
                "--bios7", "`"$(Join-Path $BiosDir 'biosnds7.rom')`"",
                "--firmware", "`"$(Join-Path $BiosDir 'firmware.bin')`"",
                "--boot", "firmware", "--port", "$OraclePort") `
            -WorkingDirectory $Framework -WindowStyle Hidden `
            -RedirectStandardOutput $oracleOut `
            -RedirectStandardError "$oracleOut.err" -PassThru

        $deadline = (Get-Date).AddSeconds(45)
        do {
            Start-Sleep -Milliseconds 250
            $pattern = ":($NativePort|$OraclePort)\s+.*LISTENING"
            $ready = @(netstat -ano | Select-String -Pattern $pattern)
        } until (
            $ready.Count -ge 2 -or (Get-Date) -gt $deadline -or
            $native.HasExited -or $oracle.HasExited)
        if ($ready.Count -lt 2) { throw "server pair not ready for $scenario" }

        Push-Location $Framework
        try {
            $extra = @()
            if ($IgnoreAudio) { $extra += "--ignore-audio" }
            & py -3 oracle\firmware_traversal.py --scenario $scenario `
                --require-zero-tier3 --native-port $NativePort `
                --oracle-port $OraclePort @extra 2>&1 |
                Tee-Object -Variable out | Out-Null
            $code = $LASTEXITCODE
        } finally { Pop-Location }
        $results += [pscustomobject]@{
            Scenario = $scenario
            Exit = $code
            Verdict = if ($code -eq 0) { "PASS" } else { "FAIL" }
            Tail = ($out | Select-Object -Last 3) -join " | "
        }
    } finally {
        # Only ever the PIDs this script started.
        foreach ($p in $native, $oracle) {
            if ($p -and -not $p.HasExited) {
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            }
        }
        foreach ($k in $Env.Keys) {
            [Environment]::SetEnvironmentVariable($k, $saved[$k])
        }
    }
}

$results | Format-Table -AutoSize
$failed = @($results | Where-Object { $_.Exit -ne 0 })
Write-Output ("G1: {0}/{1} scenarios passed" -f
    ($results.Count - $failed.Count), $results.Count)
if ($failed.Count -gt 0) { exit 1 }
