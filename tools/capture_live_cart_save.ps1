param(
    [Parameter(Mandatory = $true)]
    [int]$ProcessId,

    [Parameter(Mandatory = $true)]
    [string]$Output,

    [switch]$Force,

    [string]$ToolchainBin = "C:\msys64\mingw64\bin"
)

$ErrorActionPreference = "Stop"

if (-not ("NativeProcessMemory" -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class NativeProcessMemory {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(
        uint desiredAccess, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(
        IntPtr process, UIntPtr address, byte[] buffer, UIntPtr size,
        out UIntPtr bytesRead);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
"@
}

$process = Get-Process -Id $ProcessId -ErrorAction Stop
$executable = $process.Path
if (-not $executable) {
    throw "Cannot resolve the executable for PID $ProcessId"
}

$nm = Join-Path $ToolchainBin "nm.exe"
$objdump = Join-Path $ToolchainBin "objdump.exe"
if (-not (Test-Path -LiteralPath $nm) -or
    -not (Test-Path -LiteralPath $objdump)) {
    throw "MinGW nm.exe/objdump.exe not found under $ToolchainBin"
}
$env:PATH = "$ToolchainBin;$env:PATH"

$symbolLine = & $nm -C --defined-only $executable |
    Select-String -Pattern "\(anonymous namespace\)::g_cart_sram$" |
    Select-Object -First 1
if (-not $symbolLine) {
    throw "The runner does not contain the g_cart_sram debug symbol"
}
$symbolText = $symbolLine.Line.Trim()
if ($symbolText -notmatch "^([0-9A-Fa-f]+)\s") {
    throw "Cannot parse g_cart_sram symbol address: $symbolText"
}
$symbolAddress = [Convert]::ToUInt64($Matches[1], 16)

$imageBaseLine = & $objdump -p $executable |
    Select-String -Pattern "^\s*ImageBase\s+([0-9A-Fa-f]+)" |
    Select-Object -First 1
if (-not $imageBaseLine -or
    $imageBaseLine.Line -notmatch "^\s*ImageBase\s+([0-9A-Fa-f]+)") {
    throw "Cannot parse the runner's preferred image base"
}
$preferredBase = [Convert]::ToUInt64($Matches[1], 16)
$runtimeBase = [uint64]$process.MainModule.BaseAddress.ToInt64()
$vectorAddress = $runtimeBase + ($symbolAddress - $preferredBase)

$ProcessVmRead = [uint32]0x0010
$ProcessQueryInformation = [uint32]0x0400
$handle = [NativeProcessMemory]::OpenProcess(
    $ProcessVmRead -bor $ProcessQueryInformation, $false, $ProcessId)
if ($handle -eq [IntPtr]::Zero) {
    throw "OpenProcess failed with Windows error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}

try {
    $vector = New-Object byte[] 24
    [UIntPtr]$bytesRead = [UIntPtr]::Zero
    $ok = [NativeProcessMemory]::ReadProcessMemory(
        $handle, [UIntPtr]::new($vectorAddress), $vector, [UIntPtr]::new(24),
        [ref]$bytesRead)
    if (-not $ok -or $bytesRead.ToUInt64() -ne 24) {
        throw "Cannot read the live std::vector object"
    }

    $begin = [BitConverter]::ToUInt64($vector, 0)
    $end = [BitConverter]::ToUInt64($vector, 8)
    $capacity = [BitConverter]::ToUInt64($vector, 16)
    if ($end -lt $begin -or $capacity -lt $end -or
        ($end - $begin) -ne 8192) {
        throw ("Unexpected live EEPROM vector: begin=0x{0:X} end=0x{1:X} " +
               "capacity=0x{2:X}" -f $begin, $end, $capacity)
    }

    $save = New-Object byte[] 8192
    $ok = [NativeProcessMemory]::ReadProcessMemory(
        $handle, [UIntPtr]::new($begin), $save, [UIntPtr]::new(8192),
        [ref]$bytesRead)
    if (-not $ok -or $bytesRead.ToUInt64() -ne 8192) {
        throw "Cannot read the live EEPROM contents"
    }
} finally {
    [void][NativeProcessMemory]::CloseHandle($handle)
}

$outputPath = [IO.Path]::GetFullPath($Output)
if ((Test-Path -LiteralPath $outputPath) -and -not $Force) {
    throw "Output already exists; pass -Force to replace it: $outputPath"
}
$parent = Split-Path -Parent $outputPath
if ($parent) {
    [IO.Directory]::CreateDirectory($parent) | Out-Null
}
$temporary = "$outputPath.tmp"
[IO.File]::WriteAllBytes($temporary, $save)
if (Test-Path -LiteralPath $outputPath) {
    [IO.File]::Replace(
        $temporary, $outputPath, "$outputPath.bak", $true)
} else {
    [IO.File]::Move($temporary, $outputPath)
}

$sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $outputPath).Hash.ToLower()
[pscustomobject]@{
    ProcessId = $ProcessId
    Executable = $executable
    Output = $outputPath
    Bytes = 8192
    SHA256 = $sha256
}
