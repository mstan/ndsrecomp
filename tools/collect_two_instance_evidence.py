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
    from PIL import Image, ImageChops, ImageStat
except ImportError:  # pragma: no cover - Pillow is available in the game venv
    Image = None
    ImageChops = None
    ImageStat = None


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
NATNEG_UDP_PORTS = {27901}
REFS_DIR = ROOT / "oracle" / "wfc_screen_refs"
SCREEN_REFS = (
    "title_screen",
    "nickname_confirm_dialog",
    "wfc_connection_menu",
    "wfc_connection_setup_step1",
    "connection_test_running",
    "connection_test_settled",
    "connection_test_error",
    "wfc_match_disclaimer",
    "wfc_connecting",
    "wfc_match_save_confirm",
    "wfc_login_settled",
    "wfc_login_next",
    "wfc_match_setup_screen",
)
REGION_FOR = {
    "title_screen": "full",
    "nickname_confirm_dialog": "full",
    "wfc_connection_menu": "full",
    "wfc_connection_setup_step1": "full",
    "connection_test_running": "test_band",
    "connection_test_settled": "test_band",
    "connection_test_error": "test_band",
    "wfc_match_disclaimer": "full",
    "wfc_connecting": "full",
    "wfc_match_save_confirm": "full",
    "wfc_login_settled": "top",
    "wfc_login_next": "top",
    "wfc_match_setup_screen": "top",
}
THRESHOLD_FOR = {
    "wfc_login_settled": 20.0,
    "wfc_login_next": 20.0,
    "wfc_match_setup_screen": 20.0,
}
DEFAULT_SCREEN_THRESHOLD = 10.0
MIN_SCREEN_MARGIN = 2.0

_ref_cache: dict[str, Any] = {}


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


def load_ref(name: str):
    img = _ref_cache.get(name)
    if img is None:
        img = Image.open(REFS_DIR / f"{name}.png").convert("RGB")
        _ref_cache[name] = img
    return img


def gray_region(img, region: str):
    if region == "top":
        cropped = img.crop((0, 0, 256, 192))
    elif region == "test_band":
        cropped = img.crop((0, 230, 256, 344))
    else:
        cropped = img
    return cropped.convert("L")


def mean_abs_diff(img_a, img_b, region: str) -> float:
    a = gray_region(img_a, region)
    b = gray_region(img_b, region)
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]


def classify_screen(img) -> dict[str, Any]:
    if Image is None or ImageChops is None or ImageStat is None:
        return {"available": False, "reason": "pillow_missing"}
    if not REFS_DIR.is_dir():
        return {"available": False, "reason": "refs_missing"}

    diffs = {
        name: mean_abs_diff(img, load_ref(name), REGION_FOR.get(name, "full"))
        for name in SCREEN_REFS
    }
    ranked = sorted(diffs.items(), key=lambda kv: kv[1])
    best_name, best_diff = ranked[0]
    runner_up_name, runner_up_diff = ranked[1]
    threshold = THRESHOLD_FOR.get(best_name, DEFAULT_SCREEN_THRESHOLD)
    margin = runner_up_diff - best_diff
    confident = best_diff <= threshold and margin >= MIN_SCREEN_MARGIN
    return {
        "available": True,
        "nearest": best_name,
        "confident": confident,
        "best_diff": round(best_diff, 3),
        "runner_up": runner_up_name,
        "runner_up_diff": round(runner_up_diff, 3),
        "margin": round(margin, 3),
        "threshold": threshold,
    }


def capture_framebuffer(client: DebugClient, out_path: Path) -> dict[str, Any]:
    report: dict[str, Any] = {
        "saved": False,
        "path": None,
        "screen": {"available": False, "reason": "not_captured"},
    }
    if Image is None:
        report["screen"] = {"available": False, "reason": "pillow_missing"}
        return report
    try:
        w, h, rgb_a = client.framebuffer("A")
        wb, hb, rgb_b = client.framebuffer("B")
    except Exception as exc:  # Keep evidence collection best-effort.
        print(f"warning: framebuffer capture failed for {out_path}: {exc}")
        report["screen"] = {"available": False, "reason": "capture_failed"}
        return report
    img = Image.new("RGB", (max(w, wb), h + hb))
    img.paste(Image.frombytes("RGB", (w, h), rgb_a), (0, 0))
    img.paste(Image.frombytes("RGB", (wb, hb), rgb_b), (0, h))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path)
    report["saved"] = True
    report["path"] = str(out_path)
    report["screen"] = classify_screen(img)
    return report


def udp_bucket(event: dict[str, Any]) -> str:
    src_ip = event["src_ip"]
    dst_ip = event["dst_ip"]
    src_port = event.get("src_port", 0)
    dst_port = event.get("dst_port", 0)
    if (
        src_ip == "0.0.0.0" or dst_ip == "0.0.0.0" or
        src_port == 0 or dst_port == 0
    ):
        return "invalid_udp"
    if src_port in WFC_SERVICE_UDP_PORTS or dst_port in WFC_SERVICE_UDP_PORTS:
        return "wfc_service_udp"
    if src_port in NATNEG_UDP_PORTS or dst_port in NATNEG_UDP_PORTS:
        return "natneg_udp"
    if (
        src_port in LAN_NOISE_PORTS or dst_port in LAN_NOISE_PORTS or
        is_multicast_or_broadcast(src_ip) or is_multicast_or_broadcast(dst_ip)
    ):
        return "lan_noise_udp"
    return "candidate_peer_udp"


def summarize_udp(events: list[dict[str, Any]]) -> dict[str, Any]:
    buckets: dict[str, list[dict[str, Any]]] = {
        "wfc_service_udp": [],
        "natneg_udp": [],
        "candidate_peer_udp": [],
        "lan_noise_udp": [],
        "invalid_udp": [],
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
        "natneg_udp": buckets["natneg_udp"][:128],
        "invalid_udp": buckets["invalid_udp"][:128],
        "top_udp_endpoints": top_endpoints,
    }


def candidate_peer_udp_events(instance: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        event for event in instance["kinds"].get("udp_packet", [])
        if udp_bucket(event) == "candidate_peer_udp"
    ]


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
    endpoint_counts: Counter[tuple[str, int, str]] = Counter()
    payload_lens: dict[tuple[str, int, str], int] = {}
    for event in candidate_peer_udp_events(instance):
        src_ip = event.get("src_ip")
        dst_ip = event.get("dst_ip")
        if src_ip == local_ip:
            key = ("out", dst_ip, event.get("dst_port", 0))
            endpoint_counts[key] += 1
            payload_lens[key] = event.get("payload_len")
        elif dst_ip == local_ip:
            key = ("in", src_ip, event.get("src_port", 0))
            endpoint_counts[key] += 1
            payload_lens[key] = event.get("payload_len")
    for (direction, remote_ip, remote_port), count in endpoint_counts.most_common():
        out.append({
            "direction": direction,
            "local_ip": local_ip,
            "remote_ip": remote_ip,
            "remote_port": remote_port,
            "count": count,
            "last_payload_len": payload_lens[(direction, remote_ip, remote_port)],
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
    direction_counts: Counter[tuple[str, str]] = Counter()
    for label, instance in (("A", a), ("B", b)):
        for event in candidate_peer_udp_events(instance):
            src_ip = event.get("src_ip")
            dst_ip = event.get("dst_ip")
            if src_ip in peer_ips and dst_ip in peer_ips and src_ip != dst_ip:
                direction_counts[(src_ip, dst_ip)] += 1
                direct.append({
                    "observed_by": label,
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
        "direct_client_udp_direction_counts": [
            {
                "src_ip": src_ip,
                "dst_ip": dst_ip,
                "count": count,
            }
            for (src_ip, dst_ip), count in sorted(direction_counts.items())
        ],
        "direct_client_udp_bidirectional": (
            bool(ip_a and ip_b) and
            direction_counts[(ip_a, ip_b)] > 0 and
            direction_counts[(ip_b, ip_a)] > 0
        ),
        "shared_candidate_peer_endpoints": shared[:64],
        "shared_candidate_peer_endpoint_count": len(shared),
        "local_candidate_udp": {
            "A": local_a[:128],
            "B": local_b[:128],
        },
    }


def m7_transport_verdict(report: dict[str, Any]) -> dict[str, Any]:
    instances = report.get("instances", [])
    correlation = report.get("correlation", {})
    backend_errors = sum(
        instance.get("kind_counts", {}).get("backend_error", 0)
        for instance in instances
    )
    bucket_totals: Counter[str] = Counter()
    for instance in instances:
        bucket_totals.update(instance["udp_summary"]["counts"])

    direct_client_udp = correlation.get("direct_client_udp_count", 0)
    direct_client_udp_bidirectional = correlation.get(
        "direct_client_udp_bidirectional", False)
    shared_endpoints = correlation.get("shared_candidate_peer_endpoint_count", 0)
    inferred_ips = correlation.get("inferred_client_ips", {})
    missing_ips = [
        label for label in ("A", "B")
        if not inferred_ips.get(label)
    ]

    if backend_errors:
        status = "backend_errors_observed"
    elif missing_ips:
        status = "missing_client_ip"
    elif bucket_totals["natneg_udp"] == 0:
        status = "no_natneg_udp"
    elif bucket_totals["candidate_peer_udp"] == 0:
        status = "natneg_without_peer_udp"
    elif direct_client_udp == 0:
        status = "candidate_peer_udp_without_direct_client_udp"
    elif not direct_client_udp_bidirectional:
        status = "direct_client_udp_one_way"
    else:
        status = "direct_client_udp_bidirectional_observed"

    return {
        "status": status,
        "backend_error_count": backend_errors,
        "missing_client_ips": missing_ips,
        "natneg_udp_count": bucket_totals["natneg_udp"],
        "wfc_service_udp_count": bucket_totals["wfc_service_udp"],
        "candidate_peer_udp_count": bucket_totals["candidate_peer_udp"],
        "direct_client_udp_count": direct_client_udp,
        "direct_client_udp_bidirectional": direct_client_udp_bidirectional,
        "shared_candidate_peer_endpoint_count": shared_endpoints,
        "notes": [
            "This is a transport evidence verdict only.",
            "Race entry and lobby return still require framebuffer or operator confirmation.",
        ],
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
        framebuffer = capture_framebuffer(client, screenshot)
        report["framebuffer_saved"] = framebuffer["saved"]
        report["framebuffer_path"] = framebuffer["path"]
        report["screen"] = framebuffer["screen"]

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
        f"natneg={udp['natneg_udp']} "
        f"candidate_peer={udp['candidate_peer_udp']} "
        f"lan_noise={udp['lan_noise_udp']} "
        f"invalid={udp['invalid_udp']}"
    )
    backend_errors = report["kind_counts"].get("backend_error", 0)
    if backend_errors:
        print(f"  backend_error={backend_errors}")
    screen = report.get("screen", {})
    if screen.get("available"):
        print(
            "  screen: "
            f"nearest={screen.get('nearest')} "
            f"confident={screen.get('confident')} "
            f"diff={screen.get('best_diff')} "
            f"runner_up={screen.get('runner_up')} "
            f"margin={screen.get('margin')}"
        )
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
        "bidirectional="
        f"{correlation.get('direct_client_udp_bidirectional', False)} "
        "shared_candidate_peer_endpoints="
        f"{correlation.get('shared_candidate_peer_endpoint_count', 0)}"
    )
    for endpoint in correlation.get("shared_candidate_peer_endpoints", [])[:8]:
        print(
            "  shared endpoint "
            f"{endpoint['remote_ip']}:{endpoint['remote_port']} "
            f"A={endpoint['a_events']} B={endpoint['b_events']}"
        )


def print_verdict(verdict: dict[str, Any]) -> None:
    if not verdict:
        return
    print(
        "m7 transport verdict: "
        f"{verdict.get('status')} "
        f"natneg={verdict.get('natneg_udp_count', 0)} "
        f"candidate_peer={verdict.get('candidate_peer_udp_count', 0)} "
        f"direct_client={verdict.get('direct_client_udp_count', 0)} "
        f"bidirectional={verdict.get('direct_client_udp_bidirectional', False)} "
        f"backend_error={verdict.get('backend_error_count', 0)}"
    )


def counter_delta(current: dict[str, Any], previous: dict[str, Any]) -> dict[str, int]:
    keys = set(current) | set(previous)
    return {
        key: int(current.get(key, 0)) - int(previous.get(key, 0))
        for key in sorted(keys)
        if int(current.get(key, 0)) != int(previous.get(key, 0))
    }


def snapshot_delta(current: dict[str, Any],
                   previous: dict[str, Any] | None) -> dict[str, Any]:
    if previous is None:
        return {"baseline": True}

    previous_instances = {
        instance["label"]: instance
        for instance in previous.get("instances", [])
    }
    instance_deltas = []
    for instance in current.get("instances", []):
        label = instance["label"]
        prev = previous_instances.get(label, {})
        instance_deltas.append({
            "label": label,
            "event_counts": counter_delta(
                instance.get("event_counts", {}),
                prev.get("event_counts", {}),
            ),
            "kind_counts": counter_delta(
                instance.get("kind_counts", {}),
                prev.get("kind_counts", {}),
            ),
            "udp_counts": counter_delta(
                instance.get("udp_counts", {}),
                prev.get("udp_counts", {}),
            ),
        })

    current_status = current.get("m7_transport_verdict", {}).get("status")
    previous_status = previous.get("m7_transport_verdict", {}).get("status")
    return {
        "baseline": False,
        "status_changed": current_status != previous_status,
        "previous_status": previous_status,
        "current_status": current_status,
        "instances": instance_deltas,
    }


def print_snapshot_delta(delta: dict[str, Any]) -> None:
    if delta.get("baseline"):
        print("delta: baseline snapshot")
        return
    if delta.get("status_changed"):
        print(
            "delta: verdict "
            f"{delta.get('previous_status')} -> {delta.get('current_status')}"
        )
    printed = False
    for instance in delta.get("instances", []):
        udp = instance.get("udp_counts", {})
        kinds = instance.get("kind_counts", {})
        interesting_udp = {
            name: udp.get(name, 0)
            for name in ("wfc_service_udp", "natneg_udp", "candidate_peer_udp")
            if udp.get(name, 0)
        }
        interesting_kinds = {
            name: kinds.get(name, 0)
            for name in ("tcp_reset", "backend_drop", "backend_error")
            if kinds.get(name, 0)
        }
        if interesting_udp or interesting_kinds:
            printed = True
            print(
                f"delta {instance['label']}: "
                f"udp={interesting_udp or {}} "
                f"kinds={interesting_kinds or {}}"
            )
    if not printed and not delta.get("status_changed"):
        print("delta: no new transport-classified events")


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
    report["m7_transport_verdict"] = m7_transport_verdict(report)
    return report


def compact_snapshot(snapshot_index: int, snapshot_path: Path,
                     report: dict[str, Any], elapsed_sec: float,
                     previous_snapshot: dict[str, Any] | None = None
                     ) -> dict[str, Any]:
    compact = {
        "snapshot": snapshot_index,
        "elapsed_sec": round(elapsed_sec, 3),
        "created_at": report["created_at"],
        "path": str(snapshot_path),
        "correlation": report.get("correlation", {}),
        "m7_transport_verdict": report.get("m7_transport_verdict", {}),
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
                "natneg_udp": instance["udp_summary"]["natneg_udp"],
                "invalid_udp": instance["udp_summary"]["invalid_udp"],
                "screen": instance.get("screen", {}),
                "framebuffer_path": instance.get("framebuffer_path"),
            }
            for instance in report["instances"]
        ],
    }
    compact["delta"] = snapshot_delta(compact, previous_snapshot)
    return compact


def write_single_snapshot(args: argparse.Namespace, out_dir: Path,
                          stamp: str) -> int:
    report = collect_report(
        args.port_a, args.port_b, out_dir, args.max_per_kind, stamp)
    report_path = out_dir / "evidence.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    for instance in report["instances"]:
        print_summary(instance)
    print_correlation(report.get("correlation", {}))
    print_verdict(report.get("m7_transport_verdict", {}))
    print(f"wrote {report_path}")
    return 0


def watch(args: argparse.Namespace, out_dir: Path, stamp: str) -> int:
    snapshots_dir = out_dir / "snapshots"
    snapshots_dir.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    index: list[dict[str, Any]] = []
    snapshot_index = 0
    stop_reason: dict[str, Any] | None = None
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
        previous = index[-1] if index else None
        compact = compact_snapshot(
            snapshot_index, snapshot_path, report, elapsed, previous)
        index.append(compact)

        print(f"\n--- snapshot {snapshot_index} elapsed={elapsed:.1f}s ---")
        for instance in report["instances"]:
            print_summary(instance)
        print_correlation(report.get("correlation", {}))
        print_verdict(report.get("m7_transport_verdict", {}))
        print_snapshot_delta(compact["delta"])

        verdict_status = report.get("m7_transport_verdict", {}).get("status")
        if verdict_status in args.stop_on_verdict:
            stop_reason = {
                "kind": "verdict",
                "status": verdict_status,
                "snapshot": snapshot_index,
                "elapsed_sec": round(elapsed, 3),
            }
            print(f"stop condition matched: verdict={verdict_status}")
            break

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
        "latest_m7_transport_verdict": (
            index[-1]["m7_transport_verdict"] if index else {}
        ),
        "stop_reason": stop_reason,
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
    parser.add_argument("--stop-on-verdict", action="append", default=[],
                        help=(
                            "in watch mode, stop after writing the first "
                            "snapshot whose M7 transport verdict has this "
                            "status; may be repeated"
                        ))
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
