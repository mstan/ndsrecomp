# check_local_wfc_stack.ps1 -- fast host-side preflight for the local WFC rig.
#
# This checks the loopback services that --wfc-provider local/local-oracle
# expects to reach through Slirp's 10.64.0.1 host alias. By default it treats
# the plain local protocol-oracle services as required and reports whether the
# extra NAS HTTPS capability needed by unmodified MKDS is present. Pass
# -RequireNasHttps to make missing port 443 a failing condition.
#
[CmdletBinding()]
param(
    [string] $DnsServer = '127.0.0.1',
    [int] $DnsPort = 53,
    [string] $ExpectedDnsAnswer = '10.64.0.1',
    [string[]] $DnsNames = @(
        'conntest.nintendowifi.net',
        'nas.nintendowifi.net',
        'gpcm.gs.nintendowifi.net',
        'mariokartds.master.gs.nintendowifi.net'
    ),
    [string] $HttpAddress = '127.0.0.1',
    [int[]] $TcpPorts = @(80, 9000, 443),
    [int] $TimeoutMilliseconds = 3000,
    [string] $OutJson = '',
    [switch] $RequireNasHttps,
    [switch] $JsonOnly
)

$ErrorActionPreference = 'Stop'

if ($DnsPort -lt 1 -or $DnsPort -gt 65535) { throw "-DnsPort must be in 1..65535" }
foreach ($port in $TcpPorts) {
    if ($port -lt 1 -or $port -gt 65535) { throw "-TcpPorts entries must be in 1..65535" }
}
if ($TimeoutMilliseconds -lt 100) { throw "-TimeoutMilliseconds must be at least 100" }

function Test-TcpPort {
    param([string] $Address, [int] $Port, [int] $TimeoutMs)

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect($Address, $Port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne($TimeoutMs, $false)) {
            return [ordered]@{ port = $Port; open = $false; error = 'timeout' }
        }
        $client.EndConnect($async)
        return [ordered]@{ port = $Port; open = $true; error = $null }
    }
    catch {
        return [ordered]@{ port = $Port; open = $false; error = $_.Exception.Message }
    }
    finally {
        $client.Close()
    }
}

function Test-DnsName {
    param([string] $Name, [string] $Server, [string] $Expected)

    try {
        $answers = @(
            Resolve-DnsName -Name $Name -Server $Server -Type A -DnsOnly -ErrorAction Stop |
            Where-Object { $_.Type -eq 'A' -and $_.IPAddress } |
            ForEach-Object { $_.IPAddress }
        )
        return [ordered]@{
            name = $Name
            addresses = $answers
            matches_expected = ($answers -contains $Expected)
            error = $null
        }
    }
    catch {
        return [ordered]@{
            name = $Name
            addresses = @()
            matches_expected = $false
            error = $_.Exception.Message
        }
    }
}

function Invoke-HttpProbe {
    param(
        [string] $Url,
        [string] $HostHeader,
        [int] $TimeoutMs
    )

    try {
        $request = [System.Net.HttpWebRequest]::Create($Url)
        $request.Method = 'GET'
        $request.Timeout = $TimeoutMs
        $request.ReadWriteTimeout = $TimeoutMs
        if ($HostHeader) {
            $request.Host = $HostHeader
        }
        $response = $request.GetResponse()
        try {
            $reader = New-Object System.IO.StreamReader($response.GetResponseStream())
            $body = $reader.ReadToEnd()
            $preview = if ($body.Length -gt 80) { $body.Substring(0, 80) } else { $body }
            return [ordered]@{
                url = $Url
                host = $HostHeader
                ok = ([int] $response.StatusCode -ge 200 -and [int] $response.StatusCode -lt 400)
                status = [int] $response.StatusCode
                body_preview = $preview
                error = $null
            }
        }
        finally {
            $response.Close()
        }
    }
    catch [System.Net.WebException] {
        $status = $null
        if ($_.Exception.Response) {
            $status = [int] $_.Exception.Response.StatusCode
            $_.Exception.Response.Close()
        }
        return [ordered]@{
            url = $Url
            host = $HostHeader
            ok = $false
            status = $status
            body_preview = ''
            error = $_.Exception.Message
        }
    }
    catch {
        return [ordered]@{
            url = $Url
            host = $HostHeader
            ok = $false
            status = $null
            body_preview = ''
            error = $_.Exception.Message
        }
    }
}

function Find-WfcDnsProcesses {
    $items = @()
    Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='python3.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like '*wfc_dns.py*' } |
        ForEach-Object {
            $items += [ordered]@{
                pid = $_.ProcessId
                command_line = $_.CommandLine
            }
        }
    return $items
}

function Test-OpenSslSsl3Option {
    $command = Get-Command openssl.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) {
        return [ordered]@{
            found = $false
            path = $null
            ssl3_client_option = $false
            error = 'openssl.exe not found on PATH'
        }
    }

    try {
        $help = & $command.Source s_client -help 2>&1 | Out-String
        $hasSsl3 = ($help -match '(^|\s)-ssl3(\s|$)')
        return [ordered]@{
            found = $true
            path = $command.Source
            ssl3_client_option = [bool] $hasSsl3
            error = $null
        }
    }
    catch {
        return [ordered]@{
            found = $true
            path = $command.Source
            ssl3_client_option = $false
            error = $_.Exception.Message
        }
    }
}

$dnsChecks = @()
foreach ($name in $DnsNames) {
    $dnsChecks += (Test-DnsName -Name $name -Server $DnsServer -Expected $ExpectedDnsAnswer)
}

$tcpChecks = @()
foreach ($port in $TcpPorts) {
    $tcpChecks += (Test-TcpPort -Address $HttpAddress -Port $port -TimeoutMs $TimeoutMilliseconds)
}

$httpChecks = @(
    (Invoke-HttpProbe -Url "http://${HttpAddress}/" -HostHeader 'conntest.nintendowifi.net' -TimeoutMs $TimeoutMilliseconds),
    (Invoke-HttpProbe -Url "http://${HttpAddress}/ac" -HostHeader 'nas.nintendowifi.net' -TimeoutMs $TimeoutMilliseconds),
    (Invoke-HttpProbe -Url "http://${HttpAddress}:9000/ac" -HostHeader 'nas.nintendowifi.net' -TimeoutMs $TimeoutMilliseconds)
)

$tcpByPort = @{}
foreach ($entry in $tcpChecks) {
    $tcpByPort[[int] $entry.port] = [bool] $entry.open
}

$allDnsExpected = -not (@($dnsChecks | Where-Object { -not $_.matches_expected }))
$port80Open = ($tcpByPort.ContainsKey(80) -and $tcpByPort[80])
$port9000Open = ($tcpByPort.ContainsKey(9000) -and $tcpByPort[9000])
$port443Open = ($tcpByPort.ContainsKey(443) -and $tcpByPort[443])
$conntestHttpOk = [bool] $httpChecks[0].ok
$nasHttpOk = [bool] ($httpChecks[1].ok -or $httpChecks[2].ok)
$basicReady = ($allDnsExpected -and $port80Open -and $port9000Open -and $conntestHttpOk -and $nasHttpOk)
$mkdsReady = ($basicReady -and $port443Open)

$findings = @()
if (-not $allDnsExpected) {
    $findings += "DNS queries did not all resolve to ${ExpectedDnsAnswer} through ${DnsServer}; check tools/wfc_dns.py and port ${DnsPort}."
}
if (-not $port80Open) {
    $findings += "TCP port 80 is not open on ${HttpAddress}; dwc_haproxy HTTP routing is not reachable."
}
if (-not $port9000Open) {
    $findings += "TCP port 9000 is not open on ${HttpAddress}; direct NAS HTTP is not reachable."
}
if (-not $conntestHttpOk) {
    $findings += "Plain HTTP conntest probe did not return a successful response."
}
if (-not $nasHttpOk) {
    $findings += "Plain HTTP NAS probe did not return a successful response."
}
if (-not $port443Open) {
    $findings += "TCP port 443 is not open on ${HttpAddress}; unmodified MKDS will fail later when it reaches NAS HTTPS."
}

$openssl = Test-OpenSslSsl3Option
if (-not $openssl.ssl3_client_option) {
    $findings += "The first openssl.exe on PATH does not expose s_client -ssl3; it is not a ready SSLv3 probe for the DS-era NAS endpoint."
}

$exitOk = $basicReady
if ($RequireNasHttps) {
    $exitOk = ($exitOk -and $mkdsReady)
}

$result = [ordered]@{
    timestamp = (Get-Date).ToString('o')
    status = $(if ($exitOk) { 'ok' } else { 'failed' })
    basic_local_services_ready = [bool] $basicReady
    unmodified_mkds_connection_test_ready = [bool] $mkdsReady
    require_nas_https = [bool] $RequireNasHttps
    dns_server = "${DnsServer}:${DnsPort}"
    expected_dns_answer = $ExpectedDnsAnswer
    wfc_dns_processes = @(Find-WfcDnsProcesses)
    dns = $dnsChecks
    tcp = $tcpChecks
    http = $httpChecks
    tls = [ordered]@{
        nas_https_port_open = [bool] $port443Open
        openssl = $openssl
    }
    findings = $findings
}

$json = $result | ConvertTo-Json -Depth 8
if ($OutJson) {
    $outPath = [System.IO.Path]::GetFullPath($OutJson)
    $outDir = Split-Path -Parent $outPath
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }
    Set-Content -LiteralPath $outPath -Value $json -Encoding UTF8
}

if ($JsonOnly) {
    Write-Output $json
}
else {
    Write-Host "Local WFC preflight: $($result.status)"
    Write-Host "  basic local services ready: $($result.basic_local_services_ready)"
    Write-Host "  unmodified MKDS ready:      $($result.unmodified_mkds_connection_test_ready)"
    Write-Host "  DNS expected answer:        $ExpectedDnsAnswer via ${DnsServer}:${DnsPort}"
    foreach ($check in $tcpChecks) {
        Write-Host ("  TCP {0}: {1}" -f $check.port, $(if ($check.open) { 'open' } else { 'closed' }))
    }
    foreach ($finding in $findings) {
        Write-Warning $finding
    }
    if ($OutJson) {
        Write-Host "  wrote JSON: $outPath"
    }
}

if ($exitOk) {
    exit 0
}
exit 1
