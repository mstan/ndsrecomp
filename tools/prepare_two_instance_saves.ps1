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
# Existing mkds_instance0.sav / mkds_instance1.sav and matching
# mkds_instance0.firmware.bin / mkds_instance1.firmware.bin files are left
# untouched unless -Force is passed.
#
[CmdletBinding()]
param(
    [string] $GameRoot = 'F:\Projects\ndsrecomp\mariokartdsrecomp',
    [string] $BuildDir = 'ndsrecomp\runner\build-mkds-pcap',
    [string] $Rom = 'Mario Kart DS.nds',
    [string] $SavePrefix = 'mkds_instance',
    [string] $FirmwarePrefix = 'mkds_instance',
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
$firmware0 = Join-Path $GameRoot "${FirmwarePrefix}0.firmware.bin"
$firmware1 = Join-Path $GameRoot "${FirmwarePrefix}1.firmware.bin"
foreach ($path in @($save0, $save1, $firmware0, $firmware1)) {
    if ((Test-Path -LiteralPath $path) -and -not $Force) {
        throw "prepared file already exists: $path; pass -Force to replace the two M7 instance saves and firmware images"
    }
}

if ($Force) {
    foreach ($path in @($save0, $save1, $firmware0, $firmware1)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
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

function Invoke-DebugCommand {
    param([int] $Port, [hashtable] $Command, [int] $TimeoutMilliseconds = 30000)

    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = $TimeoutMilliseconds
    $client.SendTimeout = $TimeoutMilliseconds
    try {
        $client.Connect('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.NewLine = "`n"
        $writer.AutoFlush = $true
        $reader = New-Object System.IO.StreamReader($stream)
        $writer.WriteLine(($Command | ConvertTo-Json -Compress))
        $line = $reader.ReadLine()
        if (-not $line) {
            throw "debug server closed the connection"
        }
        $response = $line | ConvertFrom-Json
        if ($response.error) {
            throw "$($Command.cmd): $($response.error)"
        }
        return $response
    }
    finally {
        $client.Close()
    }
}

function Convert-HexToBytes {
    param([string] $Hex)

    if (($Hex.Length % 2) -ne 0) {
        throw "firmware_dump returned an odd-length hex string"
    }
    $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; ++$i) {
        $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
    }
    return $bytes
}

function Invoke-Prep {
    param([string] $Label, [int] $Port, [int] $InstanceIndex)

    $saveName = "${SavePrefix}${InstanceIndex}.sav"
    $firmwareName = "${FirmwarePrefix}${InstanceIndex}.firmware.bin"
    $savePath = Join-Path $GameRoot $saveName
    $firmwarePath = Join-Path $GameRoot $firmwareName
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
        $firmwareDump = Invoke-DebugCommand -Port $Port -Command @{ cmd = 'firmware_dump' }
        if ($firmwareDump.size -ne 262144) {
            throw "firmware_dump returned $($firmwareDump.size) bytes, expected 262144"
        }
        [System.IO.File]::WriteAllBytes(
            $firmwarePath,
            [byte[]](Convert-HexToBytes -Hex ([string] $firmwareDump.hex))
        )
        $saveInfo = Get-Item -LiteralPath $savePath
        if ($saveInfo.Length -le 0) {
            throw "scenario completed but save is empty: $savePath"
        }
        $firmwareInfo = Get-Item -LiteralPath $firmwarePath
        if ($firmwareInfo.Length -ne 262144) {
            throw "unexpected firmware image size for ${firmwarePath}: $($firmwareInfo.Length)"
        }

        Write-Host ("prepared {0}: {1} ({2} bytes), {3} ({4} bytes), screenshots {5}" -f `
            $Label, $saveInfo.FullName, $saveInfo.Length,
            $firmwareInfo.FullName, $firmwareInfo.Length, $shotsDir) -ForegroundColor Cyan
    }
    finally {
        Stop-OwnedRunner $runner
    }
}

Invoke-Prep -Label 'A' -Port $PortA -InstanceIndex 0
Invoke-Prep -Label 'B' -Port $PortB -InstanceIndex 1

$hash0 = (Get-FileHash -LiteralPath $save0 -Algorithm SHA1).Hash
$hash1 = (Get-FileHash -LiteralPath $save1 -Algorithm SHA1).Hash
$firmwareHash0 = (Get-FileHash -LiteralPath $firmware0 -Algorithm SHA1).Hash
$firmwareHash1 = (Get-FileHash -LiteralPath $firmware1 -Algorithm SHA1).Hash
if ($hash0 -eq $hash1) {
    throw "prepared instance saves are byte-identical; WFC identities did not diverge"
}
if ($firmwareHash0 -eq $firmwareHash1) {
    throw "prepared firmware images are byte-identical; per-instance identities did not diverge"
}

Write-Host ''
Write-Host 'Prepared two persistent save/firmware pairs for tools/run_two_instances.ps1:' -ForegroundColor Cyan
Write-Host "  $save0"
Write-Host "  $save1"
Write-Host "  $firmware0"
Write-Host "  $firmware1"
Write-Host ("  hashes: {0} / {1}" -f $hash0.Substring(0, 12), $hash1.Substring(0, 12))
Write-Host ("  firmware hashes: {0} / {1}" -f $firmwareHash0.Substring(0, 12), $firmwareHash1.Substring(0, 12))
Write-Host "Screenshots: $ShotsRoot"
