# prepare_two_instance_saves.ps1 -- initialize the two persistent MKDS saves
# used by tools/run_two_instances.ps1 for the owner-driven M7 Friend Roster run.
#
# This drives each instance headlessly through the real first-run setup,
# Nintendo WFC connection settings, NAS login, and online match setup using the
# existing LLE scenario driver. It does not patch the ROM or write game state
# directly; the guest creates the save data through its own code paths.
#
# Run from the GAME worktree:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\prepare_two_instance_saves.ps1
#
# Existing mkds_instance0.sav / mkds_instance1.sav files are left untouched
# unless -Force is passed.
#
[CmdletBinding()]
param(
    [string] $GameRoot = 'F:\Projects\ndsrecomp\mariokartdsrecomp',
    [string] $BuildDir = 'ndsrecomp\runner\build-mkds-pcap',
    [string] $Rom = 'Mario Kart DS.nds',
    [string] $SavePrefix = 'mkds_instance',
    [int] $PortA = 19866,
    [int] $PortB = 19867,
    [ValidateSet('slirp', 'pcap')]
    [string] $NetworkBackend = 'pcap',
    [string] $PcapAdapter = '',
    [string] $WfcProvider = 'wiimmfi',
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

$GameRoot = [System.IO.Path]::GetFullPath($GameRoot)
$FrameworkRoot = [System.IO.Path]::GetFullPath((Join-Path $GameRoot 'ndsrecomp'))
$RunnerExe = [System.IO.Path]::GetFullPath((Join-Path $GameRoot (Join-Path $BuildDir 'nds_runner.exe')))
$PythonExe = [System.IO.Path]::GetFullPath((Join-Path $GameRoot '.venv\Scripts\python.exe'))
$Scenario = Join-Path $FrameworkRoot 'oracle\mkds_wfc_scenario.py'
$RomPath = Join-Path $GameRoot $Rom

if (-not (Test-Path -LiteralPath $RunnerExe)) { throw "runner not found: $RunnerExe" }
if (-not (Test-Path -LiteralPath $PythonExe)) { throw "python not found: $PythonExe" }
if (-not (Test-Path -LiteralPath $Scenario)) { throw "scenario not found: $Scenario" }
if (-not (Test-Path -LiteralPath $RomPath)) { throw "ROM not found: $RomPath" }
if ($PortA -lt 1 -or $PortA -gt 65535) { throw "-PortA must be in 1..65535" }
if ($PortB -lt 1 -or $PortB -gt 65535) { throw "-PortB must be in 1..65535" }
if ($PortA -eq $PortB) { throw "-PortA and -PortB must be different" }

$save0 = Join-Path $GameRoot "${SavePrefix}0.sav"
$save1 = Join-Path $GameRoot "${SavePrefix}1.sav"
foreach ($save in @($save0, $save1)) {
    if ((Test-Path -LiteralPath $save) -and -not $Force) {
        throw "save already exists: $save; pass -Force to replace the two M7 instance saves"
    }
}

if ($Force) {
    foreach ($save in @($save0, $save1)) {
        if (Test-Path -LiteralPath $save) {
            Remove-Item -LiteralPath $save -Force
        }
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$ShotsRoot = Join-Path $GameRoot (Join-Path 'generated\captures' "wiimmfi-save-prep-$stamp")
New-Item -ItemType Directory -Force -Path $ShotsRoot | Out-Null

function Stop-OwnedRunner {
    param([System.Diagnostics.Process] $Process)
    if ($null -eq $Process) { return }
    $live = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if ($live) {
        Stop-Process -Id $Process.Id -Force
        Wait-Process -Id $Process.Id -ErrorAction SilentlyContinue
    }
}

function Invoke-Prep {
    param([string] $Label, [int] $Port, [int] $InstanceIndex)

    $saveName = "${SavePrefix}${InstanceIndex}.sav"
    $savePath = Join-Path $GameRoot $saveName
    $saveParent = Split-Path -Parent $savePath
    if ($saveParent) {
        New-Item -ItemType Directory -Force -Path $saveParent | Out-Null
    }
    $shotsDir = Join-Path $ShotsRoot $Label
    New-Item -ItemType Directory -Force -Path $shotsDir | Out-Null

    $runnerOut = Join-Path $env:TEMP "nds_saveprep_$Label.out.log"
    $runnerErr = Join-Path $env:TEMP "nds_saveprep_$Label.err.log"
    Remove-Item -ErrorAction SilentlyContinue $runnerOut, $runnerErr

    $argv = @('ndsrecomp\bios', '--serve', '--port', "$Port",
              '--config', 'game.toml', '--rom', "`"$Rom`"",
              '--save-path', $saveName,
              '--startup-mode', 'manual',
              '--network', 'on', '--network-backend', $NetworkBackend,
              '--wfc', 'on', '--wfc-provider', $WfcProvider,
              '--instance-index', "$InstanceIndex")
    if ($PcapAdapter) { $argv += @('--pcap-adapter', $PcapAdapter) }

    $runner = $null
    try {
        $runner = Start-Process -FilePath $RunnerExe -WorkingDirectory $GameRoot `
            -ArgumentList $argv -PassThru `
            -RedirectStandardOutput $runnerOut `
            -RedirectStandardError $runnerErr `
            -WindowStyle Hidden
        Write-Host ("started {0}: pid {1} port {2} instance-index {3} save {4}" -f `
            $Label, $runner.Id, $Port, $InstanceIndex, $saveName) -ForegroundColor Green

        & $PythonExe $Scenario --native-only --native-port $Port --shots-dir $shotsDir
        if ($LASTEXITCODE -ne 0) {
            throw "scenario failed for $Label with exit code $LASTEXITCODE"
        }

        if (-not (Test-Path -LiteralPath $savePath)) {
            throw "scenario completed but save was not created: $savePath"
        }
        $saveInfo = Get-Item -LiteralPath $savePath
        if ($saveInfo.Length -le 0) {
            throw "scenario completed but save is empty: $savePath"
        }

        Write-Host ("prepared {0}: {1} ({2} bytes), screenshots {3}" -f `
            $Label, $saveInfo.FullName, $saveInfo.Length, $shotsDir) -ForegroundColor Cyan
    }
    finally {
        Stop-OwnedRunner $runner
    }
}

Invoke-Prep -Label 'A' -Port $PortA -InstanceIndex 0
Invoke-Prep -Label 'B' -Port $PortB -InstanceIndex 1

Write-Host ''
Write-Host 'Prepared two persistent saves for tools/run_two_instances.ps1:' -ForegroundColor Cyan
Write-Host "  $save0"
Write-Host "  $save1"
Write-Host "Screenshots: $ShotsRoot"
