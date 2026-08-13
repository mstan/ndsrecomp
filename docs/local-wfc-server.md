# Local WFC server: install, run, verify, tear down

This is the runbook for the **locally hosted Nintendo WFC/DWC server** used
to develop and debug the guest network stack offline, instead of pointing a
half-built stack at Kaeru/Wiimmfi's volunteer-run production service. See
`docs/wfc-external-facts.md` (Strand D) for the licensing/provider research
this implements, and `docs/networking-oracles.md` for the project's general
AGPL prove-only policy.

Everything below was personally installed, started, and probed on this
machine on 2026-08-10. Ports, commands, and probe output are copy-pasted
from that session, not reconstructed from memory.

**Nothing in this runbook lives inside an ndsrecomp repo except this one
file.** The server, its Docker build context, and the DNS responder all
live under a scratch directory outside `F:\Projects\ndsrecomp\*`:

```
C:\Users\Matthew\AppData\Local\Temp\claude\F--Projects-ndsrecomp\9d6026bb-5c12-4aee-8e91-9d1c68f738f0\scratchpad\wfc-server\
├── dwc-docker\        # Docker build context (cloned + customized, see below)
└── wfc_dns.py          # our own tiny DNS responder (not derived from the AGPL project)
```

If that scratch directory is cleaned up, treat the "install steps" section
as the reproduction recipe — it is short.

---

## 1. What this is (implementation, fork, and verified license)

**Implementation:** `dwc_network_server_emulator`, canonical repo
`github.com/barronwaffles/dwc_network_server_emulator`.

**License — VERIFIED by fetching the repo's actual `LICENSE` file content**
(not by trusting the README or any third party's claim):

```
GNU AFFERO GENERAL PUBLIC LICENSE
Version 3, 19 November 2007
```

i.e. **AGPL-3.0**. GitHub's own repo metadata (`license.spdx_id`) agrees:
`AGPL-3.0`. This matches this project's standing policy: **prove-only**. It
is run here as an external black-box process — never linked into
ndsrecomp, never vendored, never copied, and its source was read only far
enough to (a) confirm the license, (b) find the right entry-point commands
to run it, and (c) understand its declared port/config layout so it could
be started and probed. No ndsrecomp implementation work reads this
repository.

**Fork survey — why `barronwaffles/dwc_network_server_emulator` and not a
fork:**

| Repo | `pushed_at` (via `gh api repos/<repo>`) | License | Notes |
|---|---|---|---|
| `barronwaffles/dwc_network_server_emulator` | **2024-08-02** | AGPL-3.0 | Not a fork — this is itself the current continuation of the "polaris-" codebase (its own README links to `polaris-/dwc_network_server_emulator/wiki`, an org that has folded back into this repo). Most recently active of everything surveyed. |
| `Maritoguionyo/dwc_network_server_emulator` ("AltWFC") | 2021-05-09 | AGPL-3.0 | Community AltWFC branding; stale relative to barronwaffles. |
| `ubergeek77/dwc_network_server_emulator` | 2019-05-23 | AGPL-3.0 | Fork, stale. |
| `ZehCariocaRj/dwc_network_server_emulator_kyle95wm` | 2018-08-22 | AGPL-3.0 | Fork's own README says "from 2019" and is out of date. |
| `florensie/dwc_network_server_emulator` | 2023-04-03 | AGPL-3.0 | Fork, stale relative to barronwaffles. |
| `EnergyCube/dwc_network_server_emulator` | 2021-01-24 | AGPL-3.0 | Fork, stale. |

**Python-3 note:** every one of the above (including the "current" one) is
**Python 2.7** — confirmed by literal `import BaseHTTPServer` /
`import SocketServer` (Python-2-only stdlib module names) in
`nas_server.py` and siblings, not just the README's stated requirement. A
Python-3.11 fork was found (`alkeon/dwc_network_server_emulator` on
NotABug.org) but was not used: it is far lower-profile (no GitHub stars,
no visible activity signal), and Docker made the Python-2.7 problem moot
anyway (see next section) — using the better-provenance, more-active
original was the more complete choice, not a shortcut.

**No fork was found where "even this prove-only use" would be against the
license.** All are AGPL-3.0; none add a further restriction.

---

## 2. How it's run: Docker, not a host Python 2 install

Python 2.7 is EOL and its usual Debian-Buster-based Docker base image
(`python:2.7-slim`) has had its **apt mirrors pulled from the regular
Debian mirror network** (`deb.debian.org` now 404s Buster) — a real,
current-as-of-2026 breakage, not a hypothetical. Rather than hand-install
Python 2.7 + Twisted into this machine's environment (against the task's
own instruction not to pollute the project's `.venv`s, and generally an
EOL-interpreter headache on Windows), the server runs in a **Docker
container**, using the community `TheForcer/dwc-docker` wrapper
(no LICENSE file on that repo — it's a thin Dockerfile/compose wrapper,
not a redistribution of the AGPL source; the AGPL code is `git clone`d at
*build* time from `barronwaffles/dwc_network_server_emulator`) as a
starting point, customized as follows:

- **`Dockerfile`**: patched the `apt-get update` step to point at
  `archive.debian.org` (Buster is EOL and gone from the live mirrors), and
  added an `entrypoint.sh` (see below) as the container's `CMD` in place
  of the original bare `python master_server.py`.
- **`entrypoint.sh`** (new, ours, not AGPL code): `master_server.py`'s own
  `if __name__ == "__main__":` block — read directly, this is the project's
  own documented top-level launcher — **starts every service except the
  server browser**: the line `# GameSpyServerBrowserServer` in its
  `servers` list is commented out upstream. `gamespy_server_browser_server.py`
  has its own working `if __name__ == "__main__":` entry point, so
  `entrypoint.sh` just runs *both* of the project's own, unmodified
  published entry points as two external OS processes (`master_server.py`
  and `gamespy_server_browser_server.py`, the latter started 2s later so
  the backend manager it connects to is already listening). This is
  orchestration, not a code change to the emulator.
- **`docker-compose.yml`**: every port is published as
  `"127.0.0.1:<port>:<port>"` (loopback only, never `0.0.0.0`) — see
  "Binding / exposure" below. Added the missing `9002` (GameStats HTTP)
  and the optional `9001`/`9003`/`9009`/`9998` ports to the `dwc` service's
  `EXPOSE`/`ports` lists (upstream `dwc-docker` only published a subset).

None of this touches the emulator's own `.py` files.

### Install steps performed (reproducible from scratch)

```powershell
$root = "<scratch>\wfc-server"
New-Item -ItemType Directory -Force -Path $root
Set-Location $root
git clone https://github.com/TheForcer/dwc-docker.git dwc-docker
```

Then, inside `dwc-docker\`:
1. Edit `Dockerfile`: before the `apt-get update` line, add
   `sed -i -e 's|deb.debian.org/debian|archive.debian.org/debian|g' -e 's|security.debian.org/debian-security|archive.debian.org/debian-security|g' /etc/apt/sources.list &&` and use
   `apt-get -o Acquire::Check-Valid-Until=false update -y`.
2. Add `entrypoint.sh` (content below) next to the `Dockerfile`.
3. In `Dockerfile`, add `COPY entrypoint.sh /dwc/entrypoint.sh`,
   `RUN chmod +x /dwc/entrypoint.sh`, extend `EXPOSE` to include
   `9002 9003 9009 9998`, and change `CMD` to
   `["sh", "/dwc/entrypoint.sh"]`.
4. In `docker-compose.yml`, prefix every port mapping with `127.0.0.1:`
   and add the extra ports.

`entrypoint.sh`:

```sh
#!/bin/sh
set -e
python master_server.py &
MASTER_PID=$!
sleep 2
python gamespy_server_browser_server.py &
BROWSER_PID=$!
trap "kill -TERM $MASTER_PID $BROWSER_PID 2>/dev/null" TERM INT
wait $MASTER_PID
wait $BROWSER_PID
```

### Start / stop / status

```powershell
Set-Location "<scratch>\wfc-server\dwc-docker"
docker compose build      # first time only, or after editing Dockerfile
docker compose up -d      # start (both dwc_server and dwc_haproxy containers)
docker compose ps         # status
docker logs dwc_server --tail 100    # service startup log / errors
docker compose down       # stop and remove containers (keeps the named 'data' volume)
docker compose down -v    # stop and also delete the account/session database volume
```

Persistent account/session data lives in the Docker-managed volume
`dwc-docker_data` (SQLite DBs written by the emulator itself under
`/dwc/data` inside the container). `docker compose down` (no `-v`) leaves
it intact across restarts; `-v` wipes it for a clean slate.

---

## 3. Services started and ports (all loopback-only)

Verified via `docker compose ps` (Docker Desktop, Windows, so
`127.0.0.1:<port>` is the *only* thing published — nothing binds
`0.0.0.0` on the host side) and cross-checked with
`Test-NetConnection`/raw sockets from this machine:

| Service | Protocol | Port | Started by | Reachable? |
|---|---|---|---|---|
| NAS (authentication) | TCP/HTTP | **9000** | `master_server.py` | **Yes** — see §5 |
| NAS via Host-header routing (`nas.nintendowifi.net`, `conntest.nintendowifi.net`) | TCP/HTTP | **80** (via `dwc_haproxy`) | `haproxy` container | **Yes** — see §5 |
| GPCM (GameSpy profile/presence) | TCP | **29900** | `master_server.py` | **Yes**, with a real protocol banner — see §5 |
| GPSP (GameSpy player search) | TCP | **29901** | `master_server.py` | **Yes** (TCP connect only, see §5 caveat) |
| Server browser | TCP | **28910** | `gamespy_server_browser_server.py` (needed the `entrypoint.sh` fix — **disabled by default** upstream) | **Yes** (TCP connect only) |
| QR (server query/report) | UDP | **27900** | `master_server.py` | **Yes**, port open — see §5 for what "verified" means for UDP |
| NATNEG | UDP | **27901** | `master_server.py` | **Yes**, port open — same caveat |
| GameStats (HTTP) | TCP/HTTP | **9002** | `master_server.py` | **Yes** — see §5 |
| GameStats (binary GameSpy protocol) | TCP | **29920** | `master_server.py` | **Yes**, sent an unsolicited banner |
| Storage / "sake" (DLC/save storage) | TCP/HTTP | **8000** | `master_server.py` | TCP connect verified; a bare `GET /` closes the connection without a response (needs a real request shape — not dug into further, see limitations) |
| Storage via Host-header routing (`*.sake.gs.nintendowifi.net`) | TCP/HTTP | **80** (via haproxy) | `haproxy` | Routed, same caveat as above |
| GameSpy backend manager (internal, used by the server-browser process) | TCP | **27500** | `master_server.py` | Reachable; internal-use only, not part of the DS-facing protocol surface |
| DLS1 (download service) | TCP/HTTP | **9003** | `master_server.py` | TCP connect verified only |
| Internal stats | TCP | **9001** | `master_server.py` | TCP connect verified only (no unsolicited banner logged, but the port is bound — confirmed via in-container `connect_ex`) |
| Admin page | TCP/HTTP | **9009** | `master_server.py` | **Yes** — HTTP 401 Unauthorized (alive, requires the credentials in `adminpageconf.json`) |
| Register page | TCP/HTTP | **9998** | `master_server.py` | TCP connect verified only |

**Binding / exposure:** every port above is published as
`127.0.0.1:<port>` in `docker-compose.yml` — confirmed in
`docker compose ps` output (`127.0.0.1:8000->8000/tcp`, etc., for every
line, never `0.0.0.0->`). Nothing here is reachable from outside this
machine. **No change was made to this machine's system DNS, hosts file,
or firewall** — see §4 for how DNS is handled instead.

---

## 4. DNS: a small custom responder (not from the AGPL project)

The DS resolves `*.nintendowifi.net` via whatever DNS server it's
configured to use, then connects to whatever IP comes back. To make that
work locally **without touching this machine's system DNS settings, hosts
file, or firewall**, use the small, fully original, dependency-free Python
3 UDP DNS responder at `tools\wfc_dns.py`. It is not derived
from, `dwc_network_server_emulator` or any other AGPL/GPL project — no
DNS server ships with that project at all.

Behavior: listens on UDP, and for any A-record query whose name equals or
ends with a configured suffix (default `nintendowifi.net`), answers with
a single A record pointing at a configured IP. For the runner's Slirp
backend, answer `10.64.0.1`: that is libslirp's guest-visible host alias,
and libslirp translates it to this machine's `127.0.0.1`, where the
Docker containers' ports are published. Do not answer `127.0.0.1` to the
guest; inside the guest that is the guest's own loopback. Anything else
gets `REFUSED` rather than being resolved — it deliberately does not act
as a general open resolver.

### Start / stop

```powershell
$py = "C:\Users\Matthew\AppData\Local\Programs\Python\Python312\python.exe"
# NOTE: do not invoke via a bare "python" on PATH on this machine -- devkitPro's
# MSYS2 python.exe is earlier on PATH and mangles Windows-style paths. Use the
# real Windows interpreter's full path explicitly (found via `py -3 -c "import sys; print(sys.executable)"`).
Start-Process -FilePath $py `
  -ArgumentList "`"F:\Projects\ndsrecomp\ndsrecomp\tools\wfc_dns.py`" --bind 127.0.0.1 --port 53 --answer 10.64.0.1" `
  -RedirectStandardOutput "<scratch>\wfc-server\wfc_dns.log" `
  -RedirectStandardError  "<scratch>\wfc-server\wfc_dns.err.log" `
  -WindowStyle Hidden -PassThru
# -> prints a PID; to stop: Stop-Process -Id <PID>
```

Binds to **`127.0.0.1:53` only** (loopback). This required no
administrative privilege — Windows, unlike Unix, does not restrict
non-elevated processes from binding ports below 1024, and this was
verified directly (`UdpClient.Bind` against `127.0.0.1:53` succeeded
from an unelevated PowerShell session before anything else was started).
This is **not** a change to the machine's system DNS resolver — it is
a private resolver instance nothing queries unless explicitly pointed at
it. Pointing the *guest's* DNS at the host-loopback service is the
runner's job: use `--wfc-provider local` (or `local-oracle`), which hands
the guest `10.64.0.1` via DHCP and lets Slirp translate traffic for that
address back to host loopback.

### Verified resolution (real output, this session)

```powershell
PS> Resolve-DnsName -Name gpcm.gs.nintendowifi.net -Server 127.0.0.1 -Type A

Name                     Type TTL Section IPAddress
----                     ---- --- ------- ---------
gpcm.gs.nintendowifi.net A    60  Answer  10.64.0.1

PS> Resolve-DnsName -Name nas.nintendowifi.net -Server 127.0.0.1 -Type A
nas.nintendowifi.net A    60  Answer  10.64.0.1

PS> Resolve-DnsName -Name conntest.nintendowifi.net -Server 127.0.0.1 -Type A
conntest.nintendowifi.net A    60  Answer  10.64.0.1

PS> Resolve-DnsName -Name example.com -Server 127.0.0.1 -Type A
Resolve-DnsName : example.com : DNS operation refused
```

The last line is deliberate (see above) — it demonstrates the responder
is scoped to WFC names, not a general resolver.

---

## 5. Proof it answers (real probe output, this session)

### TCP: GPCM (29900) — real protocol banner, unsolicited

```
CONNECTED, 38 bytes: \lc\1\challenge\BXZERAHECA\id\1\final\
```

This is a live GameSpy login-challenge banner sent by the server the
instant a TCP connection lands — the strongest form of evidence (no probe
payload needed at all).

### TCP: GPSP (29901) / server browser (28910) — connect verified, protocol not exercised

Both protocols are client-initiated (no unsolicited banner is expected —
GPCM is the exception in the suite). `TcpClient.Connect` succeeded to
both; no further protocol exchange was attempted. **Marking these as
"reachable, protocol handshake unverified."** This project's AGPL
prove-only stance was applied conservatively here: rather than crafting a
protocol-exact request packet by reading the emulator's own parsing code
(which would blur "black-box protocol peer" into "derived from reading
the implementation"), the deeper exchange was left unverified rather than
risk that line.

### TCP: NAS (9000, and 80 via haproxy) — HTTP 200

```
GET http://127.0.0.1:9000/ac        -> 200 "ok"
GET http://127.0.0.1/ac  (Host: nas.nintendowifi.net, via haproxy :80) -> 200 "ok"
```

### TCP: GameStats HTTP (9002) — HTTP 200 with a live token

```
GET http://127.0.0.1:9002/  -> 200, body: xB3f5LuDXXck2ySQKlVxZpUthwtNC2kf
```

### TCP: GameStats binary (29920) — unsolicited banner

```
CONNECTED: 9b,[%+4%x/44=+e.1T\final\
```

(Partially binary/encrypted challenge prefix, ending in the same
`\final\` marker style as GPCM — confirms it's alive and speaking a
GameSpy-family protocol without needing a crafted request.)

### TCP: Admin page (9009) — HTTP 401

```
GET http://127.0.0.1:9009/  -> 401 Unauthorized
```

Alive and enforcing the credentials in `adminpageconf.json`.

### TCP: Storage/"sake" (8000) — connects, closes on generic GET

```
GET http://127.0.0.1:8000/  -> "The underlying connection was closed unexpectedly"
```

TCP-level reachability was independently confirmed
(`Test-NetConnection -Port 8000` → `True`); a bare `GET /` isn't a request
shape this server responds to. **Marking as reachable but
protocol-unverified**, same reasoning as GPSP/server-browser above.

### UDP: QR (27900) and NATNEG (27901) — port-open proof by ICMP contrast

UDP gives no reply to a malformed payload by design (both are
strict binary protocols requiring a specific magic-byte header), so "no
reply" alone doesn't prove anything. Instead, this session distinguished
**open-but-silent** from **closed** using Windows' own ICMP-driven socket
error surfacing:

```
Control: send "PING" to UDP 27999 (nothing listens here)
  -> SocketErrorCode=ConnectionReset ("forcibly closed by the remote host"), near-instant

Test: send "PING" to UDP 27900 (QR)
  -> SocketErrorCode=TimedOut, full 1500ms wait, no ICMP error surfaced

Test: send "PING" to UDP 27901 (NATNEG)
  -> SocketErrorCode=TimedOut, full 1500ms wait, no ICMP error surfaced
```

A truly closed UDP port on this machine produces an immediate
`ConnectionReset` (the OS surfaces the ICMP Port Unreachable it received).
27900 and 27901 instead time out cleanly with no reset — i.e. **the ports
are bound and accepting datagrams**; the application simply doesn't
respond to a payload it can't parse, which is the expected, correct
behavior for a strict binary protocol. This is also corroborated by the
container startup log:

```
[GameSpyQRServer] Server is now listening on 0.0.0.0:27900...
[GameSpyNatNegServer] Server is now listening on 0.0.0.0:27901...
```

**What was not attempted:** a protocol-correct QR/NATNEG datagram. Same
reasoning as the GPSP/server-browser/storage cases above — this would
require deriving exact packet shapes from reading the emulator's own
parsing code, and the port-open proof above already gives a real,
non-fabricated positive result without doing that.

---

## 6. TLS / HTTPS — corrected 2026-08-10, read this before the authentication milestone

**Revision note:** an earlier version of this section cited Wii-specific
NAND certificate extraction (`00000011.app`) as the mechanism DS HTTPS
depends on. That was wrong for this target — `00000011.app` is a **Wii
NAND title** (the Wii's IOS/cert store); the DS has no NAND and no system
cert store, so that specific extraction path cannot be what a DS does.
This section was rewritten after reading the actual MKDS binary
(read-only, our own game asset — not the AGPL server project) to settle
the question with direct, on-disk evidence rather than repeat a
Wii-shaped assumption. **Bottom line, corrected: DS DWC *does* use HTTPS
for NAS specifically, validated against a certificate chain embedded
*inside the game binary itself* (not a system store, not a per-console
secret) — and the community has a documented, working, no-hardware-needed
way to satisfy it. This is better news than the original write-up: it is
solvable now, not deferred to a hardware refinement pass.**

### 6.1 Local binary evidence (read-only scan, this session)

A read-only string/byte scan (`scan_dwc_strings.py`, original script
written for this task, not derived from any AGPL source) of the game's
extracted, already-generated inputs was run against:
`F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi\generated\inputs\arm9.bin`,
`arm7.bin`, and `generated\inputs\overlays\arm9_overlay_{000,001,003}.bin`.
No file was modified.

**`arm9.bin` offset `0x00000b71`–`0x00000bf4`** — Nintendo SDK library
version tags (a standard linker-embedded convention, one tag per linked
library):

```
[SDK+NINTENDO:BACKUP]
[SDK+NINTENDO:DWC20051007-1634_DWC20051007_NOTOUCH]
[SDK+NINTENDO:WiFi1.0.10200.0510061936]
[SDK+UBIQUITOUS:SSL]
[SDK+UBIQUITOUS:CPS]
```

This confirms the game links Nintendo's DWC (GameSpy-derived) networking
library, the Nitro WiFi driver, and — critically — a distinct **SSL**
library (tagged `UBIQUITOUS`, not `NINTENDO`, i.e. third-party middleware
bundled alongside Nintendo's own SDK code, consistent with GameSpy having
supplied its own lightweight embedded SSL stack). This by itself already
rules out "no TLS at all" for this title.

**`arm9_overlay_000.bin`** (the DWC/networking overlay — 214,880 bytes)
contains the actual endpoint URLs and HTTP client code:

| Offset | String | Scheme |
|---|---|---|
| `0x00030b20` | `https://nas.nintendowifi.net/ac` | **HTTPS** |
| `0x00030e98` | `http://conntest.nintendowifi.net/` | plain HTTP |
| `0x00033d00`/`0x00033d28`/`0x00033d4c` | `https://nas.test.nintendowifi.net/ac`, `https://nas.dev.nintendowifi.net/ac`, `https://nas.nintendowifi.net/ac` (dev/test/prod variants together) | **HTTPS** |
| `0x000325f0` | `gpcm.gs.nintendowifi.net` | bare hostname, no scheme — raw TCP GameSpy protocol, matches the live `\lc\1\challenge\...` banner captured in §5 |
| `0x000330c0` | `gpsp.gs.nintendowifi.net` | bare hostname — raw TCP |
| `0x00033755` | `gamestats.gs.nintendowifi.net` | bare hostname |
| `0x00033b1f`/`0x00033b3b` | `natneg1.gs.nintendowifi.net`, `natneg2.gs.nintendowifi.net` | bare hostname — raw UDP |

So the **only HTTPS endpoint in the whole client is NAS's `/ac` path**
(`nas`/`nas.test`/`nas.dev` variants). `conntest` is deliberately plain
HTTP (it's just a connectivity probe). Everything else (GPCM, GPSP,
server browser, QR, NATNEG, GameStats) is raw TCP/UDP GameSpy protocol,
not HTTP/HTTPS at all — no TLS question applies to those, matching what
§5's probes already observed empirically (GPCM's banner, QR/NATNEG's
binary framing).

Also at this same offset region, the client's own minimal HTTP
implementation is visible directly as format strings — confirming the DS
builds its own raw HTTP requests rather than using any OS-level HTTP
stack (there is none on DS):

```
0x00030dac  POST /%s HTTP/1.0..Content-type: application/...
0x00030e00  GET /%s HTTP/1.0..Host: %s...
0x00030c58  ...User-Agent..Nitro WiFi SDK/1....
```

**`0x0003145e`–`0x00031b3f`** — embedded root/intermediate certificate
material, found as human-readable issuer/subject strings sitting directly
between raw binary key data (this is **not** standard X.509 DER — a
targeted search for `30 82 .. 30 82` ASN.1 SEQUENCE headers with
plausible certificate-sized lengths in this region found none; GameSpy's
"UBIQUITOUS:SSL" library evidently uses its own flat, non-ASN.1 record
format: raw RSA modulus bytes, then a 4-byte little-endian RSA exponent
(`01 00 01 00` = 65537), then what look like relocated ARM9 RAM pointers
(e.g. `f0 18 1b 02` = address `0x021b18f0`, a plausible NitroSDK main-RAM
address), then the issuer/subject name as a plain string):

| Offset | Issuer/subject string |
|---|---|
| `0x0003145e` | `US, Washington, Nintendo of America Inc, NOA, Nintendo CA, ca@noa.nintendo.com` |
| `0x000317d4` | `US, VeriSign, Inc., Class 3 Public Primary Certification Authority - G2, (c) 1998 VeriSign, Inc. - For authorized use only, VeriSign Trust Network` |
| `0x00031880` | `US, VeriSign, Inc., VeriSign Trust Network, (c) 1999 VeriSign, Inc. - For authorized use only, VeriSign Class 3 Public Primary Certification Authority` |
| `0x00031a34` | `US, VeriSign, Inc., Class 3 Public Primary Certification Authority` (undated variant) |
| `0x00031b0f` | `US, RSA Data Security, Inc., Secure Server Certification Authority` |

I.e. the game embeds, **inside its own binary — not in any system cert
store, because the DS has none** — a small fixed set of trusted
root/intermediate public keys: Nintendo's own intermediate ("Nintendo
CA", issued by NOA) plus several VeriSign/RSA-Data-Security public roots
that Nintendo CA itself chains up to. This directly answers the
coordinator's (a)/(b)/(c) question:

**Answer: (b).** DS DWC uses HTTPS for the NAS `/ac` endpoint specifically,
and validates the server's certificate chain against a small set of
root/intermediate public keys **embedded in the game's own DWC/SSL
library**, not a system store and not anything extracted from a
console's NAND (DS has neither).

### 6.2 How the community satisfies this without a ROM patch (corroborated)

Local evidence alone can establish what the DS *requires*; it can't show
how a third party's *server* satisfies it, so this part was corroborated
against `github.com/KaeruTeam/nds-constraint` — **the Kaeru team's own
published writeup of exactly this mechanism** (license: none declared on
that repo; read here only to understand the mechanism, same prove-only
posture as the AGPL server). Its README states the target directly as
"the Nitro/TWL SDK's SSL library" (Nitro = DS SDK codename, TWL = DSi) —
i.e. this is documented as applying to DS, not only Wii:

> SSL certificate authorities can flag a certificate as allowed to act as
> an intermediate CA... Nintendo's implementation does not check this
> flag... **any certificate signed by Nintendo with its accompanying
> private key can be used as an intermediate CA to sign new certificates,
> and the DS doesn't care.**
>
> Nintendo consoles from the Wii onwards include a Nintendo-signed
> *client certificate* and its private key. This client certificate is
> signed by the same CA as Nintendo's server certificates and is
> therefore trusted by the DS.
>
> **These are not console-unique and can be pulled from any Wii** — this
> is not a per-unit secret.

This reconciles every previously-conflicting fact cleanly:

- **Why my original citation of `00000011.app`/Wii NAND extraction wasn't
  nonsense, just mis-scoped:** `Real96/Nintendo-SSL-DWC-Installer-Script`
  (read earlier, §1) extracts exactly this shared client
  certificate+key from a Wii's NAND dump (`00000011.app`) and uses it —
  per this exact technique — to forge a new leaf certificate for
  `nas.nintendowifi.net`, then serves that leaf plus the original
  Nintendo-signed client cert as the chain. The DS follows the chain up
  through the "fake CA" (the reused client cert) to Nintendo CA/VeriSign,
  which it already trusts (§6.1's embedded keys) — and never checks that
  the client cert wasn't supposed to be allowed to sign anything. So the
  mechanism **does** apply to DS (matching the embedded "Nintendo CA"
  found in our own ROM); I was only wrong to call it Wii-specific and to
  frame it as requiring *our* DS's own hardware.
- **Why this reconciles with Kaeru's "no hacks, patches, nor flashcards"
  claim:** the DS client is genuinely unpatched — Kaeru's *server* is
  what exploits the flaw, by presenting a certificate chain built this
  way. Nothing about the DS itself needs to change.
- **Why real DS hardware is not the blocker:** the secret this depends on
  (a Nintendo-signed client certificate + private key) is explicitly
  **not console-unique** — it does not need to come from *this project's*
  DS. It needs to come from *some* Wii (real or an existing NAND dump),
  which is a different, and for this project currently unmet, hardware
  requirement than "the DS + flash cart already available" noted
  elsewhere — worth flagging precisely rather than conflating the two.

### 6.3 Practical implication for this project

- **The local server as deployed today is plain HTTP only** (confirmed:
  `haproxy` binds `:80` only, no `:443` listener —
  `Invoke-WebRequest -Uri "https://127.0.0.1/ac" ...` fails with "Unable
  to connect to the remote server", i.e. no service there at all, not a
  cert-validation failure). This is sufficient for everything that isn't
  the NAS HTTPS handshake itself: GPCM, GPSP, QR, NATNEG, server browser,
  GameStats (all raw TCP/UDP, no TLS involved per §6.1), and NAS/GameStats
  HTTP/storage/admin pages **if** something on the guest side is willing
  to talk to them over plain HTTP instead of HTTPS.
- **Getting genuine HTTPS that a real, unmodified DS/ndsrecomp guest SSL
  stack will accept is a solved, documented, no-special-hardware-required
  problem** (§6.2) — it needs a Nintendo-signed client certificate + key
  (sourceable from a Wii, not a DS), used per `nds-constraint`'s recipe to
  sign a fresh `nas.nintendowifi.net` leaf cert, terminated with a
  standards-compliant TLS server that still supports **SSLv3** (the
  README notes this is the hard part today — most modern TLS stacks have
  removed SSLv3; `openssl s_server` or an old `nginx`/`stunnel` build with
  SSLv3 re-enabled would be needed), fronting the existing plain-HTTP NAS
  port on 443. **This was not implemented this session** — no Wii-sourced
  client certificate was available — but it is now correctly understood
  as an achievable near-term addition to this deployment's
  `docker-compose.yml`, not a change to the emulator itself, and not
  gated on this project's real DS hardware.
- Whether ndsrecomp's own (not-yet-built) guest SSL implementation will
  reproduce this exact same embedded-key/CA-flag-checking (mis)behavior
  is, naturally, a question for whoever implements that guest SSL stack —
  this section only establishes what the **real, original DWC library in
  the actual MKDS ROM** requires and accepts.

### 6.4 Does the local server implement DS-flavored NAS fields, or only Wii's?

**DS-flavored, confirmed on both sides.** The game's own NAS request
fields, found alongside the URLs above in `arm9_overlay_000.bin`
(offsets `0x00030bac`–`0x00030c98`): `acctcreate`, `login`, `gsbrcd`,
`ingamesn`, `sdkver`, `birth`, `devtime`, `bssid`, `apinfo`, `devname`,
`HTTP_X_GAMECD`. Cross-checked against the local server's own database
schema (`gs_database.py`, observed in the `dwc_server` container's
startup log when it created its tables): the `users` table includes
`gsbrcd`, `csnum`, `cfc`, `bssid`, `devname`, `birth`, `console` (an
integer the server-browser code branches on as "0 = DS, 1 = Wii" when a
lookup misses) — i.e. these are DS-specific concepts (`bssid` = the
access point's BSSID at connection time, `cfc` = DS Friend Code,
`csnum`/`devname` = console serial/device name) that only make sense for
a DS client, deliberately distinct from Wii's fields. **The local server
does implement the DS-flavored NAS surface**, not only a Wii one.

---

## 7. Pointing the runner at this server

Per `docs/wfc-external-facts.md`'s already-sketched `[network.wfc]`
provider shape:

```toml
[network.wfc]
enabled = true
provider = "local-oracle"

[network.wfc.providers.local-oracle]
dns_server = "10.64.0.1"   # Slirp guest-visible host alias for wfc_dns.py on host loopback
description = "Local dwc_network_server_emulator instance (AGPL-3.0, protocol oracle only, never linked)."
```

The guest's DNS traffic should be pointed at `10.64.0.1:53`. Slirp maps
that guest-visible host alias to the real host loopback, where
`wfc_dns.py` listens on `127.0.0.1:53`. Everything the guest subsequently
connects to over TCP/UDP should also resolve to `10.64.0.1`, which Slirp
maps to the Docker-published `127.0.0.1` ports in the table in §3.

---

## 8. Health check (quick copy-paste)

```powershell
docker compose -f "<scratch>\wfc-server\dwc-docker\docker-compose.yml" ps
docker logs dwc_server --tail 20
Resolve-DnsName -Name gpcm.gs.nintendowifi.net -Server 127.0.0.1 -Type A
Test-NetConnection -ComputerName 127.0.0.1 -Port 29900   # GPCM
Test-NetConnection -ComputerName 127.0.0.1 -Port 9000    # NAS
Invoke-WebRequest http://127.0.0.1:9000/ac -UseBasicParsing
```

A healthy stack: both containers `Up`, the host-side DNS query returns
`10.64.0.1`, the port tests against `127.0.0.1` report
`TcpTestSucceeded : True`, and the NAS request returns HTTP 200.

---

## 9. Known limitations / what is NOT verified

Numbered so nothing here gets mistaken for a proven capability:

1. **No HTTPS anywhere in this deployment yet** — see §6. NAS auth over
   real TLS against the guest's own SSL stack is unverified and will not
   work as-is against today's plain-HTTP-only server. **Corrected
   2026-08-10:** this is not an unresolvable gap requiring this project's
   real DS hardware — direct evidence from the MKDS binary itself (§6.1)
   plus the Kaeru team's own published `nds-constraint` writeup (§6.2)
   shows the fix is a known, documented technique (reuse a Nintendo-signed
   *client* certificate, which is not console-unique, to sign a fresh
   `nas.nintendowifi.net` leaf cert) that needs a Wii-sourced certificate
   and an SSLv3-capable TLS terminator — neither of which existed in this
   session, so it remains undone, but it is a concrete, scoped follow-up
   rather than a hardware-blocked one.
2. **GPSP (29901), server browser (28910), and storage/"sake" (8000)**:
   TCP connectivity verified; the actual application-level protocol
   exchange was not exercised (deliberately, to avoid deriving exact
   packet shapes from reading the AGPL source — see §5).
3. **QR (27900) and NATNEG (27901)**: UDP port-open status verified by
   ICMP-contrast, not by a protocol-correct exchange (see §5). No actual
   matchmaking/NAT-negotiation flow was tested.
4. **The server browser (28910) requires the `entrypoint.sh` fix** in
   this deployment — it is disabled in `master_server.py`'s own
   `servers` list upstream (commented out), for reasons not investigated
   (possibly a known issue with running its `multiprocessing.managers`
   client in the same process as the Twisted reactor). If a future
   upstream pull replaces `entrypoint.sh`'s assumptions, re-check this.
5. **No matchmaking flow (two real/simulated peers negotiating via
   NATNEG) was tested.** Only single-connection reachability per service.
6. **No account was actually created or authenticated against NAS** —
   only that the HTTP server responds. The `/ac` acctcreate/login flow
   itself was not driven end-to-end.
7. **Database persistence across restarts** was not tested this session
   (the `dwc-docker_data` volume mechanism is standard Docker and
   presumed to work, but not exercised).
8. This runbook was written and the server verified **before** the
   sibling runner-side `[network.wfc]` provider work landed — the exact
   config keys in §7 are copied from `docs/wfc-external-facts.md`'s
   sketch and were not tested against a live runner build in this
   session.

---

## 10. Switchover to real Kaeru WFC (when ready)

Per `docs/wfc-external-facts.md` Strand C (verified there from Kaeru's own
site, fetched directly):

- **Kaeru WFC is the correct, verified entry point for a real, unpatched
  DS** — no ROM patch documented as required. On the DS's own "Nintendo
  WFC Settings", set "Auto-obtain DNS" to **No**, and set **both Primary
  and Secondary DNS to `178.62.43.212`**.
- **Do not point a DS at Wiimmfi's own `95.217.77.181`.** That address is
  Wiimmfi's **Wii/WiiU-specific DNS patcher** — its documented mechanism
  exploits a Wii IOS SSL-validation bug and then live-patches the running
  Wii game over the network; it is not a DS mechanism, and nothing on
  Wiimmfi's own site claims it works for DS. Wiimmfi's general/official
  model otherwise expects a client-side ROM patch.
- To switch over: change only the `[network.wfc.providers.*]` selection
  (`provider = "wiimmfi"` or `"kaeru"` instead of `"local-oracle"`), which
  points guest DNS at `178.62.43.212` instead of this machine's
  `wfc_dns.py`. No other ndsrecomp-side change should be needed if the
  `[network.wfc]` provider model is wired the way
  `docs/wfc-external-facts.md` sketches it.
- Before switching over: stop hammering it with iterative development
  traffic. Live Wiimmfi/Kaeru is free, volunteer-run infrastructure with a
  public ban list and real concurrent players (verified live on
  `wiimmfi.de/stats/game/mariokartds` during the research pass this
  runbook's sibling doc performed) — reserve it for actual milestone
  acceptance runs, not iteration.

---

## 11. Teardown (clean stop)

```powershell
# Stop and remove the WFC server containers (keeps the account DB volume):
Set-Location "<scratch>\wfc-server\dwc-docker"
docker compose down

# Also delete the account/session database (full reset):
docker compose down -v

# Stop the DNS responder:
Stop-Process -Id <PID from Start-Process>
# or, if the PID was lost:
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -like '*wfc_dns.py*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId }
```

Nothing outside the scratch directory needs cleanup: no system DNS,
hosts file, or firewall changes were made at any point (see §4), and all
container ports were bound to `127.0.0.1` only (see §3), so no state
outside Docker's own managed volumes and this machine's loopback
interface was touched.
