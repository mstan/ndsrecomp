#!/usr/bin/env python3
"""Capture guest-decompressed firmware images from the native LLE boot.

The checked-in firmware remains compressed/encrypted.  This tool asks a
running debug server to cold-boot the real BIOSes, stops at deterministic
retired-instruction boundaries, and writes the RAM bytes produced by that
guest execution under generated/ (which is intentionally git-ignored).

These captures are inputs to static firmware-bank generation, not a direct-
boot path.  The runtime continues to perform the complete BIOS copy/decompress
and static dispatch must verify its live bytes against the captured image.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "oracle"))

from _client import DebugClient  # noqa: E402


ARM7_FIRST_TIER3_INSN = 3_269_552
ARM7_INTERMEDIATE_TIER3_INSN = 3_278_811
ARM7_SHARED_READY_TIER3_INSN = 8_399_622
ARM7_IRQ_READY_TIER3_INSN = 27_098_211
ARM9_FIRST_TIER3_INSN = 2_930_520
MAIN_MENU_ARM9_INSN = 42_300_000


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def read_region(client: DebugClient, name: str) -> bytes:
    response = client.cmd("read_region", region=name)
    return bytes.fromhex(response["hex"])


def stop(client: DebugClient, event: str, count: int) -> None:
    response = client.cmd("run_to_event", event=event, count=count)
    if not response.get("reached"):
        raise RuntimeError(f"failed to reach {event}={count}: {response}")


def write_capture(path: Path, data: bytes, metadata: dict[str, object]) -> None:
    path.write_bytes(data)
    metadata.update({
        "path": str(path),
        "size": len(data),
        "sha1": sha1(data),
        "sha256": sha256(data),
    })


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=19852)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--out", type=Path, default=ROOT / "generated" / "firmware_capture")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    client = DebugClient(port=args.port, timeout=args.timeout)
    manifest: dict[str, object] = {
        "format": 1,
        "method": "native LLE BIOS guest-produced RAM capture",
        "captures": [],
    }
    captures: list[dict[str, object]] = manifest["captures"]  # type: ignore[assignment]
    try:
        client.cmd("reset")
        stop(client, "insn7", ARM7_FIRST_TIER3_INSN)
        regs = client.cmd("regs", cpu=7)
        if regs["r"][15] != 0x0380F800:
            raise RuntimeError(f"unexpected first ARM7 entry boundary: {regs}")
        early_wram7 = read_region(client, "wram7")
        entry: dict[str, object] = {
            "name": "arm7_first_entry_wram",
            "event": "insn7",
            "count": ARM7_FIRST_TIER3_INSN,
            "runtime_entry": "0x0380F800",
            "mode": "arm",
        }
        write_capture(args.out / "arm7_first_entry_wram.bin", early_wram7, entry)
        captures.append(entry)

        # The ARM7 firmware overwrites the loader in place once more before
        # reaching its steady-state menu image. Capture the exact first entry
        # into that third generation (zero interpreted instructions at the
        # discovery boundary in a content-validated build).
        client.cmd("reset")
        stop(client, "insn7", ARM7_INTERMEDIATE_TIER3_INSN)
        regs = client.cmd("regs", cpu=7)
        if regs["r"][15] != 0x0380F8D0:
            raise RuntimeError(f"unexpected intermediate ARM7 boundary: {regs}")
        intermediate: dict[str, bytes] = {}
        for name in ("wram7", "wramshared"):
            data = read_region(client, name)
            intermediate[name] = data
            entry = {
                "name": f"arm7_intermediate_{name}",
                "event": "insn7",
                "count": ARM7_INTERMEDIATE_TIER3_INSN,
                "runtime_entry": "0x0380F8D0",
                "mode": "thumb",
            }
            write_capture(args.out / f"arm7_intermediate_{name}.bin", data, entry)
            captures.append(entry)

        client.cmd("reset")
        stop(client, "insn7", ARM7_SHARED_READY_TIER3_INSN)
        regs = client.cmd("regs", cpu=7)
        if regs["r"][15] != 0x037FA600:
            raise RuntimeError(f"unexpected shared-ready ARM7 boundary: {regs}")
        shared_ready: dict[str, bytes] = {}
        for name in ("wram7", "wramshared"):
            data = read_region(client, name)
            shared_ready[name] = data
            entry = {
                "name": f"arm7_shared_ready_{name}",
                "event": "insn7",
                "count": ARM7_SHARED_READY_TIER3_INSN,
                "runtime_entry": "0x037FA600",
                "mode": "arm",
            }
            write_capture(args.out / f"arm7_shared_ready_{name}.bin", data, entry)
            captures.append(entry)

        client.cmd("reset")
        stop(client, "insn7", ARM7_IRQ_READY_TIER3_INSN)
        regs = client.cmd("regs", cpu=7)
        if regs["r"][15] != 0x037FD798:
            raise RuntimeError(f"unexpected IRQ-ready ARM7 boundary: {regs}")
        irq_ready: dict[str, bytes] = {}
        for name in ("wram7", "wramshared"):
            data = read_region(client, name)
            irq_ready[name] = data
            entry = {
                "name": f"arm7_irq_ready_{name}",
                "event": "insn7",
                "count": ARM7_IRQ_READY_TIER3_INSN,
                "runtime_entry": "0x037FD798",
                "mode": "arm",
            }
            write_capture(args.out / f"arm7_irq_ready_{name}.bin", data, entry)
            captures.append(entry)

        client.cmd("reset")
        stop(client, "insn9", ARM9_FIRST_TIER3_INSN)
        regs = client.cmd("regs", cpu=9)
        if regs["r"][15] != 0x021F0000:
            raise RuntimeError(f"unexpected first ARM9 entry boundary: {regs}")
        for name in ("mainram", "itcm"):
            data = read_region(client, name)
            entry = {
                "name": f"arm9_first_entry_{name}",
                "event": "insn9",
                "count": ARM9_FIRST_TIER3_INSN,
                "runtime_entry": "0x021F0000",
                "mode": "arm",
            }
            write_capture(args.out / f"arm9_first_entry_{name}.bin", data, entry)
            captures.append(entry)

        client.cmd("reset")
        stop(client, "insn9", MAIN_MENU_ARM9_INSN)
        counts = client.cmd("event_counts")
        if counts["insn9"] != MAIN_MENU_ARM9_INSN:
            raise RuntimeError(f"unexpected main-menu boundary: {counts}")

        for name in ("mainram", "itcm", "wram7", "wramshared"):
            data = read_region(client, name)
            entry = {
                "name": f"main_menu_{name}",
                "event": "insn9",
                "count": MAIN_MENU_ARM9_INSN,
            }
            write_capture(args.out / f"main_menu_{name}.bin", data, entry)
            captures.append(entry)

        # Composite source images give the finder stable, non-overlapping
        # source addresses. Config code_copy rows map runtime aliases (ITCM and
        # shared-WRAM mirrors) back to these appended source segments. Keep
        # early and steady-state generations separate: the firmware overwrites
        # executable addresses in place, so one address-only image is unsafe.
        early_mainram = (args.out / "arm9_first_entry_mainram.bin").read_bytes()
        early_itcm = (args.out / "arm9_first_entry_itcm.bin").read_bytes()
        mainram = (args.out / "main_menu_mainram.bin").read_bytes()
        itcm = (args.out / "main_menu_itcm.bin").read_bytes()
        for generation, generation_main, generation_itcm in (
            ("early", early_mainram, early_itcm),
            ("menu", mainram, itcm),
        ):
            # Runtime 0x01FF8000..0x02400000 is contiguous here: the final
            # 32 KiB ITCM mirror immediately precedes main RAM.
            arm9_composite = generation_itcm + generation_main
            entry = {
                "name": f"fw_arm9_{generation}",
                "source_layout": {
                    "0x01FF8000": "ARM9 ITCM high mirror (32 KiB)",
                    "0x02000000": "main RAM (4 MiB)",
                },
            }
            write_capture(args.out / f"fw_arm9_{generation}.bin",
                          arm9_composite, entry)
            captures.append(entry)

        menu_wram7 = (args.out / "main_menu_wram7.bin").read_bytes()
        menu_shared = (args.out / "main_menu_wramshared.bin").read_bytes()
        entry = {
            "name": "fw_arm7_early",
            "source_layout": {
                "0x03800000": "ARM7 WRAM at first firmware entry (64 KiB)",
            },
        }
        write_capture(args.out / "fw_arm7_early.bin", early_wram7, entry)
        captures.append(entry)

        arm7_intermediate = intermediate["wramshared"] + intermediate["wram7"]
        entry = {
            "name": "fw_arm7_intermediate",
            "source_layout": {
                "0x037F8000": "shared WRAM at intermediate ARM7 generation (32 KiB)",
                "0x03800000": "ARM7 WRAM at intermediate generation (64 KiB)",
            },
        }
        write_capture(args.out / "fw_arm7_intermediate.bin",
                      arm7_intermediate, entry)
        captures.append(entry)

        arm7_shared_ready = shared_ready["wramshared"] + shared_ready["wram7"]
        entry = {
            "name": "fw_arm7_shared_ready",
            "source_layout": {
                "0x037F8000": "shared WRAM after ARM7 code population (32 KiB)",
                "0x03800000": "ARM7 WRAM at shared-code entry (64 KiB)",
            },
        }
        write_capture(args.out / "fw_arm7_shared_ready.bin",
                      arm7_shared_ready, entry)
        captures.append(entry)

        arm7_irq_ready = irq_ready["wramshared"] + irq_ready["wram7"]
        entry = {
            "name": "fw_arm7_irq_ready",
            "source_layout": {
                "0x037F8000": "shared WRAM at first runtime IRQ entry (32 KiB)",
                "0x03800000": "ARM7 WRAM at first runtime IRQ entry (64 KiB)",
            },
        }
        write_capture(args.out / "fw_arm7_irq_ready.bin", arm7_irq_ready, entry)
        captures.append(entry)

        # With WRAMCNT=3 these two live views are contiguous at runtime.
        arm7_menu = menu_shared + menu_wram7
        entry = {
            "name": "fw_arm7_menu",
            "source_layout": {
                "0x037F8000": "shared WRAM at main menu (32 KiB)",
                "0x03800000": "ARM7 WRAM at main menu (64 KiB)",
            },
        }
        write_capture(args.out / "fw_arm7_menu.bin", arm7_menu, entry)
        captures.append(entry)
    finally:
        client.close()

    manifest_path = args.out / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
