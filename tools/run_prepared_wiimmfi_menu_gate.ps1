#
# Run from the Mario Kart DS game worktree after preparing save/firmware pairs:
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\run_prepared_wiimmfi_menu_gate.ps1
#
# This launches one hidden runner with an existing cartridge save, boots the
# pristine firmware menu to the regression gate, installs the matching
# prepared firmware image with firmware_replace, and drives MKDS to the
# authenticated "Choose the conditions for this match" menu.
#
[CmdletBinding()]
param(
    [string] $GameRoot = 'F:\Projects\ndsrecomp\mariokartdsrecomp',
    [string] $BuildDir = 'ndsrecomp\runner\build-mkds-pcap',
    [string] $Rom = 'Mario Kart DS.nds',
    [string] $SavePrefix = 'mkds_instance',
    [string] $FirmwarePrefix = 'mkds_instance',
    [int] $Port = 19890,
    [ValidateSet('slirp', 'pcap')]
    [string] $NetworkBackend = 'pcap',
    [string] $PcapAdapter = '',
    [string] $WfcProvider = 'wiimmfi',
    [int] $InstanceIndex = 0,
    [string] $PythonExe = '.venv\Scripts\python.exe',
    [double] $StartupTimeoutSeconds = 30,
    [int] $Attempts = 2,
    [string] $OutDir = ''
)

$ErrorActionPreference = 'Stop'

$GameRoot = [System.IO.Path]::GetFullPath($GameRoot)
Set-Location $GameRoot

$RunnerExe = [System.IO.Path]::GetFullPath((Join-Path $GameRoot (Join-Path $BuildDir 'nds_runner.exe')))
$PythonPath = [System.IO.Path]::GetFullPath((Join-Path $GameRoot $PythonExe))
$Scenario = [System.IO.Path]::GetFullPath((Join-Path $GameRoot 'ndsrecomp\oracle\mkds_online_menus.py'))
$RomPath = [System.IO.Path]::GetFullPath((Join-Path $GameRoot $Rom))
$SaveName = "${SavePrefix}${InstanceIndex}.sav"
$FirmwareName = "${FirmwarePrefix}${InstanceIndex}.firmware.bin"
$SavePath = [System.IO.Path]::GetFullPath((Join-Path $GameRoot $SaveName))
$FirmwarePath = [System.IO.Path]::GetFullPath((Join-Path $GameRoot $FirmwareName))

if (-not (Test-Path -LiteralPath $RunnerExe)) { throw "runner not found: $RunnerExe" }
if (-not (Test-Path -LiteralPath $PythonPath)) { throw "python not found: $PythonPath" }
if (-not (Test-Path -LiteralPath $Scenario)) { throw "scenario not found: $Scenario" }
if (-not (Test-Path -LiteralPath $RomPath)) { throw "ROM not found: $RomPath" }
if (-not (Test-Path -LiteralPath $SavePath)) { throw "prepared save not found: $SavePath" }
if (-not (Test-Path -LiteralPath $FirmwarePath)) { throw "prepared firmware not found: $FirmwarePath" }
if ((Get-Item -LiteralPath $SavePath).Length -ne 262144) { throw "prepared save must be 262144 bytes: $SavePath" }
if ((Get-Item -LiteralPath $FirmwarePath).Length -ne 262144) { throw "prepared firmware must be 262144 bytes: $FirmwarePath" }
if ($Port -lt 1 -or $Port -gt 65535) { throw "-Port must be in 1..65535" }
if ($InstanceIndex -lt 0 -or $InstanceIndex -gt 255) { throw "-InstanceIndex must be in 0..255" }
if ($StartupTimeoutSeconds -lt 0) { throw "-StartupTimeoutSeconds must be non-negative" }
if ($Attempts -lt 1) { throw "-Attempts must be positive" }

function Test-DebugPort {
    param([int] $TestPort)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect('127.0.0.1', $TestPort, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne(500, $false)) { return $false }
        $client.EndConnect($async)
        return $true
    }
    catch { return $false }
    finally { $client.Close() }
}

function Wait-DebugPort {
    param([int] $WaitPort, [double] $TimeoutSeconds)
    if ($TimeoutSeconds -eq 0) { return (Test-DebugPort -TestPort $WaitPort) }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        if (Test-DebugPort -TestPort $WaitPort) { return $true }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $false
}

function Get-LogTail {
    param([string] $Path, [int] $Lines = 100)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return ((Get-Content -LiteralPath $Path -Tail $Lines) -join [Environment]::NewLine)
}

function Assert-PreparedEvidence {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "scenario completed but evidence JSON was not written: $Path"
    }
    $report = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    if (-not $report.prepared_profile) {
        throw "evidence JSON does not record prepared_profile=true: $Path"
    }
    if (-not $report.stopped_at_match_setup) {
        throw "evidence JSON did not record stopped_at_match_setup=true: $Path"
    }
    $steps = @($report.steps | ForEach-Object { $_.name })
    foreach ($required in @(
        'prepared_firmware_installed',
        'wfc_connection_menu_prepared',
        'wfc_match_setup_screen'
    )) {
        if ($steps -notcontains $required) {
            throw "evidence JSON is missing required step ${required}: $Path"
        }
    }
    if ($steps -contains 'wfc_wifi_id_update_warning') {
        throw "prepared profile evidence hit Wi-Fi ID mismatch warning: $Path"
    }
    $match = $report.net_evidence.D_match_setup_screen
    if (-not $match) {
        throw "evidence JSON is missing D_match_setup_screen network evidence: $Path"
    }
    if (@($match.kinds.backend_error).Count -gt 0) {
        throw "D_match_setup_screen evidence contains backend_error event(s)"
    }
    if (@($match.kinds.backend_drop).Count -gt 0) {
        throw "D_match_setup_screen evidence contains backend_drop event(s)"
    }
    $setupStep = $report.steps |
        Where-Object { $_.name -eq 'wfc_match_setup_screen' } |
        Select-Object -Last 1
    Write-Host (
        "prepared evidence OK: match setup vblank9={0}, dns={1}/{2}, tcp_open={3}, udp={4}, tls={5}" -f `
        $setupStep.vblank9,
        @($match.kinds.dns_query).Count,
        @($match.kinds.dns_response).Count,
        @($match.kinds.tcp_open).Count,
        @($match.kinds.udp_packet).Count,
        @($match.kinds.tls_record).Count
    ) -ForegroundColor Green
}

if (Test-DebugPort -TestPort $Port) {
    throw "debug port already accepts connections: $Port"
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$GateOutDir = if ($OutDir) {
    if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $GameRoot $OutDir }
} else {
    Join-Path $GameRoot (Join-Path 'generated\captures' "wiimmfi-prepared-menu-gate-$stamp")
}
New-Item -ItemType Directory -Force -Path $GateOutDir | Out-Null

$argv = @('ndsrecomp\bios', '--serve', '--port', "$Port",
          '--config', 'game.toml', '--rom', "`"$Rom`"",
          '--save-path', $SaveName, '--startup-mode', 'manual',
          '--network', 'on', '--network-backend', $NetworkBackend,
          '--wfc', 'on', '--wfc-provider', $WfcProvider,
          '--instance-index', "$InstanceIndex")
if ($PcapAdapter) { $argv += @('--pcap-adapter', $PcapAdapter) }

$completed = $false
$lastError = $null
for ($attempt = 1; $attempt -le $Attempts; ++$attempt) {
    $AttemptOutDir = if ($Attempts -eq 1) { $GateOutDir } else { Join-Path $GateOutDir ("attempt_{0:D3}" -f $attempt) }
    New-Item -ItemType Directory -Force -Path $AttemptOutDir | Out-Null
    $ShotsDir = Join-Path $AttemptOutDir 'shots'
    $EvidenceJson = Join-Path $AttemptOutDir 'evidence.json'
    $RunnerOut = Join-Path $AttemptOutDir 'runner.out.log'
    $RunnerErr = Join-Path $AttemptOutDir 'runner.err.log'

    Write-Host (
        "launching prepared Wiimmfi menu gate on port {0} attempt {1}/{2}" -f `
        $Port, $attempt, $Attempts
    ) -ForegroundColor Cyan
    $runner = $null
    try {
        $runner = Start-Process -FilePath $RunnerExe -WorkingDirectory $GameRoot `
            -ArgumentList $argv -PassThru `
            -RedirectStandardOutput $RunnerOut `
            -RedirectStandardError $RunnerErr `
            -WindowStyle Hidden
        if (-not (Wait-DebugPort -WaitPort $Port -TimeoutSeconds $StartupTimeoutSeconds)) {
            $exitText = if ($runner.HasExited) { " exited with code $($runner.ExitCode)" } else { '' }
            throw (
                "runner did not open debug port $Port$exitText" +
                [Environment]::NewLine + (Get-LogTail -Path $RunnerErr)
            )
        }
        & $PythonPath $Scenario `
            --port $Port `
            --shots-dir $ShotsDir `
            --prepared-profile `
            --prepared-firmware $FirmwarePath `
            --evidence-json $EvidenceJson
        if ($LASTEXITCODE -ne 0) {
            throw "mkds_online_menus.py prepared profile failed with exit code $LASTEXITCODE"
        }
        Assert-PreparedEvidence -Path $EvidenceJson
        $completed = $true
        Write-Host "prepared Wiimmfi menu gate complete: $EvidenceJson" -ForegroundColor Green
        break
    }
    catch {
        $lastError = $_
        Write-Warning "attempt $attempt failed: $($_.Exception.Message)"
        if ($attempt -lt $Attempts) {
            Write-Host "retrying prepared Wiimmfi menu gate..." -ForegroundColor Yellow
        }
    }
    finally {
        if ($runner -and -not $runner.HasExited) {
            Stop-Process -Id $runner.Id -Force
            Wait-Process -Id $runner.Id -Timeout 10 -ErrorAction SilentlyContinue
        }
    }
}

if (-not $completed) {
    throw "prepared Wiimmfi menu gate failed after $Attempts attempt(s): $($lastError.Exception.Message)"
}
