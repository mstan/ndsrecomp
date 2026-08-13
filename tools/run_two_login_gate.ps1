# run_two_login_gate.ps1 -- concurrent non-matchmaking Wiimmfi proof.
#
# Launches two hidden MKDS runners with distinct instance indexes and drives
# both through tools/run_wiimmfi_menu_gate.ps1 concurrently. Each child stops at
# the authenticated match setup menu, before Friend Roster or public
# matchmaking. This is a topology/login gate for M7, not the owner-driven race
# acceptance run.
#
# Run from the Mario Kart DS game worktree after building the pcap runner:
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\run_two_login_gate.ps1
#
[CmdletBinding()]
param(
    [string] $GameRoot = 'F:\Projects\ndsrecomp\mariokartdsrecomp',
    [int] $PortA = 19901,
    [int] $PortB = 19902,
    [ValidateSet('slirp', 'pcap')]
    [string] $NetworkBackend = 'pcap',
    [string] $PcapAdapter = '',
    [string] $WfcProvider = 'wiimmfi',
    [int] $Attempts = 2,
    [double] $TimeoutSeconds = 900,
    [string] $OutDir = ''
)

$ErrorActionPreference = 'Stop'

$GameRoot = [System.IO.Path]::GetFullPath($GameRoot)
if (-not (Test-Path -LiteralPath $GameRoot)) { throw "game root not found: $GameRoot" }
if ($PortA -lt 1 -or $PortA -gt 65535) { throw "-PortA must be in 1..65535" }
if ($PortB -lt 1 -or $PortB -gt 65535) { throw "-PortB must be in 1..65535" }
if ($PortA -eq $PortB) { throw "-PortA and -PortB must be different" }
if ($Attempts -lt 1) { throw "-Attempts must be positive" }
if ($TimeoutSeconds -le 0) { throw "-TimeoutSeconds must be positive" }

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$GateScript = Join-Path $ScriptRoot 'run_wiimmfi_menu_gate.ps1'
if (-not (Test-Path -LiteralPath $GateScript)) { throw "gate script not found: $GateScript" }

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

function Get-LogTail {
    param([string] $Path, [int] $Lines = 120)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }
    return ((Get-Content -LiteralPath $Path -Tail $Lines) -join [Environment]::NewLine)
}

function Test-GateEvidence {
    param([string] $InstanceOutDir, [datetime] $Since)

    $evidence = Get-ChildItem -LiteralPath $InstanceOutDir -Recurse `
        -Filter 'evidence.json' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $Since } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $evidence) {
        return [pscustomobject]@{
            Ok = $false
            Path = $null
            Message = (
                "no fresh evidence.json was written under $InstanceOutDir " +
                "after $($Since.ToString('o'))"
            )
        }
    }

    $report = Get-Content -Raw -LiteralPath $evidence.FullName | ConvertFrom-Json
    if (-not $report.stopped_at_match_setup) {
        return [pscustomobject]@{
            Ok = $false
            Path = $evidence.FullName
            Message = "evidence did not stop at match setup"
        }
    }
    if (-not $report.net_evidence.D_match_setup_screen) {
        return [pscustomobject]@{
            Ok = $false
            Path = $evidence.FullName
            Message = "evidence is missing D_match_setup_screen"
        }
    }
    $match = $report.net_evidence.D_match_setup_screen
    $backendDrops = @($match.kinds.backend_drop).Count
    if ($backendDrops -ne 0) {
        return [pscustomobject]@{
            Ok = $false
            Path = $evidence.FullName
            Message = "evidence has $backendDrops backend_drop event(s)"
        }
    }
    return [pscustomobject]@{
        Ok = $true
        Path = $evidence.FullName
        Message = "ok"
    }
}

function Format-IPv4 {
    param([object] $Value)

    if ($null -eq $Value) { return $null }
    $u = [uint32]$Value
    return ("{0}.{1}.{2}.{3}" -f `
        (($u -shr 24) -band 0xFF),
        (($u -shr 16) -band 0xFF),
        (($u -shr 8) -band 0xFF),
        ($u -band 0xFF))
}

function Get-ClientIPv4 {
    param([object] $MatchEvidence)

    foreach ($kind in @('dns_query', 'tcp_open', 'udp_packet')) {
        foreach ($event in @($MatchEvidence.kinds.$kind)) {
            if ($event.direction -eq 0 -and $event.src_ipv4 -and $event.src_ipv4 -ne 0) {
                return (Format-IPv4 $event.src_ipv4)
            }
        }
    }
    return $null
}

function Get-GateSummary {
    param([object] $Child)

    $report = Get-Content -Raw -LiteralPath $Child.Evidence | ConvertFrom-Json
    $match = $report.net_evidence.D_match_setup_screen
    $setupStep = $report.steps |
        Where-Object { $_.name -eq 'wfc_match_setup_screen' } |
        Select-Object -Last 1
    $gpcmTcp = @(
        $match.kinds.tcp_open |
        Where-Object { $_.src_port -eq 29900 -or $_.dst_port -eq 29900 }
    )
    $masterUdp = @(
        $match.kinds.udp_packet |
        Where-Object { $_.src_port -eq 27900 -or $_.dst_port -eq 27900 }
    )
    return [pscustomobject]@{
        label = $Child.Label
        port = $Child.Port
        instance_index = $Child.InstanceIndex
        evidence = $Child.Evidence
        out_dir = $Child.OutDir
        client_ipv4 = Get-ClientIPv4 $match
        wfc_match_setup_vblank9 = $setupStep.vblank9
        dns_query = @($match.kinds.dns_query).Count
        dns_response = @($match.kinds.dns_response).Count
        tcp_open = @($match.kinds.tcp_open).Count
        gpcm_tcp_out = @($gpcmTcp | Where-Object { $_.dst_port -eq 29900 }).Count
        gpcm_tcp_in = @($gpcmTcp | Where-Object { $_.src_port -eq 29900 }).Count
        udp_packet = @($match.kinds.udp_packet).Count
        master_udp_out = @($masterUdp | Where-Object { $_.dst_port -eq 27900 }).Count
        master_udp_in = @($masterUdp | Where-Object { $_.src_port -eq 27900 }).Count
        tls_record = @($match.kinds.tls_record).Count
        backend_drop = @($match.kinds.backend_drop).Count
        backend_error = @($match.kinds.backend_error).Count
    }
}

$busy = @()
foreach ($port in @($PortA, $PortB)) {
    if (Test-DebugPort -TestPort $port) {
        $busy += $port
    }
}
if ($busy.Count -gt 0) {
    throw "debug port(s) already accept connections: $($busy -join ', ')"
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$GateOutDir = if ($OutDir) {
    if ([System.IO.Path]::IsPathRooted($OutDir)) {
        $OutDir
    } else {
        Join-Path $GameRoot $OutDir
    }
} else {
    Join-Path $GameRoot (Join-Path 'generated\captures' "wiimmfi-two-login-gate-$stamp")
}
New-Item -ItemType Directory -Force -Path $GateOutDir | Out-Null

function Start-Gate {
    param([string] $Label, [int] $Port, [int] $InstanceIndex)

    $instanceOut = Join-Path $GateOutDir $Label
    New-Item -ItemType Directory -Force -Path $instanceOut | Out-Null
    $stdout = Join-Path $instanceOut 'gate.out.log'
    $stderr = Join-Path $instanceOut 'gate.err.log'
    $args = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $GateScript,
        '-GameRoot', $GameRoot,
        '-Port', "$Port",
        '-NetworkBackend', $NetworkBackend,
        '-WfcProvider', $WfcProvider,
        '-InstanceIndex', "$InstanceIndex",
        '-Attempts', "$Attempts",
        '-OutDir', $instanceOut
    )
    if ($PcapAdapter) {
        $args += @('-PcapAdapter', $PcapAdapter)
    }

    $startedAt = Get-Date
    $process = Start-Process -FilePath 'powershell.exe' `
        -ArgumentList $args -WorkingDirectory $GameRoot -PassThru `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden
    Write-Host ("started {0}: pid {1} port {2} instance-index {3}" -f `
        $Label, $process.Id, $Port, $InstanceIndex) -ForegroundColor Cyan
    return [pscustomobject]@{
        Label = $Label
        Port = $Port
        InstanceIndex = $InstanceIndex
        StartedAt = $startedAt
        Process = $process
        OutDir = $instanceOut
        Stdout = $stdout
        Stderr = $stderr
    }
}

$children = @(
    (Start-Gate -Label 'A' -Port $PortA -InstanceIndex 0),
    (Start-Gate -Label 'B' -Port $PortB -InstanceIndex 1)
)

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$failed = @()
try {
    foreach ($child in $children) {
        $remaining = [Math]::Max(1, [int]($deadline - (Get-Date)).TotalSeconds)
        $exited = $child.Process.WaitForExit($remaining * 1000)
        if (-not $exited) {
            $failed += "$($child.Label): timed out after $TimeoutSeconds seconds"
            continue
        }
        $child.Process.Refresh()
        $exitCode = $child.Process.ExitCode
        $evidence = Test-GateEvidence -InstanceOutDir $child.OutDir `
            -Since $child.StartedAt
        if ($null -ne $exitCode -and $exitCode -ne 0) {
            $failed += (
                "$($child.Label): gate exited with code $exitCode" +
                [Environment]::NewLine + (Get-LogTail -Path $child.Stderr) +
                [Environment]::NewLine + (Get-LogTail -Path $child.Stdout)
            )
        } elseif (-not $evidence.Ok) {
            $failed += (
                "$($child.Label): $($evidence.Message)" +
                [Environment]::NewLine + (Get-LogTail -Path $child.Stderr) +
                [Environment]::NewLine + (Get-LogTail -Path $child.Stdout)
            )
        } else {
            $child | Add-Member -NotePropertyName Evidence -NotePropertyValue $evidence.Path
        }
    }
}
finally {
    foreach ($child in $children) {
        $live = Get-Process -Id $child.Process.Id -ErrorAction SilentlyContinue
        if ($live) {
            Stop-Process -Id $child.Process.Id -Force
            Wait-Process -Id $child.Process.Id -Timeout 10 -ErrorAction SilentlyContinue
        }
    }
}

if ($failed.Count -gt 0) {
    throw ($failed -join ([Environment]::NewLine + [Environment]::NewLine))
}

$instanceSummaries = @($children | ForEach-Object { Get-GateSummary $_ })
$clientIps = @($instanceSummaries | ForEach-Object { $_.client_ipv4 } | Where-Object { $_ })
if ($clientIps.Count -ne 2 -or @($clientIps | Select-Object -Unique).Count -ne 2) {
    throw "two-login gate did not observe two distinct client IPv4 addresses: $($clientIps -join ', ')"
}

$summaryPath = Join-Path $GateOutDir 'summary.json'
$summary = [pscustomobject]@{
    created_at = $stamp
    game_root = $GameRoot
    network_backend = $NetworkBackend
    wfc_provider = $WfcProvider
    pcap_adapter = $PcapAdapter
    instances = $instanceSummaries
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host 'two-instance Wiimmfi login gate complete' -ForegroundColor Green
foreach ($child in $children) {
    Write-Host "  $($child.Label): $($child.OutDir)"
    Write-Host "     evidence: $($child.Evidence)"
}
Write-Host "  summary: $summaryPath"
