#!/usr/bin/env python3
"""Drive Mario Kart DS, from a cold LLE boot, all the way into REAL GameSpy-
era Wiimmfi netcode -- login, presence, the server browser, and an actual
player search -- and document what the network genuinely did (Wiimmfi meta
epic, M6: "reach the GameSpy-era online services on real Wiimmfi").

This is a NEW, independent driver (beads-yjp M6/I14). It does not import or
modify `oracle/mkds_wfc_scenario.py` -- that file is owned by a sibling task
(I12) actively making its own navigation deterministic. The early boot ->
title -> WFC-setup -> connection-test -> NAS-login tail below intentionally
reuses that script's PUBLISHED, already-derived touch coordinates (see the
per-constant comments) because they are empirical facts about the game's UI,
not code; this file re-derives everything past `wfc_match_setup_screen` --
the match-conditions ("Choose the conditions for this match") screen and
beyond -- which no existing driver reaches.

Run against the current DS-compatible Wiimmfi route:

  nds_runner <bios-dir> --serve --port 19842 --config game.toml \
      --rom "Mario Kart DS.nds" --no-save --startup-mode manual \
      --network on --network-backend pcap --wfc on --wfc-provider wiimmfi
  python oracle/mkds_online_menus.py [--shots-dir DIR] [--port 19842]

For a non-public-matchmaking proof of authenticated menu reachability, add
`--stop-at-match-setup`; this stops after the authenticated "Choose the
conditions for this match" screen and still writes the network evidence.

------------------------------------------------------------------------
Screen-verification methodology, and a false-positive this task found in
the process of building it (fully described here because the SAME latent
bug shape -- a bare "diff(expect) <= threshold" accept -- would silently
mis-time-shift a future run):

`oracle/wfc_screen_refs/*.png` already carries reference crops with a
documented threshold (up to 20.0 mean-abs-grayscale-diff) for the two
real-network-timing-dependent screens (`connection_test_settled`,
`wfc_login_settled`, `wfc_login_next`, `wfc_match_setup_screen`), sized
from "measured intra-class distance up to ~15" for those screens per that
module's own derivation notes. In practice, during THIS task's own live
derivation (real Kaeru, 2026-08-10), the top-screen crop of the genuinely-
still-"Connecting to Nintendo WFC..." screen measured only ~6.0 mean-abs-
diff from the `wfc_login_settled` reference -- comfortably UNDER that
20.0 threshold -- so a bare threshold-only accept declares victory on the
very first poll, before the real NAS/GPCM login has even settled (this
reproduces the exact `wiimmfi-m5-final` mislabeling incident recorded in
that capture directory's own `NOTE_frames_22-24.txt`, just automated
instead of schedule-blind).

The fix used throughout this file (`wait_for_screen` below): classify each
polled frame by NEAREST NEIGHBOR among `{expect} + diag_candidates` (the
argmin of mean-abs-diff across every plausible screen at this point in the
flow, not `expect` alone), and only accept when that argmin is `expect`
AND beats the runner-up by a `margin` (default 3.0). On the frame that
falsely passed the bare-threshold test above, `wfc_connecting` was the
true argmin at diff=0.08 -- correctly rejected -- and the poll continued
for real until, ~1100-1300 VBlanks later, `wfc_connecting`'s own diff
jumped to double digits and `wfc_login_settled` hit an exact 0.0: genuine
settlement, confirmed on two independent live Kaeru runs (VBlank9 deltas
of 1150 and 1300 from the "Connecting..." tap, i.e. roughly 19-22s of
guest time -- longer than the ~10s this project's docs cite as typical,
consistent with real per-session network jitter, not a stall).

------------------------------------------------------------------------
Environment hazard this task hit repeatedly and worked around (documented
for whoever runs this next): sibling agents on this same machine run their
OWN `nds_runner` on the conventional default port (19842) and, per this
project's OWN standing environment rule ("kill stale servers before every
probe": `Get-Process nds_runner,ndsref | Stop-Process -Force`), that kill
matches by PROCESS NAME ONLY -- it has no notion of "someone else's
server on a different port" and will silently execute this driver's own
`nds_runner` out from under it mid-session. This is not a runtime bug (no
crash dump, no unhandled-exception record, no stderr past the last
buffered flush -- confirmed by checking the Windows Application-Error
event log during this task's derivation, which had zero `nds_runner.exe`
entries despite three silent terminations in one session) -- it is a
naming collision between two agents each correctly following the same
shared instruction. Mitigation used here, and recommended for any future
long-lived real-network run sharing this machine: launch on a port other
than 19842/19843 (`--port` below defaults to 19842 for drop-in
compatibility with the existing convention, but pass a private port when
sharing the machine), and if persistent collisions recur, run a renamed
COPY of the already-built `nds_runner.exe` (a plain file copy, not a
rebuild) so process-name-based kill commands cannot match it. This file
does not do that renaming for you -- it is an operational step for
whoever launches the server, not something a debug-protocol client can
do to itself.

------------------------------------------------------------------------
Courtesy (Wiimmfi is a free, volunteer-run service, and matchmaking pairs
real strangers): this driver walks into an actual "Searching for
opponents" state and lets it run for a bounded window, but never taps
forward once a real opponent is found -- if the search screen ever shows
found/joined players instead of the "Searching..." placeholder rows, stop
immediately (see `PlayersFoundError` below) rather than proceeding into a
race. Cancelling an in-progress, no-opponents-found search is always safe
(never abandons a live match) and is exactly what `cancel_search` does.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any
from collections import Counter

from _client import DebugClient

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:  # pragma: no cover - Pillow is a repo dependency
    Image = None
    ImageChops = None
    ImageStat = None


GAME_ROOT = Path(__file__).resolve().parents[2] / "mariokartdsrecomp"
DEFAULT_SHOTS_DIR = GAME_ROOT / "generated" / "captures" / "wiimmfi-online-menus"
REFS_DIR = Path(__file__).with_name("wfc_screen_refs")

STALL_DEFAULT = 300_000

# ---------------------------------------------------------------------------
# Touch targets. Everything through WFC_MATCH_CONFIRM_YES reuses the
# empirically-derived coordinates already published and cross-checked in
# `oracle/mkds_wfc_scenario.py` (read, not imported, per this task's file-
# ownership rule) -- these are facts about the game's fixed UI layout, not
# behavior owned by that script. MATCH_SETUP_OK and CANCEL_SEARCH_BACK_ARROW
# are new, derived by this task past the point that script reaches.
CART_ICON = (128, 38)
TITLE_NINTENDO_WFC = (128, 122)
DIALOG_YES = (68, 148)
DIALOG_NO = (188, 148)
WFC_SETTINGS_MENU_ITEM = (150, 133)
WFC_CONNECTION_SETTINGS_TILE = (85, 100)
CONNECTION_SLOT_1 = (43, 35)
SEARCH_FOR_AP = (128, 37)
AP_LIST_FIRST_ROW = (50, 65)
WFC_MATCH = (150, 50)
ONE_BUTTON_DIALOG = (128, 150)
WFC_MATCH_CONFIRM_YES = (68, 150)

# "Choose the conditions for this match" screen (beads-yjp M6/I14, new).
# Derived from a live framebuffer read + a labeled coordinate-grid overlay
# (same "threshold/crop the rendered bottom screen" method as every constant
# above), NOT eyeballed from a thumbnail. The first attempt at this button
# (172, 161) missed entirely -- a byte-for-byte diff between the before/
# after screenshots of that tap was exactly 0.0 across all three channels,
# proving the tap landed outside the touch-sensitive rect -- because the OK
# button here is a SMALL, left-aligned button (interior x=100-160, y=150-178
# in the bottom screen's 0..192 space), unlike every earlier OK/NEXT button
# in this flow which was full-width. Re-derived via a 6x-zoomed, finely
# gridded crop of exactly this button: interior center (128, 163).
MATCH_SETUP_OK = (128, 163)

# "Searching for opponents" screen's back arrow (beads-yjp M6/I14, new).
# Same red back-arrow glyph and bottom-left placement as
# `wfc_match_setup_screen`'s (x=5-40, y=150-185); reused directly since both
# screens share this project's standard back-navigation affordance. Tapping
# this from an active, no-opponents-found search cleanly cancels it (see
# `cancel_search`'s docstring for what this actually produces on real
# Kaeru: a benign, guest-initiated WFC disconnect, not a network fault).
CANCEL_SEARCH_BACK_ARROW = (20, 168)

A_PRESSED = 0x3FF & ~0x1
A_RELEASED = 0x3FF
B_PRESSED = 0x3FF & ~0x2
B_RELEASED = 0x3FF

GAME_BOOT_IPCSYNC_W = 12520

# ---------------------------------------------------------------------------
# Screen-content verification. See the module docstring for why this is
# nearest-neighbor classification with a required margin, not a bare
# threshold test on the expected screen alone.
REGION_FOR = {
    "title_screen": "full",
    "wfc_connection_menu": "full",
    "wfc_connection_setup_step1": "full",
    "connection_test_running": "test_band",
    "connection_test_settled": "test_band",
    "connection_test_error": "test_band",
    "wfc_match_disclaimer": "full",
    "wfc_wifi_id_update_warning": "full",
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
DEFAULT_THRESHOLD = 10.0
CONNECTION_TEST_OUTCOME_MAX_DIFF = 25.0
ERROR_CODE_LABEL_REGION = (4, 224, 94, 240)
ERROR_CODE_LABEL_THRESHOLD = 5.0
MIN_CLASSIFICATION_MARGIN = 3.0
SEARCH_PLACEHOLDER_DIFF_THRESHOLD = 3.0
WFC_SERVICE_UDP_PORTS = {27900}
NATNEG_UDP_PORTS = {27901}
LAN_NOISE_PORTS = {
    53, 67, 68, 137, 138, 1900, 3702, 5353, 5355, 6537, 9478, 9999,
    10101, 32412, 32414,
}
CONNECTION_TEST_OUT_OF_SET = (
    "wfc_connection_menu",
    "wfc_connection_setup_step1",
)

_ref_cache: dict[str, Any] = {}


def load_ref(name: str):
    img = _ref_cache.get(name)
    if img is None:
        img = Image.open(REFS_DIR / f"{name}.png").convert("RGB")
        _ref_cache[name] = img
    return img


def capture_rgb(client: DebugClient):
    w, h, rgb_a = client.framebuffer("A")
    wb, hb, rgb_b = client.framebuffer("B")
    img = Image.new("RGB", (max(w, wb), h + hb))
    img.paste(Image.frombytes("RGB", (w, h), rgb_a), (0, 0))
    img.paste(Image.frombytes("RGB", (wb, hb), rgb_b), (0, h))
    return img


def to_gray_region(img, region: str):
    if region == "top":
        cropped = img.crop((0, 0, 256, 192))
    elif region == "test_band":
        cropped = img.crop((0, 230, 256, 344))
    else:
        cropped = img
    return cropped.convert("L")


def mean_abs_diff(img_a, img_b, region: str) -> float:
    a = to_gray_region(img_a, region)
    b = to_gray_region(img_b, region)
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]


def mean_abs_diff_box(img_a, img_b, box: tuple[int, int, int, int]) -> float:
    a = img_a.crop(box).convert("L")
    b = img_b.crop(box).convert("L")
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]


def connection_test_error_label_diff(img) -> float:
    return mean_abs_diff_box(
        img, load_ref("connection_test_error"), ERROR_CODE_LABEL_REGION)


def diff_report(img, names) -> dict[str, float]:
    return {
        name: mean_abs_diff(img, load_ref(name), REGION_FOR.get(name, "full"))
        for name in names
    }


class ScreenTimeoutError(RuntimeError):
    pass


class ConnectionTestFailed(RuntimeError):
    """Raised when the WFC Settings connection test reaches an Error Code screen."""


class PlayersFoundError(RuntimeError):
    """Raised by `check_no_players_found` if the search screen ever shows a
    found/joined opponent instead of the "Searching..." placeholder -- the
    courtesy stop condition. Never suppress or retry past this."""


def wait_for_screen(client: DebugClient, shots_dir: Path, label: str,
                     expect: str, start: int, stride: int, max_extra: int,
                     stall: int, diag_candidates: tuple[str, ...] = (),
                     margin: float = 3.0, verbose: bool = True
                     ) -> tuple[Any, int, Any]:
    """Poll vblank9 forward, classifying each frame by NEAREST NEIGHBOR
    among {expect} + diag_candidates rather than a bare threshold test on
    `expect` alone (see module docstring for the false-positive this
    distinction avoids). Returns (last run_to_event result, matched vblank9
    target, matched frame). Raises ScreenTimeoutError -- naming the
    expected screen and the closest actually-seen candidate, plus a saved
    diagnostic screenshot -- if the budget is exhausted or execution
    stalls/halts.
    """
    target = start
    img = None
    names = (expect,) + tuple(n for n in diag_candidates if n != expect)
    while True:
        hit = client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
        if hit.get("terminal") or hit.get("stalled"):
            raise ScreenTimeoutError(
                f"{label}: execution stalled/halted waiting for {expect!r} "
                f"at vblank9 target={target}: {hit}")
        img = capture_rgb(client)
        diffs = diff_report(img, names)
        ranked = sorted(diffs.items(), key=lambda kv: kv[1])
        best_name, best_diff = ranked[0]
        runner_up_diff = ranked[1][1] if len(ranked) > 1 else float("inf")
        if verbose:
            print(f"    [{label} @vblank9={target}] {diffs}")
        if (best_name == expect
                and best_diff <= THRESHOLD_FOR.get(expect, DEFAULT_THRESHOLD)
                and (runner_up_diff - best_diff) >= margin):
            return hit, target, img
        if target - start >= max_extra:
            break
        target += stride
    diffs = diff_report(img, names)
    best = min(diffs, key=diffs.get)
    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / f"{label}_TIMEOUT_wanted_{expect}.png"
    img.save(fail_path)
    raise ScreenTimeoutError(
        f"{label}: timed out after vblank9 {start}..{target} (budget "
        f"{max_extra}) waiting for {expect!r}; closest actually-seen="
        f"{best!r} (diff={diffs[best]:.1f}); all diffs={diffs}; "
        f"screenshot: {fail_path}")


def wait_for_connection_test_result(client: DebugClient, shots_dir: Path,
                                     label: str, start: int, stride: int,
                                     max_extra: int, stall: int
                                     ) -> tuple[Any, int, Any]:
    """Poll the WFC Settings connection test until it succeeds or fails.

    The status text for this screen is on the bottom screen, so the top-screen
    crop cannot distinguish Testing connection from Connection successful. Use
    the same debounced nearest-outcome classifier as mkds_wfc_scenario.py.
    """
    target = start
    img = None
    diffs = {}
    outcomes = ("connection_test_running", "connection_test_settled",
                "connection_test_error")
    previous = None
    while True:
        hit = client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
        if hit.get("terminal") or hit.get("stalled"):
            raise ScreenTimeoutError(
                f"{label}: execution stalled/halted waiting for the "
                f"connection test to conclude at vblank9 target={target}: {hit}")
        img = capture_rgb(client)
        diffs = diff_report(img, outcomes)
        error_label_diff = connection_test_error_label_diff(img)
        if error_label_diff <= ERROR_CODE_LABEL_THRESHOLD:
            nearest = "connection_test_error"
            confident = True
        else:
            ordered = sorted(diffs, key=diffs.get)
            nearest = ordered[0]
            margin = diffs[ordered[1]] - diffs[nearest]
            confident = (
                margin >= MIN_CLASSIFICATION_MARGIN and
                diffs[nearest] <= CONNECTION_TEST_OUTCOME_MAX_DIFF
            )
            if not confident:
                out_of_set_diffs = diff_report(img, CONNECTION_TEST_OUT_OF_SET)
                out_of_set = min(out_of_set_diffs, key=out_of_set_diffs.get)
                if out_of_set_diffs[out_of_set] <= DEFAULT_THRESHOLD:
                    shots_dir.mkdir(parents=True, exist_ok=True)
                    fail_path = (
                        shots_dir /
                        f"{label}_MISMATCH_observed_{out_of_set}.png"
                    )
                    img.save(fail_path)
                    raise ScreenTimeoutError(
                        f"{label}: expected connection-test result screen, "
                        f"but observed {out_of_set!r} at vblank9={target}; "
                        f"connection-test diffs={diffs}; "
                        f"out_of_set_diffs={out_of_set_diffs}; "
                        f"screenshot: {fail_path}")
        confirmed = (nearest == previous) if confident else False
        previous = nearest if confident else None
        if nearest == "connection_test_settled" and confident and confirmed:
            return hit, target, img
        if nearest == "connection_test_error" and confident and confirmed:
            shots_dir.mkdir(parents=True, exist_ok=True)
            fail_path = shots_dir / f"{label}_CONNECTION_TEST_ERROR.png"
            img.save(fail_path)
            raise ConnectionTestFailed(
                f"{label}: the WFC connection test reported an on-screen "
                f"Error Code at vblank9={target} (diffs={diffs}); "
                f"error_label_diff={error_label_diff:.1f}; "
                f"screenshot: {fail_path}")
        if target - start >= max_extra:
            break
        target += stride
    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / f"{label}_TIMEOUT_still_running.png"
    img.save(fail_path)
    raise ScreenTimeoutError(
        f"{label}: connection test never concluded after vblank9 "
        f"{start}..{target} (budget {max_extra}); diffs={diffs}; "
        f"diagnostic screenshot: {fail_path}")


def start_connection_test(client: DebugClient, shots_dir: Path,
                          down: int, up: int, stall: int,
                          max_attempts: int = 3
                          ) -> tuple[Any, int, Any]:
    """Press A from WFC setup step 1 and verify the test actually starts.

    MKDS occasionally ignores the first A press after returning from AP
    selection while leaving the screen byte-identical to setup step 1. Retry
    that one input only; once an actual test/error screen appears, hand off to
    the normal result poller so backend failures remain failures.
    """
    attempt_down = down
    attempt_up = up
    names = (
        "connection_test_running",
        "connection_test_settled",
        "connection_test_error",
        "wfc_connection_setup_step1",
    )
    last_img = None
    last_diffs: dict[str, float] = {}
    for attempt in range(1, max_attempts + 1):
        hit = press_a(client, attempt_down, attempt_up, stall)
        last_img = capture_rgb(client)
        last_diffs = diff_report(last_img, names)
        error_label_diff = connection_test_error_label_diff(last_img)
        if error_label_diff <= ERROR_CODE_LABEL_THRESHOLD:
            return hit, attempt_up, last_img

        ranked = sorted(last_diffs.items(), key=lambda kv: kv[1])
        best_name, best_diff = ranked[0]
        runner_up_diff = ranked[1][1]
        confident = (
            best_diff <= THRESHOLD_FOR.get(best_name, DEFAULT_THRESHOLD)
            and (runner_up_diff - best_diff) >= MIN_CLASSIFICATION_MARGIN
        )
        if confident and best_name != "wfc_connection_setup_step1":
            return hit, attempt_up, last_img

        print(
            f"  connection test did not start after A attempt {attempt}; "
            f"nearest={best_name} diff={best_diff:.1f}; retrying")
        attempt_down = attempt_up + 60
        attempt_up = attempt_down + 140

    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / "connection_test_START_STUCK_setup_step1.png"
    if last_img is not None:
        last_img.save(fail_path)
    raise ScreenTimeoutError(
        "connection_test: A did not start the WFC connection test after "
        f"{max_attempts} attempts; diffs={last_diffs}; screenshot: {fail_path}")


def converge_to_title(client: DebugClient, shots_dir: Path, label: str,
                       start: int, stall: int, max_presses: int = 6
                       ) -> tuple[Any, int]:
    """Press B until the title screen is actually observed.

    The connection-test result dismissal path is timing-sensitive enough that
    a fixed number of B presses can land on the setup menu instead of the title
    screen. The title screen is an absorbing state for B, so checking the frame
    after each press is both stricter and harmless.
    """
    target = start
    img = None
    hit = None
    for _ in range(max_presses):
        client.cmd("keys", mask=B_PRESSED)
        target += 40
        client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
        client.cmd("keys", mask=B_RELEASED)
        target += 80
        hit = client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
        img = capture_rgb(client)
        d = mean_abs_diff(img, load_ref("title_screen"),
                          REGION_FOR["title_screen"])
        if d <= THRESHOLD_FOR.get("title_screen", DEFAULT_THRESHOLD):
            return hit, target

    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / f"{label}_TIMEOUT_never_converged_to_title.png"
    img.save(fail_path)
    diffs = diff_report(img, ("title_screen", "wfc_connection_menu",
                              "wfc_connection_setup_step1"))
    raise ScreenTimeoutError(
        f"{label}: never converged to the title screen after {max_presses} "
        f"B-presses (vblank9 target={target}); diffs={diffs}; "
        f"diagnostic screenshot: {fail_path}")


def tap(client: DebugClient, xy: tuple[int, int], vblank_down: int,
        vblank_up: int, stall: int = STALL_DEFAULT) -> dict[str, Any]:
    x, y = xy
    client.cmd("touch", x=x, y=y, down=True)
    client.cmd("run_to_event", event="vblank9", count=vblank_down, stall=stall)
    client.cmd("touch", x=x, y=y, down=False)
    return client.cmd("run_to_event", event="vblank9", count=vblank_up, stall=stall)


def press(client: DebugClient, mask_pressed: int, vblank_down: int,
          vblank_up: int, stall: int = STALL_DEFAULT) -> dict[str, Any]:
    client.cmd("keys", mask=mask_pressed)
    client.cmd("run_to_event", event="vblank9", count=vblank_down, stall=stall)
    client.cmd("keys", mask=0x3FF)
    return client.cmd("run_to_event", event="vblank9", count=vblank_up, stall=stall)


def press_a(client, down, up, stall=STALL_DEFAULT):
    return press(client, A_PRESSED, down, up, stall)


def press_b(client, down, up, stall=STALL_DEFAULT):
    return press(client, B_PRESSED, down, up, stall)


def shoot(client: DebugClient, path: Path) -> Any:
    img = capture_rgb(client)
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)
    return img


# ---------------------------------------------------------------------------
# Regression gate: the firmware-menu boot checkpoint this project already
# treats as its hardware-fidelity baseline (vblank9=120, cross-checked
# against the melonDS oracle -- see docs/wiimmfi-runbook.md section 6). This
# also happens to be the exact window the always-on Wi-Fi register ring
# (net_ring) captures during firmware-menu boot's own WLAN bring-up probing,
# before any cartridge or network activity -- useful as a cheap smoke test
# that the runtime under test hasn't regressed before spending a real Kaeru
# login attempt on it.
REGRESSION_EXPECTED = {
    "vblank9": 120, "vblank7": 120, "ipcsync_w": 211, "spi_w": 152359,
}
# NOTE (beads-yjp M6/I14, 2026-08-10): every OTHER field here matched this
# baseline exactly across two independent live runs, but vblank7 measured
# 121 both times, not 120 -- deterministic, not run-to-run noise. Left at
# the originally-stated baseline (120) rather than quietly rewritten to
# match what was observed, so this script keeps reporting that one-count
# mismatch instead of hiding it; see this task's final report for the full
# discussion (every other counter, plus the full 438-event/register-by-
# register Wi-Fi-write breakdown, matched exactly).
REGRESSION_NET_TOTAL_EXPECTED = 438
REGRESSION_TOP_REGS_EXPECTED = {0x815E: 214, 0x8158: 107, 0x815A: 106}
REGRESSION_FIRST_REG_EXPECTED = 0x8036


def run_regression_gate(client: DebugClient, stall: int = STALL_DEFAULT
                         ) -> dict[str, Any]:
    """Boot to the firmware menu (vblank9=120) and report the hardware-event
    counters plus the Wi-Fi-register-write/read ring breakdown against this
    project's known-good baseline. Reports ACTUAL numbers regardless of
    match/mismatch -- this is a smoke test, not a gate that blocks the rest
    of the run (a firmware-menu-boot regression is real signal either way,
    but this script's job is documenting the online flow, not adjudicating
    that regression)."""
    from collections import Counter

    client.cmd("reset")
    client.cmd("run_to_event", event="vblank9", count=120, stall=stall)
    counts = client.cmd("event_counts")
    events = client.cmd("net_ring_dump", max=4096, filter="all")["events"]
    kind_counts = Counter(e["kind"] for e in events)
    reg_counts = Counter(e["wifi_reg"] for e in events
                          if e["kind"] in ("wifi_reg_read", "wifi_reg_write"))
    top_regs = sorted(reg_counts.items(), key=lambda kv: -kv[1])
    report = {
        "counts": {k: counts.get(k) for k in REGRESSION_EXPECTED},
        "counts_expected": REGRESSION_EXPECTED,
        "net_total": len(events),
        "net_total_expected": REGRESSION_NET_TOTAL_EXPECTED,
        "kind_counts": dict(kind_counts),
        "top_wifi_regs": [(hex(r), n) for r, n in top_regs[:8]],
        "top_wifi_regs_expected": {hex(k): v for k, v in REGRESSION_TOP_REGS_EXPECTED.items()},
        "first_event": events[0] if events else None,
        "first_event_reg_expected": hex(REGRESSION_FIRST_REG_EXPECTED),
    }
    return report


def print_regression_gate(report: dict[str, Any]) -> None:
    print("\n--- regression gate (firmware-menu boot, vblank9=120) ---")
    for k, expected in report["counts_expected"].items():
        actual = report["counts"][k]
        flag = "OK" if actual == expected else "MISMATCH"
        print(f"  {k:<12} actual={actual!s:<10} expected={expected!s:<10} {flag}")
    total_flag = "OK" if report["net_total"] == report["net_total_expected"] else "MISMATCH"
    print(f"  net_total    actual={report['net_total']:<10} "
          f"expected={report['net_total_expected']:<10} {total_flag}")
    print(f"  kind_counts: {report['kind_counts']}")
    print(f"  top wifi regs (actual):   {report['top_wifi_regs']}")
    print(f"  top wifi regs (expected): {report['top_wifi_regs_expected']}")
    if report["first_event"]:
        first_reg = report["first_event"]["wifi_reg"]
        flag = "OK" if hex(first_reg) == report["first_event_reg_expected"] else "MISMATCH"
        print(f"  first event wifi_reg: actual={hex(first_reg)} "
              f"expected={report['first_event_reg_expected']} {flag}")


# ---------------------------------------------------------------------------
# Full net-ring evidence, EVERY kind the ring can classify (net_ring.h's
# complete NdsNetEventKind enum -- 22 entries). `mkds_wfc_scenario.py`'s own
# NET_KINDS tuple only covers the 12 host-network-layer kinds (arp, dhcp,
# dns_*, tcp_*, udp_packet, backend_*, tls_record) and omits the 10 Wi-Fi-
# device-layer kinds (wifi_reg_read/write, wifi_irq, wifi_tx_begin/frame,
# wifi_rx_frame, wifi_association, wifi_state_change, ethernet_tx/rx) --
# exactly the kinds the regression gate above needs for its Wi-Fi-register
# breakdown. This file's own dump always covers the full enum.
ALL_NET_KINDS = (
    "wifi_reg_read", "wifi_reg_write", "wifi_irq", "wifi_tx_begin",
    "wifi_tx_frame", "wifi_rx_frame", "wifi_association", "wifi_state_change",
    "ethernet_tx", "ethernet_rx",
    "arp", "dhcp", "dns_query", "dns_response",
    "tcp_open", "tcp_close", "tcp_reset", "tcp_packet", "udp_packet",
    "backend_drop", "backend_error", "tls_record",
)

# Privacy (see net_ring.h and this task's own instructions): src_mac/dst_mac
# are console-identifying data. This dump helper deliberately drops them --
# every other field (addresses, ports, lengths, protocol labels, hostnames)
# is metadata the ring already restricts itself to; MAC is the one field on
# NdsNetTraceEntry this file will never surface, log, or save.
_DROP_FIELDS = ("src_mac", "dst_mac")


def dump_net_evidence(client: DebugClient, kinds=ALL_NET_KINDS, max_per_kind=4096
                       ) -> dict[str, Any]:
    out: dict[str, list] = {}
    for kind in kinds:
        events = client.cmd("net_ring_dump", max=max_per_kind, filter=kind)["events"]
        for e in events:
            for f in _DROP_FIELDS:
                e.pop(f, None)
        out[kind] = events
    return {
        "net_state": client.cmd("net_state"),
        "event_counts": client.cmd("event_counts"),
        "kinds": out,
    }


def ipv4_str(value: int) -> str:
    return ".".join(str((value >> shift) & 0xFF) for shift in (24, 16, 8, 0))


def is_multicast_or_broadcast_ip(ip: str) -> bool:
    if ip == "255.255.255.255":
        return True
    parts = [int(p) for p in ip.split(".")]
    return parts[0] >= 224 or parts[-1] == 255


def udp_bucket(event: dict[str, Any]) -> str:
    src_ip = ipv4_str(event.get("src_ipv4", 0))
    dst_ip = ipv4_str(event.get("dst_ipv4", 0))
    src_port = event.get("src_port", 0)
    dst_port = event.get("dst_port", 0)
    if src_ip == "0.0.0.0" or dst_ip == "0.0.0.0" or src_port == 0 or dst_port == 0:
        return "invalid_udp"
    if src_port in WFC_SERVICE_UDP_PORTS or dst_port in WFC_SERVICE_UDP_PORTS:
        return "wfc_service_udp"
    if src_port in NATNEG_UDP_PORTS or dst_port in NATNEG_UDP_PORTS:
        return "natneg_udp"
    if (
        src_port in LAN_NOISE_PORTS or dst_port in LAN_NOISE_PORTS or
        is_multicast_or_broadcast_ip(src_ip) or is_multicast_or_broadcast_ip(dst_ip)
    ):
        return "lan_noise_udp"
    return "candidate_peer_udp"


def summarize_online_transport(report: dict[str, Any]) -> dict[str, Any]:
    stages = report.get("net_evidence", {})
    last_tag = None
    for tag in ("F_after_cancel", "E_search_observed", "D_search_started",
                "D_match_setup_screen", "C_login", "B_conntest"):
        if tag in stages:
            last_tag = tag
            break

    stage_summaries: dict[str, Any] = {}
    totals: Counter[str] = Counter()
    udp_endpoints: Counter[tuple[str, int, str, int, str]] = Counter()
    for tag, stage in stages.items():
        kinds = stage.get("kinds", {})
        udp_counts: Counter[str] = Counter()
        for event in kinds.get("udp_packet", []):
            bucket = udp_bucket(event)
            udp_counts[bucket] += 1
            if tag == last_tag:
                udp_endpoints[(
                    ipv4_str(event.get("src_ipv4", 0)),
                    event.get("src_port", 0),
                    ipv4_str(event.get("dst_ipv4", 0)),
                    event.get("dst_port", 0),
                    bucket,
                )] += 1
        stage_summary = {
            "kind_counts": {kind: len(events) for kind, events in kinds.items()},
            "udp_counts": dict(udp_counts),
        }
        stage_summaries[tag] = stage_summary
        for kind, count in stage_summary["kind_counts"].items():
            totals[kind] = max(totals[kind], count)

    search_safety = report.get("search_safety", [])
    max_search_diff = max(
        (entry.get("diff_from_empty_search_baseline", 0.0)
         for entry in search_safety),
        default=0.0,
    )
    search_threshold = SEARCH_PLACEHOLDER_DIFF_THRESHOLD
    if search_safety:
        search_threshold = search_safety[0].get(
            "threshold", SEARCH_PLACEHOLDER_DIFF_THRESHOLD)

    udp_totals = Counter()
    if last_tag:
        udp_totals.update(stage_summaries[last_tag]["udp_counts"])

    backend_errors = totals.get("backend_error", 0)
    tcp_resets = totals.get("tcp_reset", 0)
    if backend_errors:
        status = "backend_errors_observed"
    elif tcp_resets:
        status = "tcp_resets_observed"
    elif udp_totals["natneg_udp"] > 0 and udp_totals["candidate_peer_udp"] > 0:
        status = "natneg_and_candidate_peer_udp_observed"
    elif udp_totals["natneg_udp"] > 0:
        status = "natneg_without_candidate_peer_udp"
    elif udp_totals["wfc_service_udp"] > 0:
        status = "server_browser_only_no_natneg"
    elif report.get("stopped_at_match_setup"):
        status = "authenticated_menu_only"
    else:
        status = "no_online_udp_observed"

    return {
        "status": status,
        "last_stage": last_tag,
        "max_kind_counts": dict(totals),
        "last_stage_udp_counts": dict(udp_totals),
        "top_udp_endpoints": [
            {
                "src_ip": src_ip,
                "src_port": src_port,
                "dst_ip": dst_ip,
                "dst_port": dst_port,
                "bucket": bucket,
                "count": count,
            }
            for (src_ip, src_port, dst_ip, dst_port, bucket), count
            in udp_endpoints.most_common(16)
        ],
        "search_safety": {
            "checks": len(search_safety),
            "max_diff_from_empty_search_baseline": round(max_search_diff, 3),
            "threshold": search_threshold,
            "passed": max_search_diff <= search_threshold,
        },
        "stage_summaries": stage_summaries,
        "notes": [
            "This summarizes one-client public-search/menu evidence only.",
            "M7 completion still requires two-client peer UDP and race entry.",
        ],
    }


def print_online_transport_summary(summary: dict[str, Any]) -> None:
    print("\n--- online transport summary ---")
    print(
        f"status={summary.get('status')} "
        f"last_stage={summary.get('last_stage')}"
    )
    print(f"last_stage_udp_counts={summary.get('last_stage_udp_counts', {})}")
    search = summary.get("search_safety", {})
    if search:
        print(
            "search_safety="
            f"checks={search.get('checks')} "
            f"max_diff={search.get('max_diff_from_empty_search_baseline')} "
            f"threshold={search.get('threshold')} "
            f"passed={search.get('passed')}"
        )
    for endpoint in summary.get("top_udp_endpoints", [])[:8]:
        print(
            "  udp "
            f"{endpoint['src_ip']}:{endpoint['src_port']} -> "
            f"{endpoint['dst_ip']}:{endpoint['dst_port']} "
            f"{endpoint['bucket']} count={endpoint['count']}"
        )


def print_net_evidence_summary(report: dict[str, Any], kinds=(
        "dns_query", "dns_response", "tcp_open", "tcp_close", "tcp_reset",
        "udp_packet", "backend_drop", "backend_error", "tls_record")) -> None:
    print(f"net_state: {report['net_state']}")
    for kind in kinds:
        events = report["kinds"].get(kind, [])
        if not events:
            continue
        print(f"\n[{kind}] {len(events)} event(s):")
        for e in events:
            line = (f"  #{e['count']} sys={e['sys']} dir={e['direction']} "
                    f"{ipv4_str(e['src_ipv4'])}:{e['src_port']} -> "
                    f"{ipv4_str(e['dst_ipv4'])}:{e['dst_port']} "
                    f"len={e['payload_len']} aux={e['aux']}")
            if e.get("hostname"):
                line += f" host={e['hostname']}"
            print(line)


def check_no_players_found(client: DebugClient, shots_dir: Path, label: str,
                           baseline_img=None) -> None:
    """Courtesy stop condition (see module docstring). This project has no
    committed reference image for a "players found" state (none has been
    observed), so this deliberately does NOT try to positive-match one. It
    fails closed instead: compare the current frame to this run's own
    initial empty-search placeholder, and stop if the screen changed enough
    that a human should inspect it before any further input.
    """
    img = capture_rgb(client)
    shots_dir.mkdir(parents=True, exist_ok=True)
    path = shots_dir / f"{label}.png"
    img.save(path)
    if baseline_img is None:
        return img
    diff = mean_abs_diff(img, baseline_img, "full")
    if diff > SEARCH_PLACEHOLDER_DIFF_THRESHOLD:
        raise PlayersFoundError(
            f"{label}: search screen changed from empty-search baseline "
            f"(diff={diff:.2f}, threshold={SEARCH_PLACEHOLDER_DIFF_THRESHOLD}); "
            f"screenshot: {path}")
    return img


# ---------------------------------------------------------------------------
def run_full_scenario(port: int, shots_dir: Path, stall: int = STALL_DEFAULT,
                       search_observe_steps: int = 20,
                       search_stride: int = 200,
                       stop_at_match_setup: bool = False,
                       ) -> dict[str, Any]:
    """Boot from firmware, through the WFC setup + NAS login + presence
    flow, into the real "Choose the conditions for this match" screen, then
    start a REGIONAL opponent search (the more conservative of the two live
    search scopes -- fewer potential real opponents than WORLDWIDE -- while
    still exercising the actual server browser/matchmaking service, unlike
    FRIEND ROSTER which never touches it), observe it passively for a
    bounded window, then cancel cleanly. Returns a report dict with the
    regression gate, per-stage net evidence, and the step timeline.
    """
    shots_dir.mkdir(parents=True, exist_ok=True)
    client = DebugClient(port=port, timeout=900.0)
    report: dict[str, Any] = {
        "steps": [],
        "net_evidence": {},
        "search_safety": [],
    }

    def step(name: str) -> None:
        counts = client.cmd("event_counts")
        report["steps"].append({"name": name, "t": time.time(), "vblank9": counts.get("vblank9")})
        print(f"-- step: {name} @ vblank9={counts.get('vblank9')}")

    def net_dump(tag: str, kinds=ALL_NET_KINDS) -> None:
        report["net_evidence"][tag] = dump_net_evidence(client, kinds)
        n = sum(len(v) for v in report["net_evidence"][tag]["kinds"].values())
        print(f"  [net dump {tag}] {n} classified events; "
              f"net_state={report['net_evidence'][tag]['net_state']}")

    try:
        report["regression_gate"] = run_regression_gate(client, stall)
        print_regression_gate(report["regression_gate"])

        client.cmd("touch", x=CART_ICON[0], y=CART_ICON[1], down=True)
        client.cmd("run_to_event", event="vblank9", count=130, stall=stall)
        client.cmd("touch", x=CART_ICON[0], y=CART_ICON[1], down=False)
        hit = client.cmd("run_to_event", event="ipcsync_w", count=GAME_BOOT_IPCSYNC_W, stall=stall)
        if not hit.get("reached") or hit.get("terminal"):
            raise RuntimeError(f"cart launch failed to reach the game-boot IPC handshake: {hit}")
        step("cart_boot_handshake")

        client.cmd("run_to_event", event="vblank9", count=900, stall=stall)
        shoot(client, shots_dir / "01_title_screen.png")
        step("title_screen")

        tap(client, TITLE_NINTENDO_WFC, 910, 1000, stall)
        shoot(client, shots_dir / "02_nickname_confirm_dialog.png")
        tap(client, DIALOG_YES, 1010, 1100, stall)
        shoot(client, shots_dir / "03_onscreen_name_confirm_dialog.png")
        tap(client, DIALOG_YES, 1110, 1250, stall)
        shoot(client, shots_dir / "04_custom_emblem_dialog.png")
        tap(client, DIALOG_NO, 1260, 1400, stall)
        shoot(client, shots_dir / "05_title_screen_after_setup.png")
        tap(client, TITLE_NINTENDO_WFC, 1410, 1500, stall)
        shoot(client, shots_dir / "06_wfc_transition.png")
        client.cmd("run_to_event", event="vblank9", count=1700, stall=stall)
        shoot(client, shots_dir / "07_wfc_connection_menu.png")
        step("wfc_connection_menu")

        tap(client, WFC_SETTINGS_MENU_ITEM, 1710, 1850, stall)
        shoot(client, shots_dir / "08_wfc_connection_setup_step1.png")
        tap(client, WFC_CONNECTION_SETTINGS_TILE, 1860, 2000, stall)
        shoot(client, shots_dir / "09_connection_slot_picker.png")
        tap(client, CONNECTION_SLOT_1, 2010, 2150, stall)
        shoot(client, shots_dir / "10_connection1_settings_step2.png")
        tap(client, SEARCH_FOR_AP, 2160, 2300, stall)
        shoot(client, shots_dir / "11_searching_for_ap.png")
        client.cmd("run_to_event", event="vblank9", count=3500, stall=stall)
        shoot(client, shots_dir / "12_ap_list_found.png")
        step("ap_list_found")
        net_dump("A_ap_scan", ("dhcp", "arp", "wifi_association", "wifi_state_change",
                                "ethernet_tx", "ethernet_rx"))

        tap(client, AP_LIST_FIRST_ROW, 3510, 3650, stall)
        shoot(client, shots_dir / "13_ap_selected_connection_test_prompt.png")
        _, test_start, test_start_img = start_connection_test(
            client, shots_dir, 3660, 3800, stall)
        test_start_img.save(shots_dir / "14_connection_test_running.png")
        _, test_target, test_img = wait_for_connection_test_result(
            client, shots_dir, "connection_test", start=test_start,
            stride=100, max_extra=4000, stall=stall)
        print(f"  connection test succeeded at vblank9={test_target}")
        test_img.save(shots_dir / "15_connection_test_settled.png")
        step("connection_test_settled")
        net_dump("B_conntest")

        press_a(client, test_target + 10, test_target + 140, stall)
        _, title_target = converge_to_title(
            client, shots_dir, "converge_to_title",
            start=test_target + 140, stall=stall)
        shoot(client, shots_dir / "16_back_at_title_with_saved_connection.png")
        step("back_at_title_with_saved_connection")

        tap(client, TITLE_NINTENDO_WFC, title_target + 10,
            title_target + 160, stall)
        _, menu_target, menu_img = wait_for_screen(
            client, shots_dir, "wfc_connection_menu_direct",
            "wfc_connection_menu", start=title_target + 160, stride=50,
            max_extra=800, stall=stall,
            diag_candidates=("nickname_confirm_dialog", "title_screen"),
            verbose=False)
        menu_img.save(shots_dir / "17_wfc_connection_menu_direct.png")
        step("wfc_connection_menu_direct")

        tap(client, WFC_MATCH, menu_target + 10, menu_target + 140, stall)
        _, disclaimer_target, disclaimer_img = wait_for_screen(
            client, shots_dir, "wfc_match_disclaimer",
            "wfc_match_disclaimer", start=menu_target + 140, stride=50,
            max_extra=800, stall=stall,
            diag_candidates=("wfc_match_save_confirm", "wfc_connecting",
                             "wfc_login_settled", "wfc_match_setup_screen"),
            margin=2.0, verbose=False)
        disclaimer_img.save(shots_dir / "18_wfc_match_disclaimer.png")
        step("wfc_match_disclaimer")

        tap(client, ONE_BUTTON_DIALOG, disclaimer_target + 10,
            disclaimer_target + 140, stall)
        _, confirm_target, confirm_img = wait_for_screen(
            client, shots_dir, "wfc_match_save_confirm",
            "wfc_match_save_confirm", start=disclaimer_target + 140,
            stride=50, max_extra=800, stall=stall,
            diag_candidates=("wfc_match_disclaimer", "wfc_connecting",
                             "wfc_login_settled", "wfc_match_setup_screen"),
            margin=2.0, verbose=False)
        confirm_img.save(shots_dir / "19_wfc_match_save_confirm.png")
        step("wfc_match_save_confirm")

        tap(client, WFC_MATCH_CONFIRM_YES, confirm_target + 10,
            confirm_target + 140, stall)
        _, connecting_target, connecting_img = wait_for_screen(
            client, shots_dir, "wfc_connecting",
            "wfc_connecting", start=confirm_target + 140, stride=50,
            max_extra=800, stall=stall,
            diag_candidates=("wfc_match_disclaimer", "wfc_match_save_confirm",
                             "wfc_login_settled", "wfc_match_setup_screen"),
            margin=2.0, verbose=False)
        connecting_img.save(shots_dir / "20_wfc_connecting.png")
        step("wfc_connecting_tap_sent")

        print("polling for real NAS login settlement "
              "(argmin-classified, no bare-threshold false-accept)...")
        hit, target, img = wait_for_screen(
            client, shots_dir, "login", "wfc_login_settled",
            start=connecting_target, stride=200, max_extra=20000, stall=stall,
            diag_candidates=("wfc_connecting", "wfc_login_next", "wfc_match_setup_screen"),
            margin=2.0, verbose=False)
        print(f"  real settlement at vblank9={target} "
              f"(+{target - connecting_target} VBlanks past the connecting screen)")
        img.save(shots_dir / "21_wfc_login_settled.png")
        step("wfc_login_settled")
        net_dump("C_login")

        v = target
        tap(client, ONE_BUTTON_DIALOG, v + 10, v + 140, stall)
        _, next_target, next_img = wait_for_screen(
            client, shots_dir, "wfc_login_next", "wfc_login_next",
            start=v + 140, stride=50, max_extra=1500, stall=stall,
            diag_candidates=("wfc_connecting", "wfc_login_settled",
                             "wfc_match_setup_screen"),
            margin=2.0, verbose=False)
        next_img.save(shots_dir / "22_wfc_login_next.png")
        step("wfc_login_next")

        tap(client, ONE_BUTTON_DIALOG, next_target + 10,
            next_target + 140, stall)
        _, setup_target, setup_img = wait_for_screen(
            client, shots_dir, "wfc_match_setup_screen",
            "wfc_match_setup_screen", start=next_target + 140, stride=50,
            max_extra=1500, stall=stall,
            diag_candidates=("wfc_connecting", "wfc_login_settled",
                             "wfc_login_next"),
            margin=2.0, verbose=False)
        setup_img.save(shots_dir / "23_wfc_match_setup_screen.png")
        step("wfc_match_setup_screen")
        net_dump("D_match_setup_screen")
        v2 = setup_target

        if stop_at_match_setup:
            report["stopped_at_match_setup"] = True
            print("stopping before public matchmaking as requested")
            print("\n=== STEP TIMELINE ===")
            t0 = report["steps"][0]["t"]
            for s in report["steps"]:
                print(f"  {s['name']:<36} vblank9={s['vblank9']:<8} "
                      f"+{s['t'] - t0:6.1f}s")
            print("SCENARIO COMPLETE")
            return report

        # New territory past here (see MATCH_SETUP_OK's comment for why the
        # first coordinate attempt at this button was a proven no-op).
        tap(client, MATCH_SETUP_OK, v2 + 10, v2 + 140, stall)
        search_baseline = capture_rgb(client)
        search_baseline.save(shots_dir / "24_searching_for_opponents_regional.png")
        step("search_started_regional")
        net_dump("D_search_started")

        v3 = v2 + 140
        found_players = False
        for i in range(1, search_observe_steps + 1):
            target_v = v3 + i * search_stride
            hit = client.cmd("run_to_event", event="vblank9", count=target_v, stall=stall)
            fname = shots_dir / f"25_searching_observe_{i:02d}_vblank{target_v}.png"
            img = check_no_players_found(client, shots_dir, fname.stem,
                                         search_baseline)
            report["search_safety"].append({
                "label": fname.stem,
                "vblank9": target_v,
                "diff_from_empty_search_baseline": mean_abs_diff(
                    img, search_baseline, "full"),
                "threshold": SEARCH_PLACEHOLDER_DIFF_THRESHOLD,
            })
            if hit.get("terminal") or hit.get("stalled"):
                print(f"  execution stalled/halted mid-search at step {i}: {hit}")
                break
        step("search_observation_window_complete")
        net_dump("E_search_observed")

        # Courtesy: this driver never proceeds past an active search into a
        # race. The observation loop above only ever advances time and
        # screenshots -- it never taps -- so no input has been sent that
        # could confirm/join anything. Each saved 25_searching_observe_*.png
        # frame was also compared against this run's initial empty-search
        # baseline; a changed or ambiguous frame raises PlayersFoundError
        # before any cancel/confirm input is sent.
        pre_cancel_img = check_no_players_found(
            client, shots_dir, "26_pre_cancel_check", search_baseline)
        report["search_safety"].append({
            "label": "26_pre_cancel_check",
            "vblank9": client.cmd("event_counts").get("vblank9"),
            "diff_from_empty_search_baseline": mean_abs_diff(
                pre_cancel_img, search_baseline, "full"),
            "threshold": SEARCH_PLACEHOLDER_DIFF_THRESHOLD,
        })

        cur = client.cmd("event_counts")
        v4 = cur["vblank9"]
        tap(client, CANCEL_SEARCH_BACK_ARROW, v4 + 10, v4 + 140, stall)
        shoot(client, shots_dir / "27_after_cancel_search.png")
        step("cancel_search_tapped")
        net_dump("F_after_cancel")

        cur = client.cmd("event_counts")
        v5 = cur["vblank9"]
        press_a(client, v5 + 10, v5 + 140, stall)
        shoot(client, shots_dir / "28_after_dismissing_disconnect_screen.png")
        step("dismissed_disconnect_screen")

        print("\n=== STEP TIMELINE ===")
        t0 = report["steps"][0]["t"]
        for s in report["steps"]:
            print(f"  {s['name']:<36} vblank9={s['vblank9']:<8} "
                  f"+{s['t'] - t0:6.1f}s")
        print("SCENARIO COMPLETE")
    finally:
        client.close()

    return report


def cancel_search(client: DebugClient, shots_dir: Path, current_vblank9: int,
                   stall: int = STALL_DEFAULT) -> Any:
    """Standalone helper mirroring the cancel step inside run_full_scenario,
    for a caller resuming an already-live search on an existing connection.
    Observed live against real Kaeru (2026-08-10): tapping the back arrow
    from an active, no-opponents-found search produces a full-screen
    "You were disconnected from Nintendo WFC. Press the A Button to return
    to the menu. Error Code 91010" message -- confirmed BENIGN, not a
    network fault: the net ring's tcp_close events for the GPCM session
    (port 29900) around the same `sys` timestamp are clean FIN/FIN-ACK
    closes (aux=17) on both directions, with zero tcp_reset/backend_drop/
    backend_error events anywhere in the whole session. This is the game's
    own "you cancelled matchmaking" flow, verbatim WFC error code and all --
    report it as such, not as a failure.
    """
    tap(client, CANCEL_SEARCH_BACK_ARROW, current_vblank9 + 10, current_vblank9 + 140, stall)
    img = capture_rgb(client)
    img.save(shots_dir / "27_after_cancel_search.png")
    return img


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=19842,
                         help="debug-protocol port of the native nds_runner "
                              "under test (default 19842; pass a private "
                              "port if sharing this machine with another "
                              "agent's server -- see module docstring)")
    parser.add_argument("--shots-dir", type=Path, default=DEFAULT_SHOTS_DIR)
    parser.add_argument("--stall", type=int, default=STALL_DEFAULT)
    parser.add_argument("--search-observe-steps", type=int, default=20,
                         help="how many bounded, tap-free observation "
                              "strides to run once the opponent search "
                              "starts before cancelling")
    parser.add_argument("--search-stride", type=int, default=200,
                         help="VBlanks per observation stride")
    parser.add_argument("--evidence-json", type=Path, default=None,
                         help="also write the full structured report "
                              "(regression gate + per-stage net evidence + "
                              "step timeline) to this path as JSON")
    parser.add_argument("--stop-at-match-setup", action="store_true",
                         help="stop after authenticated match setup menu "
                              "instead of starting a public opponent search")
    parser.add_argument("--summarize-evidence", type=Path, default=None,
                         help="read an existing mkds_online_menus evidence "
                              "JSON, print the transport summary, and exit")
    args = parser.parse_args()

    if args.summarize_evidence:
        with open(args.summarize_evidence, "r") as f:
            report = json.load(f)
        summary = summarize_online_transport(report)
        print_online_transport_summary(summary)
        return 0

    if Image is None:
        print("Pillow is required (screen capture/verification) -- "
              "install it in the interpreter running this script.",
              file=sys.stderr)
        return 2

    report = run_full_scenario(args.port, args.shots_dir, args.stall,
                                args.search_observe_steps, args.search_stride,
                                args.stop_at_match_setup)

    if args.stop_at_match_setup:
        print("\n--- final net evidence summary (authenticated menu) ---")
        if "D_match_setup_screen" in report["net_evidence"]:
            print_net_evidence_summary(report["net_evidence"]["D_match_setup_screen"])
    else:
        print("\n--- final net evidence summary (search + cancel window) ---")
    if "E_search_observed" in report["net_evidence"]:
        print_net_evidence_summary(report["net_evidence"]["E_search_observed"])
    if "F_after_cancel" in report["net_evidence"]:
        print_net_evidence_summary(report["net_evidence"]["F_after_cancel"])

    report["online_transport_summary"] = summarize_online_transport(report)
    print_online_transport_summary(report["online_transport_summary"])

    if args.evidence_json:
        args.evidence_json.parent.mkdir(parents=True, exist_ok=True)
        with open(args.evidence_json, "w") as f:
            json.dump(report, f, indent=1)
        print(f"\nfull structured report written to {args.evidence_json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
