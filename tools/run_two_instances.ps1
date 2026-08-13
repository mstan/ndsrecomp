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
# Run from the GAME worktree, after building with NDS_ENABLE_PCAP_BACKEND=ON.
# To initialize the per-instance saves before the owner-driven Friend Roster
# run, use ndsrecomp\tools\prepare_two_instance_saves.ps1.
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\run_two_instances.ps1
#
# (pwsh / PowerShell 7 is NOT installed on this machine -- use powershell.exe.)
#
[CmdletBinding()]
param(
    [string] $GameRoot   = 'F:\Projects\ndsrecomp\mariokartdsrecomp',
    [string] $BuildDir   = '..\ndsrecomp\runner\build-mkds-pcap',
    [string] $Rom        = 'Mario Kart DS.nds',
    [int]    $PortA      = 19860,
    [int]    $PortB      = 19861,
    [ValidateSet('slirp', 'pcap')]
    [string] $NetworkBackend = 'pcap',
    [string] $PcapAdapter = '',
    [string] $WfcProvider = 'wiimmfi',
    [switch] $DriveB
)

$ErrorActionPreference = 'Stop'
Set-Location $GameRoot

$exe = Join-Path $GameRoot (Join-Path $BuildDir 'nds_runner.exe')
if (-not (Test-Path $exe)) { throw "runner not found: $exe" }
if (-not (Test-Path (Join-Path $GameRoot $Rom))) { throw "ROM not found: $Rom" }

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
    $save = "mkds_instance$InstanceIndex.sav"
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

$a = Start-Instance -Label 'A' -Port $PortA -InstanceIndex 0
$b = Start-Instance -Label 'B' -Port $PortB -InstanceIndex 1 -Headless:$DriveB

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
Write-Host 'After the attempt, collect proof from both always-on rings:' -ForegroundColor Cyan
Write-Host '  .venv\Scripts\python.exe ndsrecomp\tools\collect_two_instance_evidence.py'
Write-Host ''
Write-Host ('to stop: Stop-Process -Id {0},{1}' -f $a.Id, $b.Id) -ForegroundColor DarkGray
