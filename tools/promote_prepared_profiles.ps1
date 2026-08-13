# promote_prepared_profiles.ps1 -- copy a validated two-instance MKDS WFC
# save/firmware profile pair into the paths used by run_two_instances.ps1.
#
# Run from the Mario Kart DS game worktree. By default this validates the
# source pair through the same prepared Wiimmfi two-login preflight used by the
# owner launcher before copying anything.
#
# Promote the known-good scratch profiles into the default owner-run paths:
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File ndsrecomp\tools\promote_prepared_profiles.ps1 -Force
#
# The tool refuses to replace target saves while live nds_runner processes use
# those save paths. Close old owner windows first, or launch run_two_instances
# directly with -SavePrefix scratch\m7-fwprobe-instance and
# -FirmwarePrefix scratch\m7-fwprobe-instance.
#
[CmdletBinding()]
param(
    [string] $GameRoot = 'F:\Projects\ndsrecomp\mariokartdsrecomp',
    [string] $BuildDir = 'ndsrecomp\runner\build-mkds-pcap',
    [string] $Rom = 'Mario Kart DS.nds',
    [string] $SourceSavePrefix = 'scratch\m7-fwprobe-instance',
    [string] $SourceFirmwarePrefix = 'scratch\m7-fwprobe-instance',
    [string] $TargetSavePrefix = 'mkds_instance',
    [string] $TargetFirmwarePrefix = 'mkds_instance',
    [int] $PortA = 19980,
    [int] $PortB = 19981,
    [ValidateSet('slirp', 'pcap')]
    [string] $NetworkBackend = 'pcap',
    [string] $PcapAdapter = '',
    [string] $WfcProvider = 'wiimmfi',
    [int] $Attempts = 2,
    [string] $OutDir = '',
    [switch] $SkipValidation,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

$GameRoot = [System.IO.Path]::GetFullPath($GameRoot)
Set-Location $GameRoot

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Launcher = Join-Path $ScriptRoot 'run_two_instances.ps1'
if (-not (Test-Path -LiteralPath $Launcher)) {
    throw "run_two_instances.ps1 not found: $Launcher"
}
if ($PortA -lt 1 -or $PortA -gt 65535) { throw "-PortA must be in 1..65535" }
if ($PortB -lt 1 -or $PortB -gt 65535) { throw "-PortB must be in 1..65535" }
if ($PortA -eq $PortB) { throw "-PortA and -PortB must be different" }
if ($Attempts -lt 1) { throw "-Attempts must be positive" }

function Resolve-GamePath {
    param([string] $Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $GameRoot $Path))
}

function Get-PairPaths {
    param([string] $SavePrefix, [string] $FirmwarePrefix)

    return @(
        [pscustomobject]@{
            Index = 0
            Save = Resolve-GamePath "${SavePrefix}0.sav"
            Firmware = Resolve-GamePath "${FirmwarePrefix}0.firmware.bin"
        },
        [pscustomobject]@{
            Index = 1
            Save = Resolve-GamePath "${SavePrefix}1.sav"
            Firmware = Resolve-GamePath "${FirmwarePrefix}1.firmware.bin"
        }
    )
}

function Assert-InputPair {
    param([object[]] $Pairs, [string] $Label)

    $saveHashes = @()
    $firmwareHashes = @()
    foreach ($pair in $Pairs) {
        if (-not (Test-Path -LiteralPath $pair.Save)) {
            throw "$Label save missing: $($pair.Save)"
        }
        if (-not (Test-Path -LiteralPath $pair.Firmware)) {
            throw "$Label firmware missing: $($pair.Firmware)"
        }
        if ((Get-Item -LiteralPath $pair.Save).Length -ne 262144) {
            throw "$Label save must be 262144 bytes: $($pair.Save)"
        }
        if ((Get-Item -LiteralPath $pair.Firmware).Length -ne 262144) {
            throw "$Label firmware must be 262144 bytes: $($pair.Firmware)"
        }
        $saveHashes += (Get-FileHash -LiteralPath $pair.Save -Algorithm SHA1).Hash
        $firmwareHashes += (
            Get-FileHash -LiteralPath $pair.Firmware -Algorithm SHA1
        ).Hash
    }
    if ($saveHashes[0] -eq $saveHashes[1]) {
        throw "$Label saves are byte-identical"
    }
    if ($firmwareHashes[0] -eq $firmwareHashes[1]) {
        throw "$Label firmware images are byte-identical"
    }
}

function Assert-TargetsWritable {
    param([object[]] $Pairs)

    $targetNames = @()
    foreach ($pair in $Pairs) {
        $targetNames += [System.IO.Path]::GetFileName($pair.Save)
        $targetNames += [System.IO.Path]::GetFileName($pair.Firmware)
        foreach ($path in @($pair.Save, $pair.Firmware)) {
            if ((Test-Path -LiteralPath $path) -and -not $Force) {
                throw "target exists: $path; pass -Force to replace it"
            }
        }
    }

    $live = Get-CimInstance Win32_Process -Filter "Name='nds_runner.exe'" |
        Where-Object {
            $cmd = $_.CommandLine
            foreach ($name in $targetNames) {
                if ($cmd -like "*--save-path $name*") { return $true }
            }
            return $false
        }
    if ($live) {
        $ids = @($live | ForEach-Object { $_.ProcessId }) -join ', '
        throw (
            "refusing to replace target profile files while live runner(s) " +
            "use matching save paths: $ids"
        )
    }
}

function Invoke-Validation {
    if ($SkipValidation) {
        Write-Warning "skipping prepared-profile Wiimmfi validation"
        return
    }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $validationOut = if ($OutDir) {
        if ([System.IO.Path]::IsPathRooted($OutDir)) {
            $OutDir
        } else {
            Join-Path $GameRoot $OutDir
        }
    } else {
        Join-Path $GameRoot (Join-Path 'generated\captures' "promote-prepared-profiles-$stamp")
    }

    $argv = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Launcher,
        '-GameRoot', $GameRoot,
        '-BuildDir', $BuildDir,
        '-Rom', $Rom,
        '-SavePrefix', $SourceSavePrefix,
        '-FirmwarePrefix', $SourceFirmwarePrefix,
        '-PortA', "$PortA",
        '-PortB', "$PortB",
        '-NetworkBackend', $NetworkBackend,
        '-WfcProvider', $WfcProvider,
        '-PreflightOnly',
        '-LoginGateAttempts', "$Attempts",
        '-LoginGateOutDir', $validationOut
    )
    if ($PcapAdapter) {
        $argv += @('-PcapAdapter', $PcapAdapter)
    }

    Write-Host "validating source prepared profiles before promotion" `
        -ForegroundColor Cyan
    & powershell.exe @argv
    if ($LASTEXITCODE -ne 0) {
        throw "prepared profile validation failed with exit code $LASTEXITCODE"
    }
}

function Copy-Pair {
    param([object[]] $SourcePairs, [object[]] $TargetPairs)

    for ($i = 0; $i -lt $SourcePairs.Count; ++$i) {
        foreach ($kind in @('Save', 'Firmware')) {
            $source = $SourcePairs[$i].$kind
            $target = $TargetPairs[$i].$kind
            $parent = Split-Path -Parent $target
            if ($parent) {
                New-Item -ItemType Directory -Force -Path $parent | Out-Null
            }
            Copy-Item -LiteralPath $source -Destination $target -Force
        }
    }
}

$sourcePairs = Get-PairPaths -SavePrefix $SourceSavePrefix `
    -FirmwarePrefix $SourceFirmwarePrefix
$targetPairs = Get-PairPaths -SavePrefix $TargetSavePrefix `
    -FirmwarePrefix $TargetFirmwarePrefix

Assert-InputPair -Pairs $sourcePairs -Label 'source'
Assert-TargetsWritable -Pairs $targetPairs
Invoke-Validation
Copy-Pair -SourcePairs $sourcePairs -TargetPairs $targetPairs
Assert-InputPair -Pairs $targetPairs -Label 'target'

Write-Host 'prepared profile promotion complete' -ForegroundColor Green
for ($i = 0; $i -lt $targetPairs.Count; ++$i) {
    $saveHash = (Get-FileHash -LiteralPath $targetPairs[$i].Save -Algorithm SHA1).Hash
    $firmwareHash = (
        Get-FileHash -LiteralPath $targetPairs[$i].Firmware -Algorithm SHA1
    ).Hash
    Write-Host ("  {0}: {1} {2}" -f $i, $targetPairs[$i].Save, $saveHash)
    Write-Host ("  {0}: {1} {2}" -f $i, $targetPairs[$i].Firmware, $firmwareHash)
}
