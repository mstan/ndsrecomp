#!/usr/bin/env python3
"""Collect post-run evidence from two live MKDS ndsrecomp instances.

Use this during or after the owner-driven Friend Roster attempt. It does not
advance the guest, press buttons, or arm tracing; it only queries the always-on
rings and captures the current framebuffers from the two debug ports.

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


def infer_client_ip(instance: dict[str, Any]) -> str | None:
    votes: Counter[str] = Counter()
    for kind in ("dns_query", "tcp_open", "tcp_packet", "udp_packet"):
        for event in instance["kinds"].get(kind, []):
            if event.get("direction") == 0:
                src_ip = event.get("src_ip")
                if src_ip and src_ip != "0.0.0.0":
                    votes[src_ip] += 1
    if votes:
        return votes.most_common(1)[0][0]

    # DHCP offer/ack entries carry the leased address in wifi_value and as the
    # destination IPv4 in the ring event. Use that as a fallback for early
    # captures that happen before DNS/TCP/UDP traffic exists.
    for event in instance["kinds"].get("dhcp", []):
        if event.get("direction") == 1:
            dst_ip = event.get("dst_ip")
            if dst_ip and dst_ip != "0.0.0.0" and dst_ip != "255.255.255.255":
                return dst_ip
    return None


def summarize_local_candidate_udp(instance: dict[str, Any],
                                  local_ip: str | None
                                  ) -> list[dict[str, Any]]:
    if not local_ip:
        return []
    out = []
    for event in instance["udp_summary"]["candidate_peer_udp"]:
        src_ip = event.get("src_ip")
        dst_ip = event.get("dst_ip")
        if src_ip == local_ip:
            out.append({
                "direction": "out",
                "local_ip": src_ip,
                "local_port": event.get("src_port"),
                "remote_ip": dst_ip,
                "remote_port": event.get("dst_port"),
                "count": event.get("count"),
                "payload_len": event.get("payload_len"),
            })
        elif dst_ip == local_ip:
            out.append({
                "direction": "in",
                "local_ip": dst_ip,
                "local_port": event.get("dst_port"),
                "remote_ip": src_ip,
                "remote_port": event.get("src_port"),
                "count": event.get("count"),
                "payload_len": event.get("payload_len"),
            })
    return out


def correlate_instances(instances: list[dict[str, Any]]) -> dict[str, Any]:
    if len(instances) != 2:
        return {}
    a, b = instances
    ip_a = infer_client_ip(a)
    ip_b = infer_client_ip(b)
    peer_ips = {ip for ip in (ip_a, ip_b) if ip}

    direct = []
    for label, instance in (("A", a), ("B", b)):
        for event in instance["udp_summary"]["candidate_peer_udp"]:
            src_ip = event.get("src_ip")
            dst_ip = event.get("dst_ip")
            if src_ip in peer_ips and dst_ip in peer_ips and src_ip != dst_ip:
                direct.append({
                    "observed_by": label,
                    "count": event.get("count"),
                    "direction": event.get("direction"),
                    "src_ip": src_ip,
                    "src_port": event.get("src_port"),
                    "dst_ip": dst_ip,
                    "dst_port": event.get("dst_port"),
                    "payload_len": event.get("payload_len"),
                })

    local_a = summarize_local_candidate_udp(a, ip_a)
    local_b = summarize_local_candidate_udp(b, ip_b)
    endpoints_a = Counter(
        (e["remote_ip"], e["remote_port"]) for e in local_a
    )
    endpoints_b = Counter(
        (e["remote_ip"], e["remote_port"]) for e in local_b
    )
    shared = [
        {
            "remote_ip": ip,
            "remote_port": port,
            "a_events": endpoints_a[(ip, port)],
            "b_events": endpoints_b[(ip, port)],
        }
        for ip, port in sorted(set(endpoints_a) & set(endpoints_b))
    ]

    return {
        "inferred_client_ips": {"A": ip_a, "B": ip_b},
        "direct_client_udp": direct[:128],
        "direct_client_udp_count": len(direct),
        "shared_candidate_peer_endpoints": shared[:64],
        "shared_candidate_peer_endpoint_count": len(shared),
        "local_candidate_udp": {
            "A": local_a[:128],
            "B": local_b[:128],
        },
    }


def collect_instance(label: str, port: int, out_dir: Path,
                     max_per_kind: int, screenshot_prefix: str = ""
                     ) -> dict[str, Any]:
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
        screenshot = out_dir / f"{screenshot_prefix}{label}_framebuffer.png"
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


def print_correlation(correlation: dict[str, Any]) -> None:
    if not correlation:
        return
    ips = correlation.get("inferred_client_ips", {})
    print(
        "correlation: "
        f"A_ip={ips.get('A')} B_ip={ips.get('B')} "
        f"direct_client_udp={correlation.get('direct_client_udp_count', 0)} "
        "shared_candidate_peer_endpoints="
        f"{correlation.get('shared_candidate_peer_endpoint_count', 0)}"
    )
    for endpoint in correlation.get("shared_candidate_peer_endpoints", [])[:8]:
        print(
            "  shared endpoint "
            f"{endpoint['remote_ip']}:{endpoint['remote_port']} "
            f"A={endpoint['a_events']} B={endpoint['b_events']}"
        )


def collect_report(port_a: int, port_b: int, out_dir: Path, max_per_kind: int,
                   created_at: str, screenshot_prefix: str = ""
                   ) -> dict[str, Any]:
    report = {
        "created_at": created_at,
        "ports": {"A": port_a, "B": port_b},
        "instances": [
            collect_instance("A", port_a, out_dir, max_per_kind, screenshot_prefix),
            collect_instance("B", port_b, out_dir, max_per_kind, screenshot_prefix),
        ],
    }
    report["correlation"] = correlate_instances(report["instances"])
    return report


def compact_snapshot(snapshot_index: int, snapshot_path: Path,
                     report: dict[str, Any], elapsed_sec: float
                     ) -> dict[str, Any]:
    return {
        "snapshot": snapshot_index,
        "elapsed_sec": round(elapsed_sec, 3),
        "created_at": report["created_at"],
        "path": str(snapshot_path),
        "correlation": report.get("correlation", {}),
        "instances": [
            {
                "label": instance["label"],
                "port": instance["port"],
                "event_counts": instance["event_counts"],
                "net_state": instance["net_state"],
                "kind_counts": instance["kind_counts"],
                "udp_counts": instance["udp_summary"]["counts"],
                "candidate_peer_udp": instance["udp_summary"]["candidate_peer_udp"],
                "wfc_service_udp": instance["udp_summary"]["wfc_service_udp"],
                "framebuffer_path": instance.get("framebuffer_path"),
            }
            for instance in report["instances"]
        ],
    }


def write_single_snapshot(args: argparse.Namespace, out_dir: Path,
                          stamp: str) -> int:
    report = collect_report(
        args.port_a, args.port_b, out_dir, args.max_per_kind, stamp)
    report_path = out_dir / "evidence.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    for instance in report["instances"]:
        print_summary(instance)
    print_correlation(report.get("correlation", {}))
    print(f"wrote {report_path}")
    return 0


def watch(args: argparse.Namespace, out_dir: Path, stamp: str) -> int:
    snapshots_dir = out_dir / "snapshots"
    snapshots_dir.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    index: list[dict[str, Any]] = []
    snapshot_index = 0
    while True:
        now = time.monotonic()
        elapsed = now - started
        if snapshot_index > 0 and elapsed > args.watch_seconds:
            break

        created_at = time.strftime("%Y%m%d-%H%M%S")
        prefix = f"snapshot_{snapshot_index:03d}_"
        report = collect_report(
            args.port_a, args.port_b, snapshots_dir, args.max_per_kind,
            created_at, screenshot_prefix=prefix)
        snapshot_path = snapshots_dir / f"snapshot_{snapshot_index:03d}.json"
        snapshot_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        index.append(compact_snapshot(snapshot_index, snapshot_path, report, elapsed))

        print(f"\n--- snapshot {snapshot_index} elapsed={elapsed:.1f}s ---")
        for instance in report["instances"]:
            print_summary(instance)
        print_correlation(report.get("correlation", {}))

        snapshot_index += 1
        remaining = args.watch_seconds - (time.monotonic() - started)
        if remaining <= 0:
            break
        time.sleep(min(args.interval, remaining))

    summary = {
        "created_at": stamp,
        "watch_seconds": args.watch_seconds,
        "interval": args.interval,
        "ports": {"A": args.port_a, "B": args.port_b},
        "snapshots": index,
    }
    report_path = out_dir / "evidence.json"
    report_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"wrote {report_path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port-a", type=int, default=19860)
    parser.add_argument("--port-b", type=int, default=19861)
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--max-per-kind", type=int, default=4096)
    parser.add_argument("--watch-seconds", type=float, default=0.0,
                        help="collect repeated snapshots for this many seconds")
    parser.add_argument("--interval", type=float, default=5.0,
                        help="seconds between watch-mode snapshots")
    args = parser.parse_args()
    if args.max_per_kind < 1 or args.max_per_kind > 4096:
        parser.error("--max-per-kind must be in 1..4096")
    if args.interval <= 0:
        parser.error("--interval must be positive")
    if args.watch_seconds < 0:
        parser.error("--watch-seconds must be non-negative")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = args.out_dir or (
        default_game_root() / "generated" / "captures" /
        f"m7-two-instance-evidence-{stamp}"
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.watch_seconds > 0:
        return watch(args, out_dir, stamp)
    return write_single_snapshot(args, out_dir, stamp)


if __name__ == "__main__":
    raise SystemExit(main())
