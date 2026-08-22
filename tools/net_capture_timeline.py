#!/usr/bin/env python3
"""Decode an NDSNETREPLAY1 capture into a human-readable protocol timeline.

Companion to net_capture_tool.py (which owns the byte-layout parsing --
imported here, never duplicated): where `dump` prints raw records and only
decodes DHCP, this prints one line per frame with ARP/DHCP/DNS/TCP/UDP/TLS
decode -- DNS question names, DHCP message types, TCP flags and ports --
so two connection attempts in one capture can be diffed by eye. Built for
beads-lqa.8 (WFC reconnect 52200) but generic to any capture.

Usage: net_capture_timeline.py PATH [--from-cycle N] [--to-cycle N]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from net_capture_tool import read_capture  # noqa: E402

DHCP_TYPES = {
    1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE",
    5: "ACK", 6: "NAK", 7: "RELEASE", 8: "INFORM",
}

TCP_FLAG_BITS = (
    (0x01, "FIN"), (0x02, "SYN"), (0x04, "RST"),
    (0x08, "PSH"), (0x10, "ACK"), (0x20, "URG"),
)


def ipv4(b: bytes) -> str:
    return ".".join(str(x) for x in b)


def dns_qname(payload: bytes) -> str:
    # payload = DNS message; question section starts at offset 12.
    try:
        out = []
        pos = 12
        while True:
            n = payload[pos]
            if n == 0:
                break
            if n >= 0xC0:  # compression pointer; stop
                out.append("...")
                break
            pos += 1
            out.append(payload[pos:pos + n].decode("ascii", errors="replace"))
            pos += n
        return ".".join(out)
    except IndexError:
        return "<truncated>"


def describe(frame: bytes) -> str:
    if len(frame) < 14:
        return f"short frame ({len(frame)}B)"
    ethertype = int.from_bytes(frame[12:14], "big")
    if ethertype == 0x0806:
        if len(frame) >= 42:
            op = int.from_bytes(frame[20:22], "big")
            return (f"ARP {'req' if op == 1 else 'reply'} "
                    f"{ipv4(frame[28:32])} -> {ipv4(frame[38:42])}")
        return "ARP (short)"
    if ethertype != 0x0800:
        return f"ethertype 0x{ethertype:04X} ({len(frame)}B)"
    ip = frame[14:]
    if len(ip) < 20:
        return "IPv4 (truncated)"
    ihl = (ip[0] & 0x0F) * 4
    proto = ip[9]
    src, dst = ipv4(ip[12:16]), ipv4(ip[16:20])
    l4 = ip[ihl:]
    if proto == 17 and len(l4) >= 8:  # UDP
        sport = int.from_bytes(l4[0:2], "big")
        dport = int.from_bytes(l4[2:4], "big")
        payload = l4[8:]
        base = f"UDP {src}:{sport} -> {dst}:{dport} len={len(payload)}"
        if sport in (67, 68) or dport in (67, 68):
            mtype = "?"
            # DHCP options after 236-byte fixed header + 4-byte cookie.
            opts = payload[240:]
            i = 0
            while i < len(opts):
                code = opts[i]
                if code == 0xFF or code == 0:
                    if code == 0xFF:
                        break
                    i += 1
                    continue
                length = opts[i + 1] if i + 1 < len(opts) else 0
                if code == 53 and length == 1 and i + 2 < len(opts):
                    mtype = DHCP_TYPES.get(opts[i + 2], str(opts[i + 2]))
                i += 2 + length
            yiaddr = ipv4(payload[16:20]) if len(payload) >= 20 else "?"
            return f"DHCP {mtype} yiaddr={yiaddr} ({base})"
        if sport == 53 or dport == 53:
            if len(payload) >= 12:
                flags = int.from_bytes(payload[2:4], "big")
                kind = "resp" if flags & 0x8000 else "query"
                rcode = flags & 0x000F
                ancount = int.from_bytes(payload[6:8], "big")
                extra = (f" rcode={rcode} answers={ancount}"
                         if kind == "resp" else "")
                return f"DNS {kind} '{dns_qname(payload)}'{extra} ({base})"
            return f"DNS (truncated) ({base})"
        return base
    if proto == 6 and len(l4) >= 20:  # TCP
        sport = int.from_bytes(l4[0:2], "big")
        dport = int.from_bytes(l4[2:4], "big")
        doff = (l4[12] >> 4) * 4
        flags = l4[13]
        names = "".join(n for bit, n in TCP_FLAG_BITS if flags & bit) or "-"
        payload_len = len(l4) - doff
        tls = ""
        if payload_len >= 5 and l4[doff] in (20, 21, 22, 23) and l4[doff + 1] == 3:
            tls = f" TLS(type={l4[doff]})"
        return (f"TCP {names} {src}:{sport} -> {dst}:{dport} "
                f"payload={payload_len}{tls}")
    if proto == 1:
        return f"ICMP {src} -> {dst}"
    return f"IPv4 proto={proto} {src} -> {dst}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--from-cycle", type=int, default=0)
    parser.add_argument("--to-cycle", type=int, default=None)
    args = parser.parse_args()

    cap = read_capture(args.path)
    print(f"# scenario='{cap.header.scenario}' sanitized={cap.header.sanitized} "
          f"records={len(cap.records)}")
    for i, rec in enumerate(cap.records):
        if rec.guest_cycle < args.from_cycle:
            continue
        if args.to_cycle is not None and rec.guest_cycle > args.to_cycle:
            continue
        arrow = "TX" if rec.direction == 0 else "RX"
        print(f"{i:5d} {rec.guest_cycle:>16d} {arrow} {describe(rec.frame)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
