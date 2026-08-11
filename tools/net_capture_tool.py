#!/usr/bin/env python3
"""M8 offline capture tool: dump, sanitize, and check NDSNETREPLAY1 packet
captures (runner/src/net/net_capture.h) WITHOUT building or running the
emulator.

This is an independent Python re-implementation of the SAME byte layout
runner/src/net/net_capture.h defines and runner/src/net/net_capture.cpp
writes/reads, and the SAME sanitizer algorithm runner/src/net/net_sanitize.
cpp implements (see net_sanitize.h's design comment for the full privacy
rationale). Any change to either C++ header's byte layout or the sanitizer's
mapping algorithm MUST be mirrored here -- there is no shared code between
the two implementations by design (this tool exists specifically so a
capture can be inspected/sanitized without a build), so keeping them in
sync is a manual discipline, not something the type system enforces.

Subcommands:

  dump PATH               Print every record (cycle, direction, length,
                           and, for anything recognized as DHCP, the
                           decoded message type/lease) plus the file header
                           (sanitized flag, scenario, rom_sha1).

  sanitize IN OUT          Read IN, rewrite console-identifying material
                           (MAC addresses, DHCP client-id, DHCP hostname)
                           into stable synthetic substitutes -- exactly
                           net_sanitize.cpp's algorithm, byte-for-byte
                           reproduced -- and write OUT with the header's
                           `sanitized` flag set to 1. A no-op (byte-
                           identical copy, header untouched) if IN is
                           already sanitized, UNLESS --force is given.

  check PATH               Exit 0 if PATH's header says sanitized=1; exit 1
                           (with a clear stderr message) otherwise. Intended
                           for a pre-commit hook or CI step: "refuse to let
                           an unsanitized capture anywhere a commit could
                           pick it up" -- the second, independent layer of
                           this milestone's privacy defense, on top of (not
                           instead of) sanitize-by-default at capture time
                           (see docs/m8-capture-replay-design.md).

  publish IN DEST          Copies IN to DEST ONLY if `check IN` would pass;
                           refuses (nonzero exit, DEST left untouched)
                           otherwise. This is the concrete "tooling refuses
                           to write a fixture into a tracked path unless it
                           is sanitized" mechanism the M8 task asks for.
"""

from __future__ import annotations

import argparse
import shutil
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"NDSNETREPLAY1" + b"\x00" * 3  # 16 bytes, matches kNdsNetCaptureMagic
FORMAT_VERSION = 1
MAX_FRAME_BYTES = 2048  # kNdsNetCaptureMaxFrameBytes

# struct NdsNetCaptureFileHeader (net_capture.h), #pragma pack(1):
#   char magic[16]; u32 format_version; u32 header_size; u8 sanitized;
#   u8 reserved0[3]; u64 created_unix_time; char rom_sha1[41];
#   char scenario[64];
HEADER_FMT = "<16sIIB3sQ41s64s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 141, f"header layout drifted from net_capture.h: {HEADER_SIZE}"

# struct NdsNetCaptureRecordHeader: u64 guest_cycle; u32 len; u8 direction;
# u8 reserved[3].
RECORD_FMT = "<QIB3s"
RECORD_SIZE = struct.calcsize(RECORD_FMT)
assert RECORD_SIZE == 16

DIR_TX = 0
DIR_RX = 1


@dataclass
class CaptureHeader:
    format_version: int
    sanitized: bool
    created_unix_time: int
    rom_sha1: str
    scenario: str


@dataclass
class Record:
    guest_cycle: int
    direction: int
    frame: bytes


@dataclass
class Capture:
    header: CaptureHeader
    records: list[Record] = field(default_factory=list)


def _cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def read_capture(path: Path) -> Capture:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError(
            f"{path}: truncated header: expected {HEADER_SIZE} bytes, got {len(data)}")
    magic, fmt_version, header_size, sanitized, _reserved0, created, rom_sha1, scenario = \
        struct.unpack_from(HEADER_FMT, data, 0)
    if magic != MAGIC:
        raise ValueError(f"{path}: bad magic -- not an NDSNETREPLAY1 capture")
    if fmt_version != FORMAT_VERSION:
        raise ValueError(
            f"{path}: unsupported format_version {fmt_version} (expected {FORMAT_VERSION})")
    if header_size != HEADER_SIZE:
        raise ValueError(
            f"{path}: header_size mismatch: file says {header_size}, reader expects {HEADER_SIZE}")

    header = CaptureHeader(
        format_version=fmt_version,
        sanitized=bool(sanitized),
        created_unix_time=created,
        rom_sha1=_cstr(rom_sha1),
        scenario=_cstr(scenario),
    )
    cap = Capture(header=header)

    pos = HEADER_SIZE
    index = 0
    while pos < len(data):
        remaining = len(data) - pos
        if remaining < RECORD_SIZE:
            raise ValueError(
                f"{path}: truncated record header at record {index}: "
                f"expected {RECORD_SIZE} bytes, got {remaining}")
        guest_cycle, length, direction, _reserved = struct.unpack_from(
            RECORD_FMT, data, pos)
        pos += RECORD_SIZE
        if length > MAX_FRAME_BYTES:
            raise ValueError(
                f"{path}: corrupt record {index}: length field {length} "
                f"exceeds max frame size {MAX_FRAME_BYTES}")
        if direction not in (DIR_TX, DIR_RX):
            raise ValueError(
                f"{path}: corrupt record {index}: invalid direction byte {direction}")
        if len(data) - pos < length:
            raise ValueError(
                f"{path}: truncated record {index}: expected {length} "
                f"payload bytes, got {len(data) - pos}")
        frame = data[pos:pos + length]
        pos += length
        cap.records.append(Record(guest_cycle, direction, frame))
        index += 1
    return cap


def write_capture(path: Path, cap: Capture) -> None:
    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        cap.header.format_version,
        HEADER_SIZE,
        1 if cap.header.sanitized else 0,
        b"\x00\x00\x00",
        cap.header.created_unix_time,
        cap.header.rom_sha1.encode("ascii")[:40].ljust(41, b"\x00"),
        cap.header.scenario.encode("ascii")[:63].ljust(64, b"\x00"),
    )
    out = bytearray(header)
    for rec in cap.records:
        out += struct.pack(RECORD_FMT, rec.guest_cycle, len(rec.frame),
                            rec.direction, b"\x00\x00\x00")
        out += rec.frame
    path.write_bytes(bytes(out))


# ---- Sanitizer: a byte-for-byte port of net_sanitize.cpp -------------------

def _fnv1a64(data: bytes) -> int:
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


DIR_TX_SANITIZE = 0
DIR_RX_SANITIZE = 1


class SanitizeState:
    """Port of NdsNetSanitizeState (net_sanitize.h/.cpp) -- identity-based
    design: rewrites EXACTLY ONE learned MAC (the console's own, learned
    from the first TX-direction frame's Ethernet source), never "every
    MAC-shaped field." See net_sanitize.h's top comment for why: broadcast/
    multicast addresses and this project's own virtual AP/DHCP-server MACs
    are not console-identifying, and rewriting them breaks WifiAP's own
    destination-address filter (confirmed empirically)."""

    def __init__(self):
        self._has_identity = False
        self._identity_mac: bytes = b""
        self._mac_cache: dict[bytes, bytes] = {}
        self._hostname_cache: dict[bytes, bytes] = {}

    def learn_identity_from_tx_source(self, eth_src: bytes) -> None:
        if self._has_identity:
            return
        self._identity_mac = eth_src
        self._has_identity = True

    def has_identity(self) -> bool:
        return self._has_identity

    def identity_mac(self) -> bytes:
        return self._identity_mac

    def map_mac(self, mac: bytes) -> bytes:
        assert len(mac) == 6
        cached = self._mac_cache.get(mac)
        if cached is not None:
            return cached
        h = _fnv1a64(mac)
        out = bytes([0x02] + [(h >> (8 * i)) & 0xFF for i in range(5)])
        self._mac_cache[mac] = out
        return out

    def map_hostname(self, data: bytes) -> bytes:
        cached = self._hostname_cache.get(data)
        if cached is not None:
            return cached
        tag = f"ndsguest-{_fnv1a64(data) & 0xFFFFFFFF:08x}".encode("ascii")
        out = bytes(tag[i % len(tag)] for i in range(len(data)))
        self._hostname_cache[data] = out
        return out


def _rd16(b: bytes, off: int) -> int:
    return (b[off] << 8) | b[off + 1]


def _rd32(b: bytes, off: int) -> int:
    return (b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]


def _udp_checksum(src_ipv4: int, dst_ipv4: int, udp: bytes) -> int:
    total = 0
    total += (src_ipv4 >> 16) & 0xFFFF
    total += src_ipv4 & 0xFFFF
    total += (dst_ipv4 >> 16) & 0xFFFF
    total += dst_ipv4 & 0xFFFF
    total += 17
    total += len(udp)
    i = 0
    while i + 1 < len(udp):
        if i != 6:
            total += (udp[i] << 8) | udp[i + 1]
        i += 2
    if i < len(udp):
        total += udp[i] << 8
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    result = (~total) & 0xFFFF
    return result if result != 0 else 0xFFFF


def sanitize_frame(frame: bytes, direction: int, state: SanitizeState) -> bytes:
    """Byte-for-byte port of net_sanitize_ethernet_frame (identity-based
    design -- see net_sanitize.h/SanitizeState's doc comments). Returns a
    (possibly) rewritten COPY; never mutates the input. `direction` is
    DIR_TX_SANITIZE/DIR_RX_SANITIZE -- used only to learn the identity MAC
    from a TX frame's Ethernet source the first time one is seen."""
    if len(frame) < 14:
        return frame
    f = bytearray(frame)

    if direction == DIR_TX_SANITIZE and not state.has_identity():
        state.learn_identity_from_tx_source(bytes(f[6:12]))

    have_identity = state.has_identity()
    identity = state.identity_mac() if have_identity else b""
    synthetic = state.map_mac(identity) if have_identity else b""

    def rewrite_if_identity(off: int) -> None:
        nonlocal f
        if not have_identity:
            return
        if bytes(f[off:off + 6]) == identity:
            f[off:off + 6] = synthetic

    rewrite_if_identity(0)  # Ethernet dst
    rewrite_if_identity(6)  # Ethernet src

    ethertype = _rd16(f, 12)
    if ethertype == 0x0806:
        if len(f) >= 14 + 28:
            arp = 14
            rewrite_if_identity(arp + 8)   # sender hardware address
            rewrite_if_identity(arp + 18)  # target hardware address
        return bytes(f)

    if ethertype != 0x0800 or len(f) < 14 + 20:
        return bytes(f)

    ip = 14
    version = f[ip] >> 4
    ihl = (f[ip] & 0x0F) * 4
    if version != 4 or ihl < 20 or ip + ihl > len(f):
        return bytes(f)
    if f[ip + 9] != 17:  # UDP only
        return bytes(f)

    src_ipv4 = _rd32(f, ip + 12)
    dst_ipv4 = _rd32(f, ip + 16)
    udp_off = ip + ihl
    if udp_off + 8 > len(f):
        return bytes(f)

    src_port = _rd16(f, udp_off + 0)
    dst_port = _rd16(f, udp_off + 2)
    if not (src_port in (67, 68) or dst_port in (67, 68)):
        return bytes(f)

    udp_declared_len = _rd16(f, udp_off + 4)
    udp_avail = len(f) - udp_off
    udp_len = min(udp_declared_len, udp_avail)
    if udp_len < 8:
        return bytes(f)

    bootp_off = udp_off + 8
    bootp_len = udp_len - 8
    payload_changed = False

    if bootp_len >= 34 and have_identity:
        htype = f[bootp_off + 1]
        hlen = f[bootp_off + 2]
        if htype == 1 and hlen == 6:
            chaddr = bytes(f[bootp_off + 28:bootp_off + 34])
            if chaddr == identity and chaddr != synthetic:
                f[bootp_off + 28:bootp_off + 34] = synthetic
                payload_changed = True

    cookie = bootp_off + 236
    bootp_end = min(bootp_off + bootp_len, len(f))
    if (cookie + 4 <= bootp_end and f[cookie] == 0x63 and f[cookie + 1] == 0x82
            and f[cookie + 2] == 0x53 and f[cookie + 3] == 0x63):
        p = cookie + 4
        while p < bootp_end:
            code = f[p]
            if code == 0xFF:
                break
            if code == 0x00:
                p += 1
                continue
            if p + 1 >= bootp_end:
                break
            opt_len = f[p + 1]
            if p + 2 + opt_len > bootp_end:
                break
            val_off = p + 2
            val = bytes(f[val_off:val_off + opt_len])

            if (have_identity and code == 61 and opt_len == 7 and val[0] == 1
                    and val[1:7] == identity):
                if val[1:7] != synthetic:
                    f[val_off + 1:val_off + 7] = synthetic
                    payload_changed = True
            elif code == 12 and opt_len > 0:
                repl = state.map_hostname(val)
                if repl != val:
                    f[val_off:val_off + opt_len] = repl
                    payload_changed = True
            p += 2 + opt_len

    if payload_changed:
        orig_checksum = _rd16(f, udp_off + 6)
        if orig_checksum != 0:
            f[udp_off + 6] = 0
            f[udp_off + 7] = 0
            new_csum = _udp_checksum(src_ipv4, dst_ipv4, bytes(f[udp_off:udp_off + udp_len]))
            f[udp_off + 6] = (new_csum >> 8) & 0xFF
            f[udp_off + 7] = new_csum & 0xFF

    return bytes(f)


def decode_dhcp_summary(frame: bytes) -> str | None:
    """Best-effort human summary for `dump` -- not used by sanitize/check."""
    if len(frame) < 42:
        return None
    if _rd16(frame, 12) != 0x0800 or frame[14 + 9] != 17:
        return None
    ihl = (frame[14] & 0x0F) * 4
    udp_off = 14 + ihl
    if udp_off + 8 > len(frame):
        return None
    src_port, dst_port = _rd16(frame, udp_off), _rd16(frame, udp_off + 2)
    if not (src_port in (67, 68) or dst_port in (67, 68)):
        return None
    bootp_off = udp_off + 8
    if bootp_off + 240 > len(frame):
        return None
    yiaddr = _rd32(frame, bootp_off + 16)
    msg_type = 0
    cookie = bootp_off + 236
    if frame[cookie:cookie + 4] == b"\x63\x82\x53\x63":
        p = cookie + 4
        while p + 1 < len(frame):
            code = frame[p]
            if code == 0xFF:
                break
            if code == 0x00:
                p += 1
                continue
            opt_len = frame[p + 1]
            if code == 53 and opt_len >= 1:
                msg_type = frame[p + 2]
            p += 2 + opt_len
    names = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE", 5: "ACK",
             6: "NAK", 7: "RELEASE", 8: "INFORM"}
    yi = f"{(yiaddr >> 24) & 0xFF}.{(yiaddr >> 16) & 0xFF}.{(yiaddr >> 8) & 0xFF}.{yiaddr & 0xFF}"
    return f"DHCP {names.get(msg_type, f'type={msg_type}')} yiaddr={yi}"


# ---- CLI --------------------------------------------------------------------

def cmd_dump(args: argparse.Namespace) -> int:
    cap = read_capture(Path(args.path))
    h = cap.header
    print(f"format_version={h.format_version} sanitized={h.sanitized} "
          f"scenario={h.scenario!r} rom_sha1={h.rom_sha1!r} "
          f"created={time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(h.created_unix_time)) if h.created_unix_time else '?'}")
    print(f"{len(cap.records)} record(s)")
    for i, rec in enumerate(cap.records):
        dirname = "TX" if rec.direction == DIR_TX else "RX"
        extra = decode_dhcp_summary(rec.frame)
        print(f"  #{i:04d} cycle={rec.guest_cycle:<12d} dir={dirname} "
              f"len={len(rec.frame):<5d}" + (f"  {extra}" if extra else ""))
    return 0


def cmd_sanitize(args: argparse.Namespace) -> int:
    src = Path(args.input)
    dst = Path(args.output)
    cap = read_capture(src)
    if cap.header.sanitized and not args.force:
        print(f"{src}: already sanitized (pass --force to re-sanitize anyway)",
              file=sys.stderr)
        if src.resolve() != dst.resolve():
            shutil.copyfile(src, dst)
        return 0

    state = SanitizeState()
    changed = 0
    for rec in cap.records:
        new_frame = sanitize_frame(rec.frame, rec.direction, state)
        if new_frame != rec.frame:
            changed += 1
        rec.frame = new_frame
    cap.header.sanitized = True
    write_capture(dst, cap)
    print(f"{src} -> {dst}: sanitized ({changed}/{len(cap.records)} record(s) "
          f"rewritten)")
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    cap = read_capture(Path(args.path))
    if cap.header.sanitized:
        print(f"{args.path}: sanitized=1 (OK)")
        return 0
    print(f"{args.path}: sanitized=0 -- REFUSING. Run "
          f"'net_capture_tool.py sanitize {args.path} <out>' first.",
          file=sys.stderr)
    return 1


def cmd_publish(args: argparse.Namespace) -> int:
    src = Path(args.input)
    dst = Path(args.dest)
    cap = read_capture(src)
    if not cap.header.sanitized:
        print(f"REFUSING to publish '{src}' to '{dst}': capture is not "
              f"sanitized (header sanitized=0). This is the M8 privacy "
              f"guard -- see docs/m8-capture-replay-design.md. Run "
              f"'net_capture_tool.py sanitize {src} <tmp>' first, then "
              f"publish the sanitized copy.", file=sys.stderr)
        return 1
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    print(f"published '{src}' -> '{dst}' (sanitized=1)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_dump = sub.add_parser("dump", help="print every record in a capture")
    p_dump.add_argument("path")
    p_dump.set_defaults(func=cmd_dump)

    p_san = sub.add_parser("sanitize", help="write a sanitized copy of a capture")
    p_san.add_argument("input")
    p_san.add_argument("output")
    p_san.add_argument("--force", action="store_true",
                        help="re-sanitize even if already marked sanitized")
    p_san.set_defaults(func=cmd_sanitize)

    p_check = sub.add_parser("check", help="exit nonzero unless sanitized=1")
    p_check.add_argument("path")
    p_check.set_defaults(func=cmd_check)

    p_pub = sub.add_parser(
        "publish", help="copy INPUT to DEST only if sanitized=1, else refuse")
    p_pub.add_argument("input")
    p_pub.add_argument("dest")
    p_pub.set_defaults(func=cmd_publish)

    args = parser.parse_args()
    try:
        return args.func(args)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
