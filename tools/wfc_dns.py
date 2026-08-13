#!/usr/bin/env python3
"""Tiny scoped DNS responder for local Nintendo WFC tests.

Answers A-record queries under a configured suffix with one IPv4 address and
refuses everything else. This is intentionally not a recursive resolver.
"""

from __future__ import annotations

import argparse
import ipaddress
import socket
import struct
from typing import Tuple


def read_name(packet: bytes, offset: int) -> Tuple[str, int]:
    labels: list[str] = []
    jumped = False
    end = offset
    seen = set()
    while True:
        if offset >= len(packet):
            raise ValueError("name exceeds packet")
        length = packet[offset]
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(packet):
                raise ValueError("truncated compression pointer")
            ptr = ((length & 0x3F) << 8) | packet[offset + 1]
            if ptr in seen:
                raise ValueError("compression loop")
            seen.add(ptr)
            if not jumped:
                end = offset + 2
                jumped = True
            offset = ptr
            continue
        if length & 0xC0:
            raise ValueError("unsupported label type")
        offset += 1
        if length == 0:
            if not jumped:
                end = offset
            return ".".join(labels), end
        if offset + length > len(packet):
            raise ValueError("truncated label")
        labels.append(packet[offset:offset + length].decode("ascii", "ignore"))
        offset += length


def make_response(packet: bytes, answer: bytes, suffix: str) -> bytes:
    if len(packet) < 12:
        raise ValueError("truncated header")
    tid, flags, qdcount, _, _, _ = struct.unpack("!HHHHHH", packet[:12])
    if qdcount != 1:
        return struct.pack("!HHHHHH", tid, 0x8000 | (flags & 0x0100) | 5,
                           qdcount, 0, 0, 0) + packet[12:]

    qname, qend = read_name(packet, 12)
    if qend + 4 > len(packet):
        raise ValueError("truncated question")
    qtype, qclass = struct.unpack("!HH", packet[qend:qend + 4])
    question = packet[12:qend + 4]
    qname_l = qname.lower().rstrip(".")
    suffix_l = suffix.lower().rstrip(".")
    ok = (qtype == 1 and qclass == 1 and
          (qname_l == suffix_l or qname_l.endswith("." + suffix_l)))
    if not ok:
        return struct.pack("!HHHHHH", tid, 0x8000 | (flags & 0x0100) | 5,
                           1, 0, 0, 0) + question

    rr = b"\xC0\x0C" + struct.pack("!HHIH", 1, 1, 60, 4) + answer
    return struct.pack("!HHHHHH", tid, 0x8000 | (flags & 0x0100) | 0x0080,
                       1, 1, 0, 0) + question + rr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=53)
    parser.add_argument("--answer", default="10.64.0.1")
    parser.add_argument("--suffix", default="nintendowifi.net")
    args = parser.parse_args()

    answer = ipaddress.IPv4Address(args.answer).packed
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    print(f"wfc_dns: listening on {args.bind}:{args.port}, "
          f"answering *.{args.suffix} with {args.answer}", flush=True)
    while True:
        data, peer = sock.recvfrom(512)
        try:
            response = make_response(data, answer, args.suffix)
        except ValueError:
            continue
        sock.sendto(response, peer)


if __name__ == "__main__":
    raise SystemExit(main())
