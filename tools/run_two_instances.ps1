# run_two_instances.ps1 -- launch two ndsrecomp instances side by side for the
# Nintendo WFC two-client milestone (beads-yjp.1.8).
#
# Instance A is INTERACTIVE (a real SDL window you play by hand).
# Instance B is INTERACTIVE by default too, so a second person -- or the same
# person on a second controller -- can drive it; pass -DriveB to run it headless
# on the debug protocol instead.
#
# The two instances MUST have distinct console MACs, because Nintendo WFC
# identity and DS friend codes derive from the MAC: two clients presenting the
# same one appear to the service as a single console in two places and will not
# match with each other. Instance A therefore keeps the firmware dump's real MAC
# untouched (LLE-faithful), and instance B runs with --instance-index 1, which
# perturbs the MAC in the in-memory firmware image and recomputes its checksum,
# so the guest still reads it over its own SPI path. No HLE, no ROM patch.
#
#   A: --instance-index 0 (default) -> 00:09:BF:10:C3:87   (your real dump)
#   B: --instance-index 1           -> 00:09:BF:11:07:97
#
# Run from the GAME worktree, after building with NDS_ENABLE_PCAP_BACKEND=ON:
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\build_mkds_pcap_runner.ps1
#
# To initialize the per-instance saves before the owner-driven Friend Roster
# run, use ndsrecomp\tools\prepare_two_instance_saves.ps1.
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\run_two_instances.ps1
#
# Rolling ring/framebuffer evidence starts automatically while driving the
# Friend Roster attempt:
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\run_two_instances.ps1
#
# By default the collector watches for 900 seconds and stops early as soon as
# bidirectional peer UDP is proven. Pass -EvidenceWatchSeconds 0 only for a
# deliberate no-evidence launch.
#
# Before opening interactive windows, the launcher also proves the two
# pcap-backed instances can concurrently reach the authenticated match setup
# menu. Pass -SkipLoginGatePreflight only for a deliberate fast launch with
# weaker acceptance evidence.
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\run_two_instances.ps1
#
# (pwsh / PowerShell 7 is NOT installed on this machine -- use powershell.exe.)
#
[CmdletBinding()]
param(
    [string] $GameRoot   = 'F:\Projects\ndsrecomp\mariokartdsrecomp',
    [string] $BuildDir   = 'ndsrecomp\runner\build-mkds-pcap',
    [string] $Rom        = 'Mario Kart DS.nds',
    [string] $SavePrefix = 'mkds_instance',
    [int]    $PortA      = 19860,
    [int]    $PortB      = 19861,
    [ValidateSet('slirp', 'pcap')]
    [string] $NetworkBackend = 'pcap',
    [string] $PcapAdapter = '',
    [string] $WfcProvider = 'wiimmfi',
    [switch] $DriveB,
    [string] $PythonExe = '.venv\Scripts\python.exe',
    [double] $EvidenceWatchSeconds = 900,
    [double] $EvidenceStartupTimeoutSeconds = 30,
    [double] $EvidenceInterval = 5,
    [int] $EvidenceMaxPerKind = 4096,
    [string] $EvidenceOutDir = '',
    [string[]] $EvidenceStopOnVerdict = @(
        'direct_client_udp_bidirectional_observed'
    ),
    [switch] $SkipSavePreflight,
    [switch] $SkipPortPreflight,
    [switch] $SkipNetworkRuntimePreflight,
    [switch] $RunLoginGatePreflight,
    [switch] $SkipLoginGatePreflight,
    [string] $LoginGateOutDir = '',
    [int] $LoginGateAttempts = 2,
    [double] $LoginGateTimeoutSeconds = 900,
    [double] $LoginGatePortReleaseTimeoutSeconds = 15,
    [switch] $PreflightOnly
)

$ErrorActionPreference = 'Stop'
Set-Location $GameRoot

$exe = Join-Path $GameRoot (Join-Path $BuildDir 'nds_runner.exe')
if (-not (Test-Path $exe)) {
    throw (
        "runner not found: $exe; build it with " +
        "ndsrecomp\tools\build_mkds_pcap_runner.ps1 from the game worktree"
    )
}
if (-not (Test-Path (Join-Path $GameRoot $Rom))) { throw "ROM not found: $Rom" }
if ($PortA -lt 1 -or $PortA -gt 65535) { throw "-PortA must be in 1..65535" }
if ($PortB -lt 1 -or $PortB -gt 65535) { throw "-PortB must be in 1..65535" }
if ($PortA -eq $PortB) { throw "-PortA and -PortB must be different" }
if ($EvidenceWatchSeconds -lt 0) { throw "-EvidenceWatchSeconds must be non-negative" }
if ($EvidenceStartupTimeoutSeconds -lt 0) { throw "-EvidenceStartupTimeoutSeconds must be non-negative" }
if ($EvidenceInterval -le 0) { throw "-EvidenceInterval must be positive" }
if ($EvidenceMaxPerKind -lt 1 -or $EvidenceMaxPerKind -gt 4096) {
    throw "-EvidenceMaxPerKind must be in 1..4096"
}
foreach ($status in $EvidenceStopOnVerdict) {
    if ([string]::IsNullOrWhiteSpace($status)) {
        throw "-EvidenceStopOnVerdict entries must be non-empty"
    }
}
if ($LoginGateAttempts -lt 1) { throw "-LoginGateAttempts must be positive" }
if ($LoginGateTimeoutSeconds -le 0) { throw "-LoginGateTimeoutSeconds must be positive" }
if ($LoginGatePortReleaseTimeoutSeconds -lt 0) {
    throw "-LoginGatePortReleaseTimeoutSeconds must be non-negative"
}

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Resolve-GamePath {
    param([string] $Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return (Join-Path $GameRoot $Path)
}

function Test-DebugPort {
    param([int] $Port)

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
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
    param([int] $Port, [double] $TimeoutSeconds)

    if ($TimeoutSeconds -eq 0) {
        return (Test-DebugPort -Port $Port)
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        if (Test-DebugPort -Port $Port) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)

    return $false
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

function Get-LogTail {
    param([string] $Path, [int] $Lines = 80)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }
    return ((Get-Content -LiteralPath $Path -Tail $Lines) -join [Environment]::NewLine)
}

function Assert-InstanceSaves {
    if ($SkipSavePreflight) {
        Write-Warning "skipping per-instance save preflight"
        return
    }

    $saveHashes = @()
    foreach ($index in 0, 1) {
        $saveName = "${SavePrefix}${index}.sav"
        $savePath = Join-Path $GameRoot $saveName
        if (-not (Test-Path -LiteralPath $savePath)) {
            throw (
                "missing prepared save: $savePath; run " +
                "ndsrecomp\tools\prepare_two_instance_saves.ps1 first, or " +
                "pass -SkipSavePreflight for a deliberate manual setup run"
            )
        }
        $saveInfo = Get-Item -LiteralPath $savePath
        if ($saveInfo.Length -ne 262144) {
            throw (
                "unexpected save size for ${savePath}: $($saveInfo.Length) " +
                "bytes, expected 262144 from prepare_two_instance_saves.ps1"
            )
        }
        $saveHashes += (Get-FileHash -LiteralPath $savePath -Algorithm SHA1).Hash
    }
    if ($saveHashes[0] -eq $saveHashes[1]) {
        throw (
            "per-instance saves are byte-identical; run " +
            "ndsrecomp\tools\prepare_two_instance_saves.ps1 -Force to create " +
            "separate WFC identities before the Friend Roster attempt"
        )
    }

    Write-Host ("save preflight OK: {0}0.sav and {0}1.sav" -f $SavePrefix) `
        -ForegroundColor Green
}

function Assert-DebugPortsFree {
    if ($SkipPortPreflight) {
        Write-Warning "skipping debug-port preflight"
        return
    }

    $busy = @()
    foreach ($port in @($PortA, $PortB)) {
        if (Test-DebugPort -Port $port) {
            $busy += $port
        }
    }
    if ($busy.Count -gt 0) {
        throw (
            "debug port(s) already accept connections: $($busy -join ', '); " +
            "choose different -PortA/-PortB values or pass -SkipPortPreflight " +
            "for an intentional shared-machine run"
        )
    }

    Write-Host ("debug-port preflight OK: {0}, {1}" -f $PortA, $PortB) `
        -ForegroundColor Green
}

function Wait-DebugPortsFree {
    param([int] $PortA, [int] $PortB, [double] $TimeoutSeconds)

    if ($SkipPortPreflight) {
        Write-Warning "skipping debug-port release preflight"
        return
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $busy = @()
        foreach ($port in @($PortA, $PortB)) {
            if (Test-DebugPort -Port $port) {
                $busy += $port
            }
        }
        if ($busy.Count -eq 0) {
            Write-Host ("debug-port release OK: {0}, {1}" -f $PortA, $PortB) `
                -ForegroundColor Green
            return
        }
        if ($TimeoutSeconds -eq 0) { break }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)

    throw (
        "debug port(s) still accept connections after login preflight: " +
        "$($busy -join ', ')"
    )
}

function Assert-EvidenceCollectorPreflight {
    if ($EvidenceWatchSeconds -le 0) { return }

    $pythonPath = Resolve-GamePath $PythonExe
    $collector = Join-Path $GameRoot 'ndsrecomp\tools\collect_two_instance_evidence.py'
    if (-not (Test-Path -LiteralPath $pythonPath)) { throw "python not found: $pythonPath" }
    if (-not (Test-Path -LiteralPath $collector)) { throw "collector not found: $collector" }

    Write-Host "evidence collector preflight OK" -ForegroundColor Green
}

function Assert-NetworkRuntimePreflight {
    if ($SkipNetworkRuntimePreflight) {
        Write-Warning "skipping network runtime preflight"
        return
    }
    if ($NetworkBackend -ne 'pcap') {
        return
    }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $outLog = Join-Path $env:TEMP "nds_m7_pcap_preflight_$stamp.out.log"
    $errLog = Join-Path $env:TEMP "nds_m7_pcap_preflight_$stamp.err.log"
    $argv = @('ndsrecomp\bios', '--serve', '--port', "$PortA",
              '--config', 'game.toml', '--rom', "`"$Rom`"",
              '--no-save', '--startup-mode', 'manual',
              '--network', 'on', '--network-backend', 'pcap',
              '--wfc', 'on', '--wfc-provider', $WfcProvider,
              '--instance-index', '0')
    if ($PcapAdapter) { $argv += @('--pcap-adapter', $PcapAdapter) }

    Write-Host "pcap runtime preflight: launching headless probe on port $PortA" `
        -ForegroundColor Cyan
    $p = Start-Process -FilePath $exe -ArgumentList $argv -PassThru `
            -RedirectStandardOutput $outLog `
            -RedirectStandardError  $errLog `
            -WindowStyle Hidden
    try {
        if (-not (Wait-DebugPort -Port $PortA -TimeoutSeconds $EvidenceStartupTimeoutSeconds)) {
            $exitText = if ($p.HasExited) { " exited with code $($p.ExitCode)" } else { '' }
            throw (
                "pcap preflight runner did not open debug port $PortA$exitText" +
                [Environment]::NewLine + (Get-LogTail -Path $errLog)
            )
        }

        $result = Invoke-DebugCommand -Port $PortA -Command @{
            cmd = 'run_to_event'
            event = 'vblank9'
            count = 120
        }
        if (-not $result.reached -or $result.counts.vblank9 -ne 120) {
            throw (
                "pcap preflight failed to reach vblank9=120: " +
                ($result | ConvertTo-Json -Compress)
            )
        }
        $state = Invoke-DebugCommand -Port $PortA -Command @{ cmd = 'net_state' }
        if ($state.capacity -lt 1) {
            throw "pcap preflight returned invalid net_state"
        }
        Write-Host (
            "pcap runtime preflight OK: vblank9={0} ring_capacity={1}" -f `
            $result.counts.vblank9, $state.capacity
        ) -ForegroundColor Green
    }
    finally {
        if ($p -and -not $p.HasExited) {
            Stop-Process -Id $p.Id -Force
            Wait-Process -Id $p.Id -Timeout 10 -ErrorAction SilentlyContinue
        }
    }
}

function Assert-TwoLoginGatePreflight {
    if ($SkipLoginGatePreflight) {
        Write-Warning "skipping two-login gate preflight"
        return
    }

    $loginGate = Join-Path $ScriptRoot 'run_two_login_gate.ps1'
    if (-not (Test-Path -LiteralPath $loginGate)) {
        throw "two-login gate script not found: $loginGate"
    }

    $outDir = $LoginGateOutDir
    if (-not $outDir) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $outDir = Join-Path $GameRoot (Join-Path 'generated\captures' "m7-login-preflight-$stamp")
    }

    $argv = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $loginGate,
        '-GameRoot', $GameRoot,
        '-PortA', "$PortA",
        '-PortB', "$PortB",
        '-NetworkBackend', $NetworkBackend,
        '-WfcProvider', $WfcProvider,
        '-Attempts', "$LoginGateAttempts",
        '-TimeoutSeconds', "$LoginGateTimeoutSeconds",
        '-OutDir', $outDir
    )
    if ($PcapAdapter) {
        $argv += @('-PcapAdapter', $PcapAdapter)
    }

    Write-Host "two-login preflight: proving concurrent authenticated menus" `
        -ForegroundColor Cyan
    & powershell.exe @argv
    if ($LASTEXITCODE -ne 0) {
        throw "two-login preflight failed with exit code $LASTEXITCODE"
    }

    Wait-DebugPortsFree -PortA $PortA -PortB $PortB `
        -TimeoutSeconds $LoginGatePortReleaseTimeoutSeconds
}

Assert-InstanceSaves
Assert-DebugPortsFree
Assert-EvidenceCollectorPreflight
Assert-NetworkRuntimePreflight
Assert-TwoLoginGatePreflight

if ($PreflightOnly) {
    Write-Host "preflight complete; no instances launched" -ForegroundColor Cyan
    exit 0
}

# Never kill by process name -- other sessions and agents may own runners.
# Only the PIDs this script starts are ours to stop.
$existing = Get-CimInstance Win32_Process -Filter "Name='nds_runner.exe'" |
            Select-Object ProcessId, CommandLine
if ($existing) {
    Write-Host "note: other nds_runner processes are already running; leaving them alone:" -ForegroundColor Yellow
    $existing | ForEach-Object { Write-Host ("  pid {0}" -f $_.ProcessId) }
}

function Start-Instance {
    param([string] $Label, [int] $Port, [int] $InstanceIndex, [switch] $Headless)

    # Each instance MUST get its own cartridge save. Without --save-path the
    # runner derives the save path from the ROM filename (main.cpp:923-930),
    # so both instances -- launched from this one working directory -- opened,
    # read and wrote the SAME "Mario Kart DS.sav".
    #
    # That was measured, not theorised: on 2026-08-11 a shared-save
    # two-instance run had one instance complete WFC login while the other
    # never got past the Wi-Fi Connection Setup menu, and re-running the same
    # test with the saves separated had BOTH instances reach
    # wfc_match_setup_screen with identical counters (vblank9=7600). See
    # beads-yjp.1.15.
    #
    # It also undoes half the point of the per-instance MAC above: MKDS keeps
    # its Nintendo WFC profile -- and the friend roster this script tells you
    # to use -- in the cartridge save, so one shared save means one shared
    # WFC identity no matter how distinct the two MACs are.
    #
    # Note --no-save would also separate them (and is what the scenario
    # driver documents for scripted runs), but it is WRONG here: registering
    # each instance as the other's friend has to persist across the session,
    # and --no-save would discard it.
    $save = "${SavePrefix}${InstanceIndex}.sav"
    $argv = @('ndsrecomp\bios', '--config', 'game.toml', '--rom', "`"$Rom`"",
              '--save-path', $save,
              '--network', 'on', '--network-backend', $NetworkBackend,
              '--wfc', 'on', '--wfc-provider', $WfcProvider,
              '--port', "$Port", '--instance-index', "$InstanceIndex")
    if ($PcapAdapter) { $argv += @('--pcap-adapter', $PcapAdapter) }
    if ($Headless) { $argv += '--serve' } else { $argv += '--interactive' }

    $p = Start-Process -FilePath $exe -ArgumentList $argv -PassThru `
            -RedirectStandardOutput ("$env:TEMP\nds_$Label.out.log") `
            -RedirectStandardError  ("$env:TEMP\nds_$Label.err.log")
    Write-Host ("started {0}: pid {1}  port {2}  instance-index {3}  save {4}  {5}" -f `
        $Label, $p.Id, $Port, $InstanceIndex, $save, $(if ($Headless) { 'headless' } else { 'interactive' })) `
        -ForegroundColor Green
    return $p
}

function Wait-DebugPorts {
    param([int] $PortA, [int] $PortB)

    if ($EvidenceStartupTimeoutSeconds -eq 0) {
        return ((Test-DebugPort -Port $PortA) -and (Test-DebugPort -Port $PortB))
    }

    $deadline = (Get-Date).AddSeconds($EvidenceStartupTimeoutSeconds)
    do {
        if ((Test-DebugPort -Port $PortA) -and (Test-DebugPort -Port $PortB)) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)

    return $false
}

function Start-EvidenceCollector {
    param([int] $PortA, [int] $PortB)

    if ($EvidenceWatchSeconds -le 0) { return $null }

    $pythonPath = Resolve-GamePath $PythonExe
    $collector = Join-Path $GameRoot 'ndsrecomp\tools\collect_two_instance_evidence.py'
    if (-not (Test-Path -LiteralPath $pythonPath)) { throw "python not found: $pythonPath" }
    if (-not (Test-Path -LiteralPath $collector)) { throw "collector not found: $collector" }
    if (-not (Wait-DebugPorts -PortA $PortA -PortB $PortB)) {
        Write-Warning (
            "debug ports $PortA and $PortB did not both accept connections " +
            "within $EvidenceStartupTimeoutSeconds seconds; evidence collector not started"
        )
        return $null
    }

    $outDir = if ($EvidenceOutDir) {
        if ([System.IO.Path]::IsPathRooted($EvidenceOutDir)) {
            $EvidenceOutDir
        } else {
            Join-Path $GameRoot $EvidenceOutDir
        }
    } else {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        Join-Path $GameRoot (Join-Path 'generated\captures' "m7-two-instance-evidence-$stamp")
    }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $collectorOut = Join-Path $outDir 'collector.out.log'
    $collectorErr = Join-Path $outDir 'collector.err.log'
    $argv = @('-B', $collector,
              '--port-a', "$PortA", '--port-b', "$PortB",
              '--watch-seconds', "$EvidenceWatchSeconds",
              '--interval', "$EvidenceInterval",
              '--max-per-kind', "$EvidenceMaxPerKind",
              '--out-dir', $outDir)
    foreach ($status in $EvidenceStopOnVerdict) {
        $argv += @('--stop-on-verdict', $status)
    }
    $p = Start-Process -FilePath $pythonPath -WorkingDirectory $GameRoot `
            -ArgumentList $argv -PassThru `
            -RedirectStandardOutput $collectorOut `
            -RedirectStandardError $collectorErr `
            -WindowStyle Hidden
    Write-Host ("started evidence collector: pid {0}  out {1}" -f $p.Id, $outDir) `
        -ForegroundColor Green
    Write-Host "  stdout: $collectorOut"
    Write-Host "  stderr: $collectorErr"
    return $p
}

$a = Start-Instance -Label 'A' -Port $PortA -InstanceIndex 0
$b = Start-Instance -Label 'B' -Port $PortB -InstanceIndex 1 -Headless:$DriveB
$collector = Start-EvidenceCollector -PortA $PortA -PortB $PortB

Write-Host ''
Write-Host 'Both instances are up. Logs:' -ForegroundColor Cyan
Write-Host "  A: $env:TEMP\nds_A.{out,err}.log   (debug port $PortA)"
Write-Host "  B: $env:TEMP\nds_B.{out,err}.log   (debug port $PortB)"
Write-Host "  network: backend=$NetworkBackend provider=$WfcProvider"
if ($PcapAdapter) { Write-Host "  pcap adapter: $PcapAdapter" }
Write-Host ''
Write-Host 'Suggested route -- use FRIEND ROSTER, not a public search:' -ForegroundColor Cyan
Write-Host '  Each instance now has its own friend code (they derive from the MAC).'
Write-Host '  Register each as the other''s friend, then connect via Friend Roster.'
Write-Host '  That pairs the two instances directly instead of matchmaking with a'
Write-Host '  real stranger who would suffer if we desync.'
Write-Host ''
if ($collector) {
    Write-Host 'Rolling evidence collection is already running.' -ForegroundColor Cyan
} else {
    Write-Host 'During the attempt, collect rolling proof from both always-on rings:' -ForegroundColor Cyan
    Write-Host '  .venv\Scripts\python.exe ndsrecomp\tools\collect_two_instance_evidence.py --watch-seconds 900 --interval 5'
    Write-Host '  Add --stop-on-verdict direct_client_udp_bidirectional_observed to stop once peer UDP is proven.'
    Write-Host 'The launcher normally starts this automatically; this run used -EvidenceWatchSeconds 0 or could not start the collector.'
}
Write-Host 'For a final post-run snapshot, omit --watch-seconds/--interval.'
Write-Host ''
$ids = @($a.Id, $b.Id)
if ($collector) { $ids += $collector.Id }
Write-Host ('to stop: Stop-Process -Id {0}' -f ($ids -join ',')) -ForegroundColor DarkGray
