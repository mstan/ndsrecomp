"""Shared TCP client for the ndsrecomp debug protocol (see ../TCP.md).

Speaks line-delimited JSON to either the native runtime (port 19842) or
the melonDS oracle (port 19843). Used by the diff_* probes. Version-
independent of the melonDS-side patch — it only relies on the protocol.
"""

import json
import socket


class DebugClient:
    def __init__(self, host="127.0.0.1", port=19842, timeout=30.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def cmd(self, name, **args):
        args["cmd"] = name
        self.sock.sendall((json.dumps(args) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("debug server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        resp = json.loads(line)
        if isinstance(resp, dict) and resp.get("error"):
            raise RuntimeError(f"{name}: {resp['error']}")
        return resp

    # convenience wrappers
    def ping(self):
        return self.cmd("ping")

    def run_to_event(self, event, count):
        return self.cmd("run_to_event", event=event, count=count)

    def read_region(self, region):
        r = self.cmd("read_region", region=region)
        return bytes.fromhex(r["hex"] if isinstance(r, dict) else r)

    def framebuffer(self, engine):
        r = self.cmd("framebuffer", engine=engine)
        return r["w"], r["h"], bytes.fromhex(r["rgb"])

    def event_counts(self):
        return self.cmd("event_counts")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def parse_event(spec):
    """'vblank9:30' -> ('vblank9', 30)"""
    name, _, count = spec.partition(":")
    return name, int(count or 0)


def first_divergence(a, b):
    """Return (index, a_byte, b_byte) of the first differing byte, or None."""
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i, a[i], b[i]
    if len(a) != len(b):
        return n, (a[n] if n < len(a) else None), (b[n] if n < len(b) else None)
    return None
