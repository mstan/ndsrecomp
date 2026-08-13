# build_mkds_pcap_runner.ps1 -- build the MKDS runner used by M7 Wiimmfi tests.
#
# Run from the Mario Kart DS game worktree:
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\build_mkds_pcap_runner.ps1
#
[CmdletBinding()]
param(
    [string] $GameRoot = (Get-Location).Path,
    [string] $BuildDir = 'ndsrecomp\runner\build-mkds-pcap',
    [string] $TitleBankDir = 'generated\recomp',
    [string] $RomSha1 = '691e00d9a5dd80b04f80cc7559503e8b06848785',
    [string] $CMakeExe = 'C:\msys64\mingw64\bin\cmake.exe',
    [int] $Jobs = 12,
    [switch] $SkipTests
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $CMakeExe)) {
    throw "CMake not found: $CMakeExe"
}
if (-not (Test-Path -LiteralPath $GameRoot)) {
    throw "game root not found: $GameRoot"
}

$runnerSource = Join-Path $GameRoot 'ndsrecomp\runner'
$titleBanks = Join-Path $GameRoot $TitleBankDir
$buildPath = Join-Path $GameRoot $BuildDir

if (-not (Test-Path -LiteralPath $runnerSource)) {
    throw "runner source not found: $runnerSource"
}
if (-not (Test-Path -LiteralPath $titleBanks)) {
    throw "title bank directory not found: $titleBanks"
}
if ($Jobs -lt 1) {
    throw "-Jobs must be positive"
}
if ($RomSha1 -notmatch '^[0-9a-f]{40}$') {
    throw "-RomSha1 must be exactly 40 lowercase hex digits"
}

Write-Host "configuring pcap-enabled MKDS runner..." -ForegroundColor Cyan
& $CMakeExe -G Ninja -S $runnerSource -B $buildPath `
    -DCMAKE_BUILD_TYPE=Release `
    -DNDS_BOOTSTRAP_FIRMWARE=ON `
    -DNDS_ENABLE_PCAP_BACKEND=ON `
    -DNDS_TITLE_BANK_DIR="$titleBanks" `
    -DNDS_TITLE_ROM_SHA1="$RomSha1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "building pcap-enabled MKDS runner..." -ForegroundColor Cyan
& $CMakeExe --build $buildPath -j $Jobs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $SkipTests) {
    Write-Host "running pcap build tests..." -ForegroundColor Cyan
    & (Join-Path (Split-Path -Parent $CMakeExe) 'ctest.exe') `
        --test-dir $buildPath --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "pcap runner ready: $(Join-Path $buildPath 'nds_runner.exe')" `
    -ForegroundColor Green
