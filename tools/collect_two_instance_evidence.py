#!/usr/bin/env python3
"""Collect post-run evidence from two live MKDS ndsrecomp instances.

Use this after the owner-driven Friend Roster attempt. It does not advance the
guest, press buttons, or arm tracing; it only queries the always-on rings and
captures the current framebuffers from the two debug ports.

Default ports match tools/run_two_instances.ps1.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Any

SCRIPT_PATH = Path(__file__.replace("\\", "/")).resolve()
ROOT = SCRIPT_PATH.parents[1]
sys.path.insert(0, str(ROOT / "oracle"))

from _client import DebugClient  # noqa: E402

try:
    from PIL import Image
except ImportError:  # pragma: no cover - Pillow is available in the game venv
    Image = None


EVIDENCE_KINDS = (
    "dhcp",
    "dns_query",
    "dns_response",
    "tcp_open",
    "tcp_close",
    "tcp_reset",
    "tcp_packet",
    "udp_packet",
    "tls_record",
    "backend_drop",
    "backend_error",
)
DROP_FIELDS = ("src_mac", "dst_mac")
LAN_NOISE_PORTS = {
    53, 67, 68, 137, 138, 1900, 3702, 5353, 5355, 6537, 9478, 9999,
    10101, 32412, 32414,
}
WFC_SERVICE_UDP_PORTS = {27900}


def ipv4(value: int) -> str:
    return ".".join(str((value >> shift) & 0xFF) for shift in (24, 16, 8, 0))


def is_multicast_or_broadcast(ip: str) -> bool:
    if ip == "255.255.255.255":
        return True
    parts = [int(p) for p in ip.split(".")]
    return parts[0] >= 224 or parts[-1] == 255


def sanitize_event(event: dict[str, Any]) -> dict[str, Any]:
    out = dict(event)
    for field in DROP_FIELDS:
        out.pop(field, None)
    if "src_ipv4" in out:
        out["src_ip"] = ipv4(out["src_ipv4"])
    if "dst_ipv4" in out:
        out["dst_ip"] = ipv4(out["dst_ipv4"])
    return out


def capture_framebuffer(client: DebugClient, out_path: Path) -> bool:
    if Image is None:
        return False
    try:
        w, h, rgb_a = client.framebuffer("A")
        wb, hb, rgb_b = client.framebuffer("B")
    except Exception as exc:  # Keep evidence collection best-effort.
        print(f"warning: framebuffer capture failed for {out_path}: {exc}")
        return False
    img = Image.new("RGB", (max(w, wb), h + hb))
    img.paste(Image.frombytes("RGB", (w, h), rgb_a), (0, 0))
    img.paste(Image.frombytes("RGB", (wb, hb), rgb_b), (0, h))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path)
    return True


def udp_bucket(event: dict[str, Any]) -> str:
    src_ip = event["src_ip"]
    dst_ip = event["dst_ip"]
    src_port = event.get("src_port", 0)
    dst_port = event.get("dst_port", 0)
    if src_port in WFC_SERVICE_UDP_PORTS or dst_port in WFC_SERVICE_UDP_PORTS:
        return "wfc_service_udp"
    if (
        src_port in LAN_NOISE_PORTS or dst_port in LAN_NOISE_PORTS or
        is_multicast_or_broadcast(src_ip) or is_multicast_or_broadcast(dst_ip)
    ):
        return "lan_noise_udp"
    return "candidate_peer_udp"


def summarize_udp(events: list[dict[str, Any]]) -> dict[str, Any]:
    buckets: dict[str, list[dict[str, Any]]] = {
        "wfc_service_udp": [],
        "candidate_peer_udp": [],
        "lan_noise_udp": [],
    }
    endpoint_counts: Counter[tuple[str, int, str, int]] = Counter()
    for event in events:
        bucket = udp_bucket(event)
        buckets[bucket].append(event)
        endpoint_counts[(
            event["src_ip"], event.get("src_port", 0),
            event["dst_ip"], event.get("dst_port", 0),
        )] += 1
    top_endpoints = [
        {
            "src_ip": src_ip,
            "src_port": src_port,
            "dst_ip": dst_ip,
            "dst_port": dst_port,
            "count": count,
        }
        for (src_ip, src_port, dst_ip, dst_port), count
        in endpoint_counts.most_common(32)
    ]
    return {
        "counts": {name: len(values) for name, values in buckets.items()},
        "candidate_peer_udp": buckets["candidate_peer_udp"][:128],
        "wfc_service_udp": buckets["wfc_service_udp"][:128],
        "top_udp_endpoints": top_endpoints,
    }


def collect_instance(label: str, port: int, out_dir: Path,
                     max_per_kind: int) -> dict[str, Any]:
    client = DebugClient(port=port, timeout=30.0)
    try:
        report: dict[str, Any] = {
            "label": label,
            "port": port,
            "ping": client.ping(),
            "event_counts": client.cmd("event_counts"),
            "net_state": client.cmd("net_state"),
            "kinds": {},
        }
        screenshot = out_dir / f"{label}_framebuffer.png"
        report["framebuffer_saved"] = capture_framebuffer(client, screenshot)
        report["framebuffer_path"] = str(screenshot) if report["framebuffer_saved"] else None

        for kind in EVIDENCE_KINDS:
            dump = client.cmd("net_ring_dump", max=max_per_kind, filter=kind)
            events = [sanitize_event(e) for e in dump.get("events", [])]
            report["kinds"][kind] = events

        report["kind_counts"] = {
            kind: len(events) for kind, events in report["kinds"].items()
        }
        report["udp_summary"] = summarize_udp(report["kinds"]["udp_packet"])
        return report
    finally:
        client.close()


def default_game_root() -> Path:
    cwd = Path.cwd()
    if (cwd / "game.toml").is_file() and (cwd / "ndsrecomp").is_dir():
        return cwd
    sibling = ROOT.parents[0] / "mariokartdsrecomp"
    if (sibling / "game.toml").is_file():
        return sibling
    return ROOT


def print_summary(report: dict[str, Any]) -> None:
    print(f"{report['label']} port {report['port']}:")
    counts = report["event_counts"]
    print(
        f"  vblank9={counts.get('vblank9')} vblank7={counts.get('vblank7')} "
        f"ipcsync_w={counts.get('ipcsync_w')}"
    )
    print(f"  net_state={report['net_state']}")
    nonzero = {
        kind: count for kind, count in report["kind_counts"].items() if count
    }
    print(f"  ring counts={nonzero}")
    udp = report["udp_summary"]["counts"]
    print(
        "  udp: "
        f"wfc_service={udp['wfc_service_udp']} "
        f"candidate_peer={udp['candidate_peer_udp']} "
        f"lan_noise={udp['lan_noise_udp']}"
    )
    backend_errors = report["kind_counts"].get("backend_error", 0)
    if backend_errors:
        print(f"  backend_error={backend_errors}")
    if report.get("framebuffer_saved"):
        print(f"  framebuffer={report['framebuffer_path']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port-a", type=int, default=19860)
    parser.add_argument("--port-b", type=int, default=19861)
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--max-per-kind", type=int, default=4096)
    args = parser.parse_args()

    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = args.out_dir or (
        default_game_root() / "generated" / "captures" /
        f"m7-two-instance-evidence-{stamp}"
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    report = {
        "created_at": stamp,
        "ports": {"A": args.port_a, "B": args.port_b},
        "instances": [
            collect_instance("A", args.port_a, out_dir, args.max_per_kind),
            collect_instance("B", args.port_b, out_dir, args.max_per_kind),
        ],
    }
    report_path = out_dir / "evidence.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    for instance in report["instances"]:
        print_summary(instance)
    print(f"wrote {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
