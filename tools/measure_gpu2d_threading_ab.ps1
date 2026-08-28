# Interleaved A/B for the threaded GPU2D scanline renderer.
#
# Runs the SAME binary alternately with NDS_GPU2D_THREADED=0 and =1 to a fixed
# guest anchor (insn9), so both legs execute identical guest work, and reports
# min-of-N per leg. Two metrics:
#
#   wall_s        end-to-end wall time to the anchor, profiling OFF. This is
#                 the honest end-to-end number; nothing is instrumented.
#   display_ms    scheduler display_ns at the anchor, profiling ON. This is
#                 the emu-thread cost of the display device specifically, and
#                 it is what the change targets. Note gpu2d render_ns is NOT
#                 an emu-thread number in the threaded leg -- it is measured
#                 inside the worker.
#
# -Affinity limits both legs to the same core mask (e.g. 0x3 = 2 cores) to
# approximate field hardware.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$BiosDir = "F:\Projects\ndsrecomp\ndsrecomp\bios",
    [string]$Rom = "",
    [string]$Config = "",
    [string]$Boot = "lle",
    [int]$Port = 19940,
    [long]$Anchor = 100000000,
    [int]$Repetitions = 3,
    [int]$Workers = 1,
    [long]$Affinity = 0,
    [string]$Witness = ""
)

$ErrorActionPreference = "Stop"
if (-not $Witness) {
    $Witness = Join-Path $PSScriptRoot "gpu2d_witness.py"
}
if (-not (Test-Path $Witness)) { throw "witness script not found: $Witness" }

$extra = @("--boot", $Boot)
if ($Rom) { $extra += @("--rom", $Rom) }
if ($Config) { $extra += @("--config", $Config) }

function Invoke-Leg {
    param([string]$Threaded, [bool]$Profiled, [int]$LegPort)
    $env:NDS_GPU2D_THREADED = $Threaded
    $env:NDS_GPU2D_WORKERS = "$Workers"
    if ($Profiled) {
        $env:NDS_PROFILE_GPU = "1"
        $env:NDS_PROFILE_SCHED = "exact"
    } else {
        Remove-Item Env:NDS_PROFILE_GPU -ErrorAction SilentlyContinue
        Remove-Item Env:NDS_PROFILE_SCHED -ErrorAction SilentlyContinue
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $pyArgs = @($Witness, $Exe, $BiosDir, "$LegPort", "$Anchor") + $extra
    $raw = & py -3 @pyArgs 2>&1
    $sw.Stop()
    $json = $null
    try { $json = ($raw -join "`n") | ConvertFrom-Json } catch { }
    [pscustomobject]@{
        Wall = $sw.Elapsed.TotalSeconds
        DisplayMs = if ($json) { $json.sched_display_ns / 1e6 } else { $null }
        DevicesMs = if ($json) { $json.sched_devices_ns / 1e6 } else { $null }
        RoundMs = if ($json) { $json.sched_sampled_round_ns / 1e6 } else { $null }
        ThreadedLines = if ($json) { $json.threaded_lines } else { $null }
        InlineLines = if ($json) { $json.inline_lines } else { $null }
        Vblank9 = if ($json) { $json.vblank9 } else { $null }
        Raw = $raw
    }
}

if ($Affinity -ne 0) {
    # Constrain this PowerShell process; children inherit the mask.
    (Get-Process -Id $PID).ProcessorAffinity = [IntPtr]$Affinity
    Write-Output ("affinity mask 0x{0:X} applied to this process and children" -f $Affinity)
}

$results = @{}
foreach ($profiled in @($false, $true)) {
    $tag = if ($profiled) { "profiled" } else { "plain" }
    foreach ($leg in @("0", "1")) { $results["$tag/$leg"] = @() }
    for ($i = 0; $i -lt $Repetitions; $i++) {
        # Interleaved: alternate legs within each repetition so drift and
        # thermal state hit both legs equally.
        foreach ($leg in @("0", "1")) {
            $r = Invoke-Leg -Threaded $leg -Profiled $profiled `
                -LegPort ($Port + $i * 4 + [int]$leg)
            $results["$tag/$leg"] += $r
            $d = if ($null -ne $r.DisplayMs) { "{0:N1} ms" -f $r.DisplayMs } else { "n/a" }
            Write-Output ("  {0} threaded={1} rep{2}: wall={3:N2}s display={4}" -f $tag, $leg, $i, $r.Wall, $d)
        }
    }
}

Write-Output ""
Write-Output "=== min-of-$Repetitions ==="
$rows = @()
foreach ($key in @("plain/0", "plain/1", "profiled/0", "profiled/1")) {
    $set = $results[$key]
    if (-not $set) { continue }
    $rows += [pscustomobject]@{
        Leg = $key
        WallS = [math]::Round(($set | Measure-Object Wall -Minimum).Minimum, 3)
        DisplayMs = if ($null -ne $set[0].DisplayMs) {
            [math]::Round(($set | Measure-Object DisplayMs -Minimum).Minimum, 1)
        } else { $null }
        DevicesMs = if ($null -ne $set[0].DevicesMs) {
            [math]::Round(($set | Measure-Object DevicesMs -Minimum).Minimum, 1)
        } else { $null }
        RoundMs = if ($null -ne $set[0].RoundMs) {
            [math]::Round(($set | Measure-Object RoundMs -Minimum).Minimum, 1)
        } else { $null }
        Threaded = $set[-1].ThreadedLines
        Inline = $set[-1].InlineLines
        Vblank9 = $set[-1].Vblank9
    }
}
$rows | Format-Table -AutoSize
