#!/usr/bin/env python3
"""Drive Mario Kart DS to the point where DHCP has completed, for the M8
capture/replay proof (beads Wiimmfi meta epic, capture at the Ethernet
backend boundary) -- WITHOUT going anywhere near real Kaeru.

This intentionally reuses oracle/mkds_wfc_scenario.py's own navigation
(constants, tap()/press_a() helpers, verify_screen/wait_for_connection_test_
result screen-content checks) rather than re-deriving new touch coordinates
or timing -- see this project's own directive to "reuse the existing driver
... rather than writing new navigation." mkds_wfc_scenario.py is owned by a
sibling task (making its own navigation deterministic) and is NOT edited
here; this file only imports names from it.

Why not just call run_scenario() from that module directly: run_scenario()
continues past the connection test into the real NAS login flow, which
contacts the live Kaeru service over the Internet -- exactly the kind of
"hammering a free volunteer-run service" this milestone's whole premise
(deterministic capture/replay so CI never needs to touch the real network)
argues against. DHCP happens well before that point: association + the
DHCP lease are prerequisites for the connection test itself (a plain HTTP
reachability check), which is fully local (this project's own virtual AP +
libslirp's built-in DHCP server) and never touches the Internet. So this
script runs the IDENTICAL steps run_scenario() runs, verbatim, up to and
including the "connection_test_settled" checkpoint, and stops there --
never reaching the WFC-match/login steps that follow it in that file.

Usage (server must already be running with --serve, --network-backend
slirp or replay, and whatever --net-capture-* flags the caller wants):

    python tools/m8_dhcp_capture_drive.py --port 19842
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# oracle/ is a sibling directory of this file's parent (tools/); reuse its
# navigation module by import, not by copying its logic.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "oracle"))

from mkds_wfc_scenario import (  # noqa: E402  (import after sys.path fixup)
    DebugClient,
    CART_ICON,
    GAME_BOOT_IPCSYNC_W,
    TITLE_NINTENDO_WFC,
    DIALOG_YES,
    DIALOG_NO,
    WFC_SETTINGS_MENU_ITEM,
    WFC_CONNECTION_SETTINGS_TILE,
    CONNECTION_SLOT_1,
    SEARCH_FOR_AP,
    AP_LIST_FIRST_ROW,
    tap,
    press_a,
    shoot,
    verify_screen,
    wait_for_connection_test_result,
)

GAME_ROOT = Path(__file__).resolve().parents[2] / "mariokartdsrecomp-wiimmfi"
DEFAULT_SHOTS_DIR = GAME_ROOT / "generated" / "captures" / "m8-dhcp"


def drive_to_dhcp(port: int, shots_dir: Path, stall: int = 300_000) -> dict:
    """Verbatim replay of mkds_wfc_scenario.run_scenario()'s own steps, up
    to and including "connection_test_settled" -- then stops (does NOT
    proceed into the WFC-match/NAS-login steps that follow in that file).
    Returns the final event_counts dict."""
    client = DebugClient(port=port, timeout=900.0)
    label = "m8dhcp"
    checkpoints: list[tuple[str, dict]] = []

    def checkpoint(name: str) -> dict:
        counts = client.cmd("event_counts")
        checkpoints.append((name, counts))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_{name}.png")
        print(f"  checkpoint {name}: vblank9={counts.get('vblank9')} "
              f"vblank7={counts.get('vblank7')}")
        return counts

    client.cmd("reset")
    client.cmd("run_to_event", event="vblank9", count=120, stall=stall)
    client.cmd("touch", x=CART_ICON[0], y=CART_ICON[1], down=True)
    client.cmd("run_to_event", event="vblank9", count=130, stall=stall)
    client.cmd("touch", x=CART_ICON[0], y=CART_ICON[1], down=False)

    hit = client.cmd("run_to_event", event="ipcsync_w",
                      count=GAME_BOOT_IPCSYNC_W, stall=stall)
    if not hit.get("reached") or hit.get("terminal"):
        raise RuntimeError(f"cart launch failed to reach the game-boot IPC "
                            f"handshake: {hit}")
    checkpoint("cart_boot_handshake")

    client.cmd("run_to_event", event="vblank9", count=900, stall=stall)
    checkpoint("title_screen")
    verify_screen(client, shots_dir, f"{label}_title_screen", "title_screen")

    tap(client, TITLE_NINTENDO_WFC, 910, 1000, stall=stall)
    checkpoint("nickname_confirm_dialog")
    verify_screen(client, shots_dir, f"{label}_nickname_confirm_dialog",
                  "nickname_confirm_dialog", diag_candidates=("title_screen",))

    tap(client, DIALOG_YES, 1010, 1100, stall=stall)
    checkpoint("onscreen_name_confirm_dialog")

    tap(client, DIALOG_YES, 1110, 1250, stall=stall)
    checkpoint("custom_emblem_dialog")

    tap(client, DIALOG_NO, 1260, 1400, stall=stall)
    checkpoint("title_screen_after_setup")
    verify_screen(client, shots_dir, f"{label}_title_screen_after_setup",
                  "title_screen")

    tap(client, TITLE_NINTENDO_WFC, 1410, 1500, stall=stall)
    checkpoint("wfc_transition")

    client.cmd("run_to_event", event="vblank9", count=1700, stall=stall)
    checkpoint("wfc_connection_menu")
    verify_screen(client, shots_dir, f"{label}_wfc_connection_menu",
                  "wfc_connection_menu", diag_candidates=("title_screen",))

    tap(client, WFC_SETTINGS_MENU_ITEM, 1710, 1850, stall=stall)
    checkpoint("wfc_connection_setup_step1")
    verify_screen(client, shots_dir, f"{label}_wfc_connection_setup_step1",
                  "wfc_connection_setup_step1",
                  diag_candidates=("wfc_connection_menu",))

    tap(client, WFC_CONNECTION_SETTINGS_TILE, 1860, 2000, stall=stall)
    checkpoint("connection_slot_picker")

    tap(client, CONNECTION_SLOT_1, 2010, 2150, stall=stall)
    checkpoint("connection1_settings_step2")

    tap(client, SEARCH_FOR_AP, 2160, 2300, stall=stall)
    checkpoint("searching_for_ap")

    client.cmd("run_to_event", event="vblank9", count=3500, stall=stall)
    checkpoint("ap_list_found")

    tap(client, AP_LIST_FIRST_ROW, 3510, 3650, stall=stall)
    checkpoint("ap_selected_connection_test_prompt")

    press_a(client, 3660, 3800, stall=stall)
    checkpoint("connection_test_running")

    # This is the step that needs DHCP to have already succeeded (the
    # connection test is a reachability check against the already-leased
    # IP). Polled, not a fixed offset -- see wait_for_connection_test_
    # result's own docstring in mkds_wfc_scenario.py.
    hit, target = wait_for_connection_test_result(
        client, shots_dir, f"{label}_connection_test_settled",
        start=3800, stride=100, max_extra=4000, stall=stall,
    )
    counts = checkpoint("connection_test_settled")

    print(f"[m8_dhcp_capture_drive] STOPPING here (deliberately, before the "
          f"WFC-match/NAS-login steps that would contact the real Kaeru "
          f"service) -- final vblank9={counts.get('vblank9')}")
    client.close()
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=19842)
    parser.add_argument("--shots-dir", type=Path, default=DEFAULT_SHOTS_DIR)
    parser.add_argument("--stall", type=int, default=300_000)
    args = parser.parse_args()
    try:
        drive_to_dhcp(args.port, args.shots_dir, args.stall)
    except Exception as exc:  # noqa: BLE001 -- top-level driver, report and exit nonzero
        print(f"[m8_dhcp_capture_drive] FAILED: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
