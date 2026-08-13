#!/usr/bin/env python3
"""Drive Mario Kart DS, from a cold LLE boot, through the real touch UI into
its Nintendo WFC "Search for an Access Point" flow -- and compare native
against the melonDS oracle at every step (beads-yjp task I7).

This is the scripted, reusable equivalent of manually tapping through:

  firmware menu -> tap the cart icon -> MKDS title screen
  -> tap NINTENDO WFC -> nickname/emblem first-run setup
  -> tap NINTENDO WFC again -> Nintendo Wi-Fi Connection menu
  -> tap NINTENDO WFC SETTINGS -> Connection 1 -> Search for an Access
     Point -> (network scan) -> tap the discovered "ndsrecomp" AP
  -> A to start the connection test

Every touch coordinate below was derived empirically, not guessed: each was
found by reading the actual rendered `framebuffer` (bottom/touch screen,
256x192) at the relevant step, thresholding brightness/color to locate
button edges, and picking a point safely inside the interior (see the
`*_BOX` comments -- ranges observed against a live vblank9-tagged capture,
not eyeballed from a thumbnail). Screenshots for every step are written to
`generated/captures/wiimmfi-i7/` in the game worktree.

Timing follows the same lesson as `mkds_cart_icon_launch_smoke.py`: touch
reactions are a multi-VBlank state machine (menu fade-outs, network scans),
so every step runs generously far forward before the next input or
checkpoint -- never judged "no reaction" from an immediate snapshot.

Rendezvous discipline (beads-yjp.7): the one truly asynchronous transition
in this whole flow -- the cart-icon tap booting into MKDS's own game code --
is rendezvoused on the hardware event it actually is (`ipcsync_w` reaching
12520), never a raw VBlank count (see `mkds_cart_icon_launch_smoke.py`'s
docstring for the full rationale). Every step *after* that point is a
scripted, deterministically-timed input applied at an absolute `vblank9`
target measured from the SAME already-rendezvoused instant on both
machines -- not "wait an arbitrary amount then compare," but "advance both
identically-seeded deterministic machines by the same counted number of
VBlank IRQs after the same architectural sync point," which is the same
methodology `oracle/firmware_traversal.py`'s `run` action already uses for
scripted post-sync navigation. Every checkpoint below still asserts full
hardware-event-counter equality (never a tolerance band) so a genuine
timing divergence cannot slip through disguised as "expected skew."

Launch both servers with the SAME cartridge inserted and `manual` startup:

  nds_runner <bios-dir> --serve --port 19842 --config game.toml \
      --rom "Mario Kart DS.nds" --no-save --startup-mode manual \
      --network on --wfc on --wfc-provider kaeru

`--network on` is REQUIRED and is not the default: host networking defaults
to off (runner/src/frontend.h, owner decision 2026-08-11) so that offline
sessions do not construct a libslirp NAT they never use. Without it this
scenario reaches the AP search and then fails, because no host backend
exists to answer DHCP. `--no-save` is equally required and not optional --
this script drives the FIRST-RUN nickname/emblem flow, which only appears
when no cartridge save exists, and it is also what keeps two concurrent
instances from sharing one save (see tools/run_two_instances.ps1 and
beads-yjp.1.15).
  ndsref --bios9 <bios9.rom> --bios7 <bios7.rom> --firmware <firmware.bin> \
      --rom "Mario Kart DS.nds" --boot firmware --port 19843

  python oracle/mkds_wfc_scenario.py [--shots-dir DIR]

Native-only runs include the debug port in their screenshot label by default
so concurrent drivers do not overwrite each other's diagnostics.
"""

from __future__ import annotations

import argparse
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

from _client import DebugClient

try:
    from PIL import Image
except ImportError:  # pragma: no cover - Pillow is a repo dependency
    Image = None


GAME_ROOT = Path(__file__).resolve().parents[2] / "mariokartdsrecomp-wiimmfi"
DEFAULT_SHOTS_DIR = GAME_ROOT / "generated" / "captures" / "wiimmfi-i7"

# ---------------------------------------------------------------------------
# Screen-content verification (beads-yjp I12, determinism fix).
#
# The historical version of this driver synchronized the WFC-setup ->
# NAS-login tail entirely on fixed VBlank offsets measured from the last
# rendezvous point. That works when every step's timing is guest-
# deterministic, but two things in this flow are NOT: the connection-test
# round trip (plain HTTP) and, much more so, the real NAS login handshake
# (DNS -> TCP -> SSLv3 -> HTTP against Kaeru), whose latency is real-host-
# network-dependent. A fixed offset picked to cover the common case will,
# on a slow trip, tap the "NEXT"/"OK"/YES coordinates on a screen that is
# STILL showing "Connecting to Nintendo WFC..." -- which is silently
# accepted as input by whatever screen happens to be there, walking the
# menu state machine somewhere else entirely (the "unrelated wizard page"
# failure mode this task was asked to fix). Empirically: on 2026-08-10, the
# `wiimmfi-m5-final` capture directory contains three screenshots
# (frames 22-24) saved under checkpoint names "wfc_login_settled",
# "wfc_login_next", "wfc_match_setup_screen" that are, byte for byte,
# identical to frame 21's "wfc_connecting" -- proof the fixed-offset
# schedule silently sailed past a login that had not actually finished.
#
# The fix: every checkpoint below that has a saved reference image in
# oracle/wfc_screen_refs/ is either (a) verified immediately, with a loud
# exception naming the expected vs. actual screen on mismatch (for steps
# already established as guest-deterministic -- see the per-name comment
# in REGION_FOR/THRESHOLD_FOR), or (b) polled for -- advancing vblank9 in
# small strides, re-checking the framebuffer each time, up to a bounded
# overall budget -- for the two steps whose timing is real-network-
# dependent (the connection test and the NAS login). This is the "poll the
# framebuffer until the expected screen is present, with a bounded overall
# timeout and a clear failure message" option from the task brief; ring-
# event rendezvous was evaluated too (see the module docstring's `NET_KINDS`
# / `net_ring_dump` machinery below) but `run_to_event` only accepts the
# hardware-counter names in `nds_event_value()` (vblank9/7, ipcsync_w,
# fifo9to7/7to9, dma_done, timer_ovf, spi_w, soundbias_w, insn9/7) --
# network-layer events (dns_query, tcp_open, tls_record, ...) exist only in
# the passive ring, not as a `run_to_event` target -- so there is no
# hardware counter to rendezvous execution on for "the AP responded" or
# "the NAS TLS handshake finished"; screen-content polling is the correct
# tool for those two specific steps, not a fallback of convenience.
#
# Reference images and thresholds were derived empirically, not guessed:
# every reference in wfc_screen_refs/ was cropped from an already-captured,
# manually-verified-correct screenshot (cross-checked against 4-9
# independently-run historical captures of the same checkpoint spanning
# multiple real sessions), and every threshold was chosen by measuring
# actual mean-absolute-grayscale-pixel-difference between independent
# real captures of the SAME screen (intra-class distance, found to be
# exactly 0.0 for every guest-deterministic screen and up to ~15-46 for the
# two real-network-timing-dependent screens, entirely from decorative
# background animation phase -- see REGION_FOR's per-name notes) versus
# DIFFERENT screens (inter-class distance, >=~30 in every case checked).
# Thresholds below sit comfortably inside that measured gap, not at an
# arbitrary round number.
try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:  # pragma: no cover - Pillow is a repo dependency
    ImageChops = None
    ImageStat = None

REFS_DIR = Path(__file__).with_name("wfc_screen_refs")

# Which crop of the 256x384 top+bottom composite actually carries the
# content that distinguishes this screen, established by direct
# byte-for-byte inspection (see the task-session notes): dialog/status TEXT
# for the late WFC-login flow renders on the TOP screen (engine A, rows
# 0-191) with only decorative scrolling-checkerboard background on the
# bottom screen -- and that decorative background is exactly the thing
# that differs between two real, differently-timed network round trips
# (measured: connection_test_settled's TOP screen was byte-identical
# between two independently-timed real runs while its BOTTOM screen
# differed in up to 84% of pixels in the bottom-most band). Earlier
# menu/dialog screens instead carry their distinguishing content across
# BOTH screens (e.g. title screen artwork top + button list bottom), and
# are fully guest-deterministic, so the full composite is used there.
REGION_FOR = {
    "title_screen": "full",
    "nickname_confirm_dialog": "full",
    "wfc_connection_menu": "full",
    "wfc_connection_setup_step1": "full",
    # CORRECTED after a live run caught the bug empirically (see
    # wait_for_connection_test_result's docstring): the WFC *Settings* app's
    # status text (this screen, plus its "running"/"error" siblings) renders
    # on the BOTTOM screen inside a narrow band, not the top screen -- the
    # opposite of the late WFC *Match*/login dialogs below, whose text is on
    # top. A first version of this file used region="top" here, under which
    # "Testing connection..." and "Connection successful." are BYTE-IDENTICAL
    # (the status text is entirely on the bottom screen) -- i.e. it could
    # never have distinguished "still running" from "settled" at all. Fixed
    # to "test_band" (composite rows 230-344, the message pane) below.
    "connection_test_running": "test_band",
    "connection_test_settled": "test_band",
    "connection_test_error": "test_band",
    "wfc_match_disclaimer": "full",
    "wfc_connecting": "full",
    "wfc_match_save_confirm": "full",
    "wfc_login_settled": "top",           # real-network-timing-dependent
    "wfc_login_next": "top",              # real-network-timing-dependent
    "wfc_match_setup_screen": "top",      # real-network-timing-dependent
}

# Mean-abs-grayscale-pixel-difference thresholds. Default 10 for the
# guest-deterministic screens (measured intra-class distance: 0.0; nearest
# different screen: >=~30) with headroom for PNG/theme dithering noise;
# 20 for the three real-network-dependent late screens (measured
# intra-class distance up to ~15 for connection_test_settled) to absorb
# real, legitimate host-timing jitter without materially eroding the
# separation from a genuinely different screen.
THRESHOLD_FOR = {
    "wfc_login_settled": 20.0,
    "wfc_login_next": 20.0,
    "wfc_match_setup_screen": 20.0,
}
DEFAULT_THRESHOLD = 10.0
CONNECTION_TEST_OUTCOME_MAX_DIFF = 25.0
ERROR_CODE_LABEL_REGION = (4, 224, 94, 240)
ERROR_CODE_LABEL_THRESHOLD = 5.0
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
    """Same composite (top screen over bottom screen) shoot() writes to
    disk, but returned as an in-memory Image for comparison."""
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
        # The WFC *Settings* app's status message pane (composite rows
        # 230-344 == bottom-screen-local rows 38-152), empirically located
        # by diffing "connection_test_running" against "connection_test_
        # settled" and finding the differing-pixel bounding box (y 230-344,
        # x 13-244) -- see connection_test_settled's REGION_FOR comment.
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


def verify_screen(client: DebugClient, shots_dir: Path, label: str,
                   expect: str, diag_candidates: tuple[str, ...] = ()) -> Any:
    """Check the CURRENT framebuffer against `expect`'s reference image
    immediately (no polling/retry) -- for checkpoints already established
    as guest-deterministic, where a mismatch means the driver itself has a
    bug (wrong offset, wrong coordinate), not real-network jitter. Fails
    loudly, naming the expected screen and the closest known screen
    actually observed, plus a saved diagnostic screenshot, instead of
    silently trusting the schedule and mislabeling whatever was captured
    (the defect this task was asked to fix)."""
    img = capture_rgb(client)
    d = mean_abs_diff(img, load_ref(expect), REGION_FOR.get(expect, "full"))
    if d <= THRESHOLD_FOR.get(expect, DEFAULT_THRESHOLD):
        return img
    names = (expect,) + tuple(n for n in diag_candidates if n != expect)
    diffs = diff_report(img, names)
    best = min(diffs, key=diffs.get)
    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / f"{label}_MISMATCH_wanted_{expect}.png"
    img.save(fail_path)
    raise ScreenTimeoutError(
        f"{label}: expected screen {expect!r} (diff={d:.1f} > "
        f"threshold={THRESHOLD_FOR.get(expect, DEFAULT_THRESHOLD)}); closest "
        f"known screen actually on-screen is {best!r} (diff={diffs[best]:.1f}); "
        f"all candidate diffs={diffs}; diagnostic screenshot: {fail_path}"
    )


def wait_for_screen(client: DebugClient, shots_dir: Path, label: str,
                     expect: str, start: int, stride: int, max_extra: int,
                     stall: int, diag_candidates: tuple[str, ...] = ()
                     ) -> tuple[Any, int]:
    """Poll vblank9 forward from `start` in `stride`-sized steps (never a
    single fixed jump) until `expect`'s reference screen appears, up to
    `max_extra` VBlanks total budget. Returns (last run_to_event result,
    vblank9 target it matched at). Raises ScreenTimeoutError -- naming the
    expected screen and the closest known screen actually seen, plus a
    saved screenshot -- if the budget is exhausted or execution stalls/
    halts, instead of ever silently proceeding into an unverified screen."""
    target = start
    img = None
    hit = None
    while True:
        hit = client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
        if hit.get("terminal") or hit.get("stalled"):
            raise ScreenTimeoutError(
                f"{label}: execution stalled/halted waiting for {expect!r} "
                f"at vblank9 target={target}: {hit}"
            )
        img = capture_rgb(client)
        d = mean_abs_diff(img, load_ref(expect), REGION_FOR.get(expect, "full"))
        if d <= THRESHOLD_FOR.get(expect, DEFAULT_THRESHOLD):
            return hit, target
        if target - start >= max_extra:
            break
        target += stride
    names = (expect,) + tuple(n for n in diag_candidates if n != expect)
    diffs = diff_report(img, names)
    best = min(diffs, key=diffs.get)
    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / f"{label}_TIMEOUT_wanted_{expect}.png"
    img.save(fail_path)
    raise ScreenTimeoutError(
        f"{label}: timed out after vblank9 {start}..{target} (budget "
        f"{max_extra}) waiting for screen {expect!r}; closest known screen "
        f"actually on-screen is {best!r} (diff={diffs[best]:.1f}); all "
        f"candidate diffs={diffs}; diagnostic screenshot: {fail_path}"
    )


# Every screen in the WFC Match -> NAS-login chain shares near-identical
# chrome (one blue message box, white text, the same scrolling checkerboard
# background) and differs only in the message text -- caught empirically on
# a real, otherwise-successful run against Kaeru: single-reference
# threshold checks (wait_for_screen/verify_screen) mis-fired here. Measured
# full-composite mean-abs-diff between this family's members is only
# ~2.3-13.3 (vs. >=~30 for every guest-deterministic early-game screen
# checked earlier), which no single fixed threshold can straddle safely --
# "wfc_login_next"'s own reference sat only 7.6 away from "wfc_connecting"
# under a 20.0 threshold, so a frame that was still showing "Connecting to
# Nintendo WFC..." was accepted as "From now on please use this DS...": the
# exact "wrong filename/wrong screen" defect this whole task exists to fix,
# reproduced by the fix's own first draft. wait_for_nearest_screen replaces
# every late-chain wait_for_screen call with a closed-set nearest-of-N
# classification instead (still region="full", still debounced) -- the
# smallest measured margin here is wfc_login_settled vs wfc_login_next at
# 3.26, so this is not bulletproof against a screen that changes value
# mid-poll in exactly the wrong instant, but is decisively better than a
# threshold that could not distinguish them at all.
LATE_CHAIN_CANDIDATES = (
    "wfc_match_disclaimer", "wfc_match_save_confirm", "wfc_connecting",
    "wfc_login_settled", "wfc_login_next", "wfc_match_setup_screen",
    "title_screen",
)


MIN_CLASSIFICATION_MARGIN = 2.0  # see wait_for_nearest_screen's docstring


def wait_for_nearest_screen(client: DebugClient, shots_dir: Path, label: str,
                             accept: str, start: int, stride: int,
                             max_extra: int, stall: int,
                             candidates: tuple[str, ...] = LATE_CHAIN_CANDIDATES,
                             region: str = "full", debounce: bool = True,
                             min_margin: float = MIN_CLASSIFICATION_MARGIN,
                             ) -> tuple[Any, int]:
    """Poll vblank9 forward, classifying each frame as whichever of
    `candidates` it is closest to (all compared in the same `region`), and
    return once the nearest match is `accept` -- confirmed on two
    consecutive polls when `debounce` (guards against a single-frame
    transient, e.g. the connection-test screen's brief blank frame between
    clearing "Testing connection..." and rendering its result, this task's
    other empirically-caught instance of the same lesson) -- AND only once
    the nearest candidate beats the runner-up by at least `min_margin`.

    The margin check matters independently of debounce: nearest-of-N alone
    still accepts on however thin a margin the data happens to produce,
    and this family's measured margins run as low as ~3.26 (wfc_login_
    settled vs. wfc_login_next). Requiring a minimum margin means an
    ambiguous frame (nearest and runner-up close together) is treated as
    "not yet confirmed" and polled past, rather than accepted on a
    coin-flip-thin margin -- the same false-positive shape a sibling
    agent independently hit and fixed in its own screen verifier while
    this file was already mid-fix for it; the margin gate is the one
    piece their fix added that debounce alone does not cover, so it is
    folded in here too rather than left as a known gap.

    Raises ScreenTimeoutError with the full diff table on timeout/stall/
    halt (including "the best two candidates were too close to call")."""
    target = start
    img = None
    hit = None
    previous = None
    diffs: dict[str, float] = {}
    while True:
        hit = client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
        if hit.get("terminal") or hit.get("stalled"):
            raise ScreenTimeoutError(
                f"{label}: execution stalled/halted waiting for {accept!r} "
                f"at vblank9 target={target}: {hit}"
            )
        img = capture_rgb(client)
        diffs = {name: mean_abs_diff(img, load_ref(name), region)
                 for name in candidates}
        ordered = sorted(diffs, key=diffs.get)
        nearest = ordered[0]
        runner_up = ordered[1] if len(ordered) > 1 else None
        margin = (diffs[runner_up] - diffs[nearest]) if runner_up else float("inf")
        confident = margin >= min_margin
        confirmed = (nearest == previous) if debounce else True
        previous = nearest if confident else None
        if nearest == accept and confident and confirmed:
            return hit, target
        if target - start >= max_extra:
            break
        target += stride
    best = min(diffs, key=diffs.get)
    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / f"{label}_TIMEOUT_wanted_{accept}.png"
    img.save(fail_path)
    raise ScreenTimeoutError(
        f"{label}: timed out after vblank9 {start}..{target} (budget "
        f"{max_extra}) waiting for screen {accept!r}; closest known screen "
        f"actually on-screen is {best!r} (diff={diffs[best]:.1f}); all "
        f"candidate diffs={diffs}; diagnostic screenshot: {fail_path}"
    )


class ConnectionTestFailed(RuntimeError):
    """Raised when the WFC Settings connection test reaches an
    "Error Code: ..." screen. Distinguished from ScreenTimeoutError so a
    caller can tell an observed in-game connection-test failure apart from a
    polling timeout or navigation mismatch."""
    pass


def wait_for_connection_test_result(client: DebugClient, shots_dir: Path,
                                     label: str, start: int, stride: int,
                                     max_extra: int, stall: int
                                     ) -> tuple[Any, int]:
    """Poll the connection-test screen to its conclusion.

    This step needed its own function, not a plain wait_for_screen() call,
    because of a bug this exact fix caught empirically on a live run against
    the local WFC server: "Testing connection..." (still running) and
    "Connection successful." (settled) are visually near-identical outside
    the narrow status-message band (mean abs diff ~3.4 even in that band,
    far under any single fixed threshold that would also need to reject
    "still running" reliably) -- while "Error Code: ..." (failed) differs
    much more (~17-18). A single expect-vs-threshold check (verify_screen /
    wait_for_screen's model) cannot reliably tell "still running" apart from
    "settled" here; nearest-of-3 classification against all three known
    outcomes can, and does (see wfc_screen_refs' connection_test_* trio and
    the REGION_FOR "test_band" comment for the measured margins).

    Returns (hit, target) on a *successful* settle. Raises
    ConnectionTestFailed (not a timeout) if the test concludes with an
    on-screen "Error Code". A previous session's own historical capture,
    wiimmfi-i9-kaeru/native_16_connection_test_settled.png, turns out to
    show this exact error screen despite its filename -- another instance
    of the mislabeling defect this task exists to fix.

    Debounced: a "settled"/"error" classification is only trusted once seen
    on two consecutive polls. Caught empirically: the message box passes
    through a brief, genuinely ambiguous BLANK frame between clearing
    "Testing connection..." and rendering the final result line, which a
    single-sample nearest-of-3 classification can misfile as "settled"
    (empty space happening to sit closer to that reference than to the
    other two in this crop) -- one more instance of exactly the class of
    bug this task exists to fix, this time inside the fix's own new
    polling code, caught by actually running it rather than trusting it.

    Also margin-gated (MIN_CLASSIFICATION_MARGIN, same rationale as
    wait_for_nearest_screen): the nearest outcome must beat the runner-up
    by that much, not just be numerically smallest, before it counts
    toward debounce confirmation at all.
    """
    target = start
    img = None
    hit = None
    outcomes = ("connection_test_running", "connection_test_settled",
                "connection_test_error")
    previous = None
    while True:
        hit = client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
        if hit.get("terminal") or hit.get("stalled"):
            raise ScreenTimeoutError(
                f"{label}: execution stalled/halted waiting for the "
                f"connection test to conclude at vblank9 target={target}: {hit}"
            )
        img = capture_rgb(client)
        diffs = diff_report(img, outcomes)
        error_label_diff = connection_test_error_label_diff(img)
        if error_label_diff <= ERROR_CODE_LABEL_THRESHOLD:
            nearest = "connection_test_error"
            margin = float("inf")
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
                        f"screenshot: {fail_path}"
                    )
        confirmed = (nearest == previous) if confident else False
        previous = nearest if confident else None
        if nearest == "connection_test_settled" and confident and confirmed:
            return hit, target
        if nearest == "connection_test_error" and confident and confirmed:
            shots_dir.mkdir(parents=True, exist_ok=True)
            fail_path = shots_dir / f"{label}_CONNECTION_TEST_ERROR.png"
            img.save(fail_path)
            raise ConnectionTestFailed(
                f"{label}: the WFC connection test itself reported an "
                f"on-screen Error Code at vblank9={target} (diffs={diffs}); "
                f"error_label_diff={error_label_diff:.1f}; "
                f"screenshot: {fail_path}"
            )
        # Still running, or an unconfirmed single-sample "settled"/"error"
        # read -- keep polling either way.
        if target - start >= max_extra:
            break
        target += stride
    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / f"{label}_TIMEOUT_still_running.png"
    img.save(fail_path)
    raise ScreenTimeoutError(
        f"{label}: connection test never concluded (still 'running') after "
        f"vblank9 {start}..{target} (budget {max_extra}); diffs={diffs}; "
        f"diagnostic screenshot: {fail_path}"
    )


def converge_to_title(client: DebugClient, shots_dir: Path, label: str,
                       start: int, stall: int, max_presses: int = 6
                       ) -> tuple[Any, int]:
    """Replaces the old fixed 3x-press_b schedule (documented in a
    previous revision of this file as "NOT FULLY AIRTIGHT": 2 of 3 real
    runs converged, the third landed on an unrelated wizard page). Presses
    B, checks the actual screen, and repeats -- title screen is an
    absorbing state so extra presses there are harmless no-ops -- instead
    of trusting a fixed press count to have been enough."""
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
        d = mean_abs_diff(img, load_ref("title_screen"), REGION_FOR["title_screen"])
        if d <= THRESHOLD_FOR.get("title_screen", DEFAULT_THRESHOLD):
            return hit, target
    shots_dir.mkdir(parents=True, exist_ok=True)
    fail_path = shots_dir / f"{label}_TIMEOUT_never_converged_to_title.png"
    img.save(fail_path)
    diffs = diff_report(img, ("title_screen", "wfc_connection_menu"))
    raise ScreenTimeoutError(
        f"{label}: never converged to the title screen after {max_presses} "
        f"B-presses (vblank9 target={target}); diffs={diffs}; diagnostic "
        f"screenshot: {fail_path}"
    )


# Hardware-event counters compared at every checkpoint. Deliberately
# excludes insn9/insn7/cyc9/cyc7 -- see mkds_cart_icon_launch_smoke.py for
# the established precedent (two independently implemented ARM cores do
# not necessarily retire identical host instruction/cycle counts even when
# architecturally faithful).
MATCHED_COUNTERS = (
    "vblank9", "vblank7", "ipcsync_w", "fifo9to7", "fifo7to9",
    "spi_w", "irq9", "irq7", "soundbias_w",
)

GAME_BOOT_IPCSYNC_W = 12520

# ---------------------------------------------------------------------------
# Touch targets, derived from live framebuffer reads (bottom/touch screen,
# 256x192 DS coordinate space). Each was located by dumping the framebuffer
# at the step in question, cropping to the bottom screen, and either
# eyeballing a pixel grid overlay or thresholding button-fill brightness to
# find the interior box -- see the session's derivation notes for the exact
# pixel runs; kept here as the load-bearing summary.

# Firmware menu: DS-card slot banner (established by beads-yjp.3).
CART_ICON = (128, 38)

# MKDS title screen bottom-screen menu list (SINGLE PLAYER / MULTIPLAYER /
# NINTENDO WFC / RECORDS | OPTIONS), each a ~46px-tall full-width band
# starting at y=8. NINTENDO WFC's band is y=104-138; center y=122.
TITLE_NINTENDO_WFC = (128, 122)

# First-run nickname/emblem confirmation dialogs (all three share the same
# button geometry): the dialog box interior, YES at x=25-110 (center 68),
# NO at x=145-230 (center 188), both bands y=138-158 (center y=148).
DIALOG_YES = (68, 148)
DIALOG_NO = (188, 148)

# Nintendo Wi-Fi Connection menu (NINTENDO WFC MATCH / FRIEND CODE /
# NINTENDO WFC SETTINGS), three full-width bands each ~48px tall starting
# y=26; NINTENDO WFC SETTINGS is the third, y=122-144, center y=133.
# x=150 lands inside the button body, clear of the left-hand icon glyph.
WFC_SETTINGS_MENU_ITEM = (150, 133)

# "Nintendo Wi-Fi Connection Setup" step-1 screen: two tiles, "Nintendo
# Wi-Fi Connection Settings" (blue, left, ~2/3 width) and "Options"
# (orange, right). Blue tile interior center.
WFC_CONNECTION_SETTINGS_TILE = (85, 100)

# Connection-slot picker (Connection 1/2/3 + Erase Settings, three equal
# columns across the full 256px width -> each ~85px wide). Connection 1's
# "None" label sits near the top of its column.
CONNECTION_SLOT_1 = (43, 35)

# Connection 1 Settings, step 2: "Search for an Access Point" full-width
# bar near the top of the bottom screen.
SEARCH_FOR_AP = (128, 37)

# Discovered-AP list, first (only) row: the "ndsrecomp" SSID text starts
# near the list's left edge; the row itself spans the full list width.
AP_LIST_FIRST_ROW = (50, 65)

# ---------------------------------------------------------------------------
# Wiimmfi M5 (beads-yjp.1.6): past the connection test, into the real NAS
# login attempt. Coordinates below were derived the same way as everything
# above -- read a live `framebuffer` at the step in question, threshold for
# the button box, take the interior center -- with one added wrinkle this
# milestone surfaced for the first time: the connection-test-settled screen
# was reached via one full round trip to a REAL backend (Kaeru or the local
# test server), and that round trip's actual latency is real-host-network-
# timing-dependent (see docs/adr-melonds-wifi-vendoring.md's "ACCEPTED, NOT
# FIXED" note on libslirp's CLOCK_MONOTONIC timers) -- unlike every earlier
# step in this file, which only depends on the guest's own deterministic
# UI/input timing. Two full scripted attempts this milestone landed on
# DIFFERENT screens after the identical "dismiss the result, press B twice"
# sequence (once on the Wi-Fi Connection menu, once already overshot to the
# title screen) purely because the earlier network round trip settled a few
# VBlanks earlier or later. Pressing B repeatedly with generous settle
# windows still converges reliably: the title screen is an absorbing state
# (further B presses there are no-ops), and re-tapping NINTENDO WFC from the
# title screen, once a nickname/emblem profile and a tested Connection slot
# already exist this session, skips straight past the first-run dialogs
# (nickname/onscreen-name/emblem confirm) directly to the Wi-Fi Connection
# menu -- verified against real Kaeru. That converge-then-retap shape is
# what CONNECT_TO_WFC_MENU below implements, instead of a fixed B-press
# count.
#
# Wi-Fi Connection menu (see TITLE_NINTENDO_WFC's tap target for the title
# screen's own button), three full-width bands starting y=26, same geometry
# note as WFC_SETTINGS_MENU_ITEM below: NINTENDO WFC MATCH is the FIRST
# band, y=26-74, center y=50 -- the real "connect and log in" action (as
# opposed to NINTENDO WFC SETTINGS, which only edits/tests connection
# slots and never itself performs the NAS handshake).
WFC_MATCH = (150, 50)

# Single centered full-width button used by three different one-button
# informational dialogs encountered after tapping WFC_MATCH with an
# already-tested connection: the "Nintendo WFC treats your ... Game Card
# and Nintendo DS as a set" disclaimer (button text "NEXT"), the "Wi-Fi ID
# ... has been saved to the Game Card" notice (also "NEXT"), and the
# "From now on, please use this Nintendo DS ..." notice ("OK"). All three
# render the identical button geometry (interior y=140-159 in the bottom-
# screen's own 0-192 coordinate space, center y=150 -- NOT y=178, which is
# background, a mistake made and corrected while deriving this) regardless
# of the button's label, so one constant covers all three steps.
ONE_BUTTON_DIALOG = (128, 150)

# "Save this Nintendo DS system's Nintendo WFC configuration to this Game
# Card and connect to Nintendo WFC?" YES/NO confirmation -- same band
# geometry as DIALOG_YES/DIALOG_NO above (y=140-159, center 150; x centers
# empirically found at 68/196, matching DIALOG_YES/DIALOG_NO's 68/188
# closely enough that either pair lands inside the respective button).
WFC_MATCH_CONFIRM_YES = (68, 150)

# DS button state is active-low (0=pressed) in the `keys` mask, default
# 0x3FF = nothing pressed. Bit 0 = A, bit 1 = B.
A_PRESSED = 0x3FF & ~0x1
A_RELEASED = 0x3FF
B_PRESSED = 0x3FF & ~0x2
B_RELEASED = 0x3FF


def tap(client: DebugClient, xy: tuple[int, int], vblank_down: int,
        vblank_up: int, stall: int = 300_000) -> dict[str, Any]:
    """Press at xy, hold until vblank_down, release, run to vblank_up."""
    x, y = xy
    client.cmd("touch", x=x, y=y, down=True)
    client.cmd("run_to_event", event="vblank9", count=vblank_down, stall=stall)
    client.cmd("touch", x=x, y=y, down=False)
    return client.cmd("run_to_event", event="vblank9", count=vblank_up,
                       stall=stall)


def press_b(client: DebugClient, vblank_down: int, vblank_up: int,
            stall: int = 300_000) -> dict[str, Any]:
    client.cmd("keys", mask=B_PRESSED)
    client.cmd("run_to_event", event="vblank9", count=vblank_down, stall=stall)
    client.cmd("keys", mask=B_RELEASED)
    return client.cmd("run_to_event", event="vblank9", count=vblank_up,
                       stall=stall)


def press_a(client: DebugClient, vblank_down: int, vblank_up: int,
            stall: int = 300_000) -> dict[str, Any]:
    client.cmd("keys", mask=A_PRESSED)
    client.cmd("run_to_event", event="vblank9", count=vblank_down, stall=stall)
    client.cmd("keys", mask=A_RELEASED)
    return client.cmd("run_to_event", event="vblank9", count=vblank_up,
                       stall=stall)


def shoot(client: DebugClient, path: Path) -> None:
    if Image is None:
        return
    w, h, rgb_a = client.framebuffer("A")
    wb, hb, rgb_b = client.framebuffer("B")
    img = Image.new("RGB", (max(w, wb), h + hb))
    img.paste(Image.frombytes("RGB", (w, h), rgb_a), (0, 0))
    img.paste(Image.frombytes("RGB", (wb, hb), rgb_b), (0, h))
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)


def run_scenario(port: int, shots_dir: Path, label: str,
                  stall: int = 300_000) -> list[tuple[str, dict[str, Any]]]:
    """Drive one machine through the whole scenario; return a list of
    (step_name, event_counts) checkpoints taken along the way."""
    client = DebugClient(port=port, timeout=900.0)
    checkpoints: list[tuple[str, dict[str, Any]]] = []

    def checkpoint(name: str) -> None:
        checkpoints.append((name, client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_{name}.png")

    try:
        client.cmd("reset")
        client.cmd("run_to_event", event="vblank9", count=120, stall=stall)
        client.cmd("touch", x=CART_ICON[0], y=CART_ICON[1], down=True)
        client.cmd("run_to_event", event="vblank9", count=130, stall=stall)
        client.cmd("touch", x=CART_ICON[0], y=CART_ICON[1], down=False)

        # Hardware-event rendezvous, not a raw frame number (beads-yjp.7).
        hit = client.cmd("run_to_event", event="ipcsync_w",
                          count=GAME_BOOT_IPCSYNC_W, stall=stall)
        if not hit.get("reached") or hit.get("terminal"):
            raise RuntimeError(f"{label}: cart launch failed to reach the "
                                f"game-boot IPC handshake: {hit}")
        checkpoint("cart_boot_handshake")

        # ---- Steps through the connection test: measured guest-
        # deterministic (intra-class pixel diff == 0.0 across 4-9
        # independent real runs spanning separate sessions -- see the
        # module-level comment). Fixed VBlank offsets are kept (polling
        # would burn framebuffer round trips for zero benefit here), but
        # every checkpoint with a saved reference is now verified
        # immediately and fails loudly -- naming the expected vs. actual
        # screen -- rather than trusting the offset blindly.
        client.cmd("run_to_event", event="vblank9", count=900, stall=stall)
        checkpoint("title_screen")
        verify_screen(client, shots_dir, f"{label}_title_screen",
                      "title_screen")

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

        # The scan itself takes many hundreds of VBlanks (channel sweep +
        # beacon/probe-response cadence) -- run far enough forward to reach
        # the discovered-AP list rather than judging "still searching" as a
        # terminal state (same lesson as the cart-icon tap: don't conclude
        # from an early sample). This is our own virtual AP's simulated
        # beacon/probe timing (guest-side, not a real host network round
        # trip), which is why it stayed in the fixed-offset section above
        # rather than the polled section below.
        client.cmd("run_to_event", event="vblank9", count=3500, stall=stall)
        checkpoint("ap_list_found")

        tap(client, AP_LIST_FIRST_ROW, 3510, 3650, stall=stall)
        checkpoint("ap_selected_connection_test_prompt")

        press_a(client, 3660, 3800, stall=stall)
        checkpoint("connection_test_running")

        # ---- First real-network-timing-dependent step: the connection
        # test is a genuine plain-HTTP round trip (conntest.nintendowifi.net
        # or the local server), so poll for the settled screen instead of
        # assuming a fixed offset always covers it. This also correctly
        # surfaces a real backend failure (ConnectionTestFailed) instead of
        # a driver bug -- see wait_for_connection_test_result's docstring.
        hit, target = wait_for_connection_test_result(
            client, shots_dir, f"{label}_connection_test_settled",
            start=3800, stride=100, max_extra=4000, stall=stall,
        )
        checkpoints.append(("connection_test_settled", client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_connection_test_settled.png")

        # ---- Wiimmfi M5: past the test, into the real NAS login attempt.
        # Replaces the old fixed 3x-press_b + fixed-offset-retap schedule,
        # documented in a previous revision of this file as "NOT FULLY
        # AIRTIGHT" (2 of 3 real runs converged correctly; the third landed
        # on an unrelated wizard page). Every step from here on either
        # polls for its expected screen or verifies it immediately.
        press_a(client, target + 10, target + 150, stall=stall)  # dismiss
                                                                    # "Connection
                                                                    # successful."
        hit, target = converge_to_title(
            client, shots_dir, f"{label}_converge_to_title",
            start=target + 150, stall=stall,
        )
        checkpoints.append(("back_at_title_with_saved_connection",
                             client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_back_at_title_with_saved_connection.png")

        tap(client, TITLE_NINTENDO_WFC, target + 10, target + 160, stall=stall)
        # Retapping NINTENDO WFC from the title screen, once a nickname/
        # emblem profile and a tested connection slot already exist this
        # session, should skip straight past the first-run dialogs to the
        # Wi-Fi Connection menu. Poll for that screen, but hand the
        # nickname dialog to the diagnostics too: if it reappears instead
        # (meaning this session's profile/slot state was NOT what we
        # assumed), the failure message will say so explicitly rather than
        # tapping WFC_MATCH's coordinates blind onto an unrelated dialog.
        hit, target = wait_for_screen(
            client, shots_dir, f"{label}_wfc_connection_menu_direct",
            "wfc_connection_menu", start=target + 160, stride=50,
            max_extra=800, stall=stall,
            diag_candidates=("nickname_confirm_dialog", "title_screen"),
        )
        checkpoints.append(("wfc_connection_menu_direct", client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_wfc_connection_menu_direct.png")

        tap(client, WFC_MATCH, target + 10, target + 140, stall=stall)
        hit, target = wait_for_nearest_screen(
            client, shots_dir, f"{label}_wfc_match_disclaimer",
            "wfc_match_disclaimer", start=target + 140, stride=50,
            max_extra=800, stall=stall,
        )  # "Nintendo WFC treats your ... Game Card and Nintendo DS as a
           # set." / NEXT
        checkpoints.append(("wfc_match_disclaimer", client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_wfc_match_disclaimer.png")

        tap(client, ONE_BUTTON_DIALOG, target + 10, target + 140, stall=stall)
        hit, target = wait_for_nearest_screen(
            client, shots_dir, f"{label}_wfc_match_save_confirm",
            "wfc_match_save_confirm", start=target + 140, stride=50,
            max_extra=800, stall=stall,
        )  # "Save this ... configuration ... and connect to Nintendo WFC?"
           # / YES,NO
        checkpoints.append(("wfc_match_save_confirm", client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_wfc_match_save_confirm.png")

        tap(client, WFC_MATCH_CONFIRM_YES, target + 10, target + 140, stall=stall)
        hit, target = wait_for_nearest_screen(
            client, shots_dir, f"{label}_wfc_connecting",
            "wfc_connecting", start=target + 140, stride=50, max_extra=800,
            stall=stall,
        )  # "Connecting to Nintendo WFC... Please wait a moment..." -- the
           # real DNS/TCP/TLS/HTTP NAS handshake happens somewhere in the
           # poll below.
        checkpoints.append(("wfc_connecting", client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_wfc_connecting.png")

        # ---- THE fix this task exists for. The real handshake (DNS ->
        # TCP SYN -> SSLv3 ClientHello/ServerHello/ClientKeyExchange/
        # ChangeCipherSpec x2/ApplicationData x2 -> clean FIN) is real-
        # host-network-timing-dependent; historical measurement against
        # real Kaeru put it around ~600 VBlanks (~10s of guest time) end to
        # end, but that is an observation, not a guarantee. Poll in small
        # strides up to a generous budget (20000 VBlanks =~ 33x the
        # observed typical case) instead of asserting a fixed offset always
        # covers it; on timeout, report the closest known screen actually
        # on-screen (still "connecting"? reverted to title? something
        # else?) rather than silently mislabeling whatever got captured --
        # the exact defect this task's frames 22-24 in wiimmfi-m5-final
        # demonstrated. Nearest-of-N (not a single threshold) -- see
        # LATE_CHAIN_CANDIDATES' comment: a single-reference threshold here
        # was caught live accepting a still-"connecting" frame as this
        # screen (and, one step later, as "wfc_login_next" too).
        hit, target = wait_for_nearest_screen(
            client, shots_dir, f"{label}_wfc_login_settled",
            "wfc_login_settled", start=target, stride=200, max_extra=20000,
            stall=stall,
        )  # "The Wi-Fi ID from this Nintendo DS has been saved to the Game
           # Card." / NEXT on success.
        checkpoints.append(("wfc_login_settled", client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_wfc_login_settled.png")

        tap(client, ONE_BUTTON_DIALOG, target + 10, target + 140, stall=stall)
        hit, target = wait_for_nearest_screen(
            client, shots_dir, f"{label}_wfc_login_next",
            "wfc_login_next", start=target + 140, stride=50, max_extra=1500,
            stall=stall,
        )  # "From now on, please use this Nintendo DS ..." / OK
        checkpoints.append(("wfc_login_next", client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_wfc_login_next.png")

        tap(client, ONE_BUTTON_DIALOG, target + 10, target + 140, stall=stall)
        hit, target = wait_for_nearest_screen(
            client, shots_dir, f"{label}_wfc_match_setup_screen",
            "wfc_match_setup_screen", start=target + 140, stride=50,
            max_extra=1500, stall=stall,
        )  # "Choose the conditions for this match." -- reaching this
           # screen is itself proof of a completed, authenticated login:
           # MKDS only offers match setup after NAS + GPCM both succeeded.
        checkpoints.append(("wfc_match_setup_screen", client.cmd("event_counts")))
        shoot(client, shots_dir / f"{label}_{len(checkpoints):02d}_wfc_match_setup_screen.png")
    finally:
        client.close()

    return checkpoints


# ---------------------------------------------------------------------------
# Network-evidence dump (Wiimmfi M3/M4, beads I9 tasks 1-4): reconnects to an
# already-run native server (the ring is server-side state, independent of
# any particular debug-protocol connection, so this can run after
# run_scenario's own client has closed) and reports what the classifier
# (runner/src/net/net_classify.cpp) actually saw -- DHCP/DNS/ARP/TCP/UDP --
# plus the Tier-3 interpreter coverage context the task asks us to check
# before blaming the network for a stalled handshake (a functionally
# correct guest that misses its own retry window looks identical to a
# broken network from the outside).
NET_KINDS = (
    "arp", "dhcp", "dns_query", "dns_response",
    "tcp_open", "tcp_close", "tcp_reset", "tcp_packet", "udp_packet",
    "backend_drop", "backend_error",
    # Wiimmfi M5: passive TLS record classification (net_classify.cpp's
    # classify_tls_record) -- ClientHello/ServerHello/ClientKeyExchange/
    # ChangeCipherSpec/ApplicationData labels, by record header bytes only.
    # `aux` packs (content_type << 16) | (handshake_type << 8) |
    # version_minor; `wifi_value`/`wifi_reg` are unused (0) for this kind.
    "tls_record",
)


def dump_network_evidence(port: int) -> dict[str, Any]:
    client = DebugClient(port=port, timeout=60.0)
    try:
        report: dict[str, Any] = {
            "event_counts": client.cmd("event_counts"),
            "static_coverage": client.cmd("static_coverage"),
            "net_state": client.cmd("net_state"),
            "kinds": {},
        }
        for kind in NET_KINDS:
            dump = client.cmd("net_ring_dump", max=4096, filter=kind)
            report["kinds"][kind] = dump["events"]
        return report
    finally:
        client.close()


def print_network_evidence(report: dict[str, Any]) -> None:
    print("\n--- network ring evidence ---")
    print("event_counts:", json_dumps(report["event_counts"]))
    print("static_coverage (Tier-3):", json_dumps(report["static_coverage"]))
    print("net_state:", json_dumps(report["net_state"]))
    for kind, events in report["kinds"].items():
        if not events:
            continue
        print(f"\n[{kind}] {len(events)} event(s):")
        for e in events:
            src_ip = ipv4_str(e["src_ipv4"])
            dst_ip = ipv4_str(e["dst_ipv4"])
            line = (
                f"  #{e['count']} sys={e['sys']} dir={e['direction']} "
                f"{src_ip}:{e['src_port']} -> {dst_ip}:{e['dst_port']} "
                f"len={e['payload_len']} aux=0x{e['aux']:X} "
                f"wifi_value=0x{e['wifi_value']:X}"
            )
            if e.get("hostname"):
                line += f" host={e['hostname']}"
            print(line)


def ipv4_str(value: int) -> str:
    return ".".join(str((value >> shift) & 0xFF) for shift in (24, 16, 8, 0))


def json_dumps(value: Any) -> str:
    import json
    return json.dumps(value, sort_keys=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-port", type=int, default=19842)
    parser.add_argument("--oracle-port", type=int, default=19843)
    parser.add_argument("--shots-dir", type=Path, default=DEFAULT_SHOTS_DIR)
    parser.add_argument("--stall", type=int, default=300_000)
    # The oracle diverges into a "Wi-Fi ID may have been erased" boot-time
    # stall well before this scenario's networking steps even begin
    # (beads-yjp: a pre-existing MKDS game-boot divergence upstream of all
    # networking, not owned by this task). --native-only runs just the
    # native side and reports ring evidence instead of raising on the
    # oracle's already-known divergence.
    parser.add_argument("--native-only", action="store_true",
                        help="skip the oracle entirely; run native only and "
                             "dump network-ring evidence at the end")
    parser.add_argument("--native-label", default=None,
                        help="screenshot/checkpoint label for --native-only; "
                             "defaults to native_p<port> to keep concurrent "
                             "diagnostics distinct")
    args = parser.parse_args()

    if args.native_only:
        native_label = args.native_label or f"native_p{args.native_port}"
        native = run_scenario(args.native_port, args.shots_dir, native_label,
                              args.stall)
        print(f"{'step':<36} result")
        for name, counts in native:
            print(f"{name:<36} vblank9={counts.get('vblank9')} "
                  f"vblank7={counts.get('vblank7')}")
        report = dump_network_evidence(args.native_port)
        print_network_evidence(report)
        return 0

    with ThreadPoolExecutor(max_workers=2) as pool:
        native_future = pool.submit(
            run_scenario, args.native_port, args.shots_dir, "native",
            args.stall,
        )
        oracle_future = pool.submit(
            run_scenario, args.oracle_port, args.shots_dir, "oracle",
            args.stall,
        )
        native = native_future.result()
        oracle = oracle_future.result()

    if len(native) != len(oracle):
        raise SystemExit(
            f"native took {len(native)} checkpoints, oracle took "
            f"{len(oracle)} -- one side failed partway through the script"
        )

    print(f"{'step':<36} {'result':<10} detail")
    first_divergence = None
    for (name_n, counts_n), (name_o, counts_o) in zip(native, oracle):
        if name_n != name_o:
            raise SystemExit(f"checkpoint name mismatch: {name_n!r} vs "
                              f"{name_o!r} -- scenario scripts drifted")
        diffs = {
            key: (counts_n[key], counts_o[key])
            for key in MATCHED_COUNTERS
            if counts_n[key] != counts_o[key]
        }
        status = "OK" if not diffs else "DIVERGE"
        detail = "" if not diffs else ", ".join(
            f"{k}={v[0]}/{v[1]}" for k, v in diffs.items()
        )
        print(f"{name_n:<36} {status:<10} {detail}")
        if diffs and first_divergence is None:
            first_divergence = (name_n, diffs)

    if first_divergence is not None:
        name, diffs = first_divergence
        raise SystemExit(
            f"FIRST DIVERGENCE at checkpoint {name!r}: "
            + ", ".join(f"{k}=native:{v[0]}/oracle:{v[1]}"
                         for k, v in diffs.items())
        )

    print("\nAll checkpoints match exactly between native and oracle -- "
          "no divergence found through the full driven WFC-setup scenario.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
