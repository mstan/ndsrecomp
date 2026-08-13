# run_wiimmfi_menu_gate.ps1 -- non-public-matchmaking Wiimmfi menu proof.
#
# Run from the Mario Kart DS game worktree after building the pcap runner:
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\run_wiimmfi_menu_gate.ps1
#
# The script launches one hidden runner, drives the real LLE UI through Wi-Fi
# setup, connection test, NAS/GameSpy login, and stops at the authenticated
# "Choose the conditions for this match" menu before any public search starts.
#
[CmdletBinding()]
param(
    [string] $GameRoot = 'F:\Projects\ndsrecomp\mariokartdsrecomp',
    [string] $BuildDir = '..\ndsrecomp\runner\build-mkds-pcap',
    [string] $Rom = 'Mario Kart DS.nds',
    [int] $Port = 19890,
    [ValidateSet('slirp', 'pcap')]
    [string] $NetworkBackend = 'pcap',
    [string] $PcapAdapter = '',
    [string] $WfcProvider = 'wiimmfi',
    [string] $PythonExe = '.venv\Scripts\python.exe',
    [double] $StartupTimeoutSeconds = 30,
    [string] $OutDir = ''
)

$ErrorActionPreference = 'Stop'

$GameRoot = [System.IO.Path]::GetFullPath($GameRoot)
Set-Location $GameRoot

$RunnerExe = [System.IO.Path]::GetFullPath((Join-Path $GameRoot (Join-Path $BuildDir 'nds_runner.exe')))
$PythonPath = [System.IO.Path]::GetFullPath((Join-Path $GameRoot $PythonExe))
$Scenario = [System.IO.Path]::GetFullPath((Join-Path $GameRoot 'ndsrecomp\oracle\mkds_online_menus.py'))
$RomPath = [System.IO.Path]::GetFullPath((Join-Path $GameRoot $Rom))

if (-not (Test-Path -LiteralPath $RunnerExe)) {
    throw (
        "runner not found: $RunnerExe; build it with " +
        "ndsrecomp\tools\build_mkds_pcap_runner.ps1 from the game worktree"
    )
}
if (-not (Test-Path -LiteralPath $PythonPath)) { throw "python not found: $PythonPath" }
if (-not (Test-Path -LiteralPath $Scenario)) { throw "scenario not found: $Scenario" }
if (-not (Test-Path -LiteralPath $RomPath)) { throw "ROM not found: $RomPath" }
if ($Port -lt 1 -or $Port -gt 65535) { throw "-Port must be in 1..65535" }
if ($StartupTimeoutSeconds -lt 0) { throw "-StartupTimeoutSeconds must be non-negative" }

function Test-DebugPort {
    param([int] $TestPort)

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect('127.0.0.1', $TestPort, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne(500, $false)) {
            return $false
        }
        $client.EndConnect($async)
        return $true
    }
    catch {
        return $false
    }
    finally {
        $client.Close()
    }
}

function Wait-DebugPort {
    param([int] $WaitPort, [double] $TimeoutSeconds)

    if ($TimeoutSeconds -eq 0) {
        return (Test-DebugPort -TestPort $WaitPort)
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        if (Test-DebugPort -TestPort $WaitPort) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)

    return $false
}

function Get-LogTail {
    param([string] $Path, [int] $Lines = 80)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }
    return ((Get-Content -LiteralPath $Path -Tail $Lines) -join [Environment]::NewLine)
}

function Assert-WiimmfiMenuEvidence {
    param([string] $Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "scenario completed but evidence JSON was not written: $Path"
    }

    $report = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    if (-not $report.stopped_at_match_setup) {
        throw "evidence JSON did not record stopped_at_match_setup=true: $Path"
    }

    $steps = @($report.steps | ForEach-Object { $_.name })
    foreach ($required in @(
        'connection_test_settled',
        'wfc_login_settled',
        'wfc_login_next',
        'wfc_match_setup_screen'
    )) {
        if ($steps -notcontains $required) {
            throw "evidence JSON is missing required step ${required}: $Path"
        }
    }

    $matchEvidence = $report.net_evidence.D_match_setup_screen
    if (-not $matchEvidence) {
        throw "evidence JSON is missing D_match_setup_screen network evidence: $Path"
    }

    $requiredKinds = @{
        dns_query    = 1
        dns_response = 1
        tcp_open     = 1
        tls_record   = 1
        udp_packet   = 1
    }
    foreach ($kind in $requiredKinds.Keys) {
        $events = @($matchEvidence.kinds.$kind)
        if ($events.Count -lt $requiredKinds[$kind]) {
            throw (
                "D_match_setup_screen evidence has $($events.Count) " +
                "$kind event(s), expected at least $($requiredKinds[$kind])"
            )
        }
    }

    $backendErrors = @($matchEvidence.kinds.backend_error)
    if ($backendErrors.Count -gt 0) {
        throw "D_match_setup_screen evidence contains backend_error event(s): $($backendErrors.Count)"
    }

    $setupStep = $report.steps |
        Where-Object { $_.name -eq 'wfc_match_setup_screen' } |
        Select-Object -Last 1
    $summary = (
        "evidence OK: wfc_match_setup_screen vblank9={0}, dns={1}/{2}, " +
        "tcp_open={3}, udp={4}, tls={5}"
    ) -f (
        $setupStep.vblank9,
        @($matchEvidence.kinds.dns_query).Count,
        @($matchEvidence.kinds.dns_response).Count,
        @($matchEvidence.kinds.tcp_open).Count,
        @($matchEvidence.kinds.udp_packet).Count,
        @($matchEvidence.kinds.tls_record).Count
    )
    Write-Host $summary -ForegroundColor Green
}

if (Test-DebugPort -TestPort $Port) {
    throw "debug port already accepts connections: $Port"
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$GateOutDir = if ($OutDir) {
    if ([System.IO.Path]::IsPathRooted($OutDir)) {
        $OutDir
    } else {
        Join-Path $GameRoot $OutDir
    }
} else {
    Join-Path $GameRoot (Join-Path 'generated\captures' "wiimmfi-menu-gate-$stamp")
}
New-Item -ItemType Directory -Force -Path $GateOutDir | Out-Null

$ShotsDir = Join-Path $GateOutDir 'shots'
$EvidenceJson = Join-Path $GateOutDir 'evidence.json'
$RunnerOut = Join-Path $GateOutDir 'runner.out.log'
$RunnerErr = Join-Path $GateOutDir 'runner.err.log'

$argv = @('ndsrecomp\bios', '--serve', '--port', "$Port",
          '--config', 'game.toml', '--rom', "`"$Rom`"",
          '--no-save', '--startup-mode', 'manual',
          '--network', 'on', '--network-backend', $NetworkBackend,
          '--wfc', 'on', '--wfc-provider', $WfcProvider)
if ($PcapAdapter) { $argv += @('--pcap-adapter', $PcapAdapter) }

Write-Host "launching Wiimmfi menu gate runner on port $Port" -ForegroundColor Cyan
$runner = Start-Process -FilePath $RunnerExe -WorkingDirectory $GameRoot `
    -ArgumentList $argv -PassThru `
    -RedirectStandardOutput $RunnerOut `
    -RedirectStandardError $RunnerErr `
    -WindowStyle Hidden

try {
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
        --evidence-json $EvidenceJson `
        --stop-at-match-setup
    if ($LASTEXITCODE -ne 0) {
        throw "mkds_online_menus.py failed with exit code $LASTEXITCODE"
    }

    Assert-WiimmfiMenuEvidence -Path $EvidenceJson

    Write-Host "Wiimmfi menu gate complete" -ForegroundColor Green
    Write-Host "  evidence: $EvidenceJson"
    Write-Host "  screenshots: $ShotsDir"
}
finally {
    if ($runner -and -not $runner.HasExited) {
        Stop-Process -Id $runner.Id -Force
        Wait-Process -Id $runner.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
}
