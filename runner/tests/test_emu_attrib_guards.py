#!/usr/bin/env python3
"""Structural regression checks for the emu-time attribution partition
(beads-yjp.54, runner/src/emu_profile.h).

WHY A STRUCTURAL TEST
---------------------
The value of this module is a PARTITION: the buckets must be exhaustive over
the emu phase and must not overlap. Neither property is a number anyone can
assert -- they are properties of WHERE the regions are placed. The failure mode
is silent: delete the region from one device tick and the runner still builds,
still runs, still writes a perf record, and the missing cost simply reappears
inside sched_other or inside the enclosing bucket. Nothing goes red. Six months
later someone re-derives "16-30 ms per dip frame is unaccounted", which is the
exact hole this module was built to close.

So each property below is one named assertion:

  * every consumer of emu time that the survey identified has a region, and it
    is at the FUNCTION DEFINITION rather than at a call site, so a new caller
    cannot bypass it;
  * all six guest bus slow paths are wrapped, none partially converted;
  * the scheduler round opens the round region before it does anything;
  * every declared bucket is actually used, and every bucket the header marks
    exact is one the design says is exact;
  * the reporting surfaces exist -- the perf record's emu_attrib block and the
    debug-server command -- because a counter nobody can read is not
    observability.

Run standalone or through ctest (`emu_attrib_guards_test`).
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "runner/src"

EMU_H = SRC / "emu_profile.h"
EMU_CPP = SRC / "emu_profile.cpp"

failures = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name}: {detail}")
        failures.append(name)


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def body(source, name, path):
    """Return the brace-matched body of function `name`."""
    match = re.search(
        r"\b(?:extern\s+\"C\"\s+)?(?:static\s+)?(?:inline\s+)?"
        r"(?:void|bool|int|int32_t|uint8_t|uint16_t|uint32_t|uint64_t|"
        r"unsigned\s+long\s+long)"
        r"\s*\*?\s*" + re.escape(name) + r"\s*\([^;{]*?\)\s*(?:noexcept\s*)?\{",
        source, re.S)
    if not match:
        raise AssertionError(f"missing function {name} in {path}")
    start, depth, i = match.end(), 1, match.end()
    while i < len(source) and depth:
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
        i += 1
    return source[start:i - 1]


# ── 1. Every identified consumer has a region at its definition ─────────────
#
# (file, function, expected bucket token). The bucket token is checked so a
# refactor cannot silently retarget a region at the wrong bucket -- which would
# keep the sum right and make the attribution wrong, the harder bug to see.
SITES = [
    ("scheduler.cpp", "run_slice", "NDS_EMU_EXEC_ARM9"),
    ("scheduler.cpp", "switch_to", "NDS_EMU_CTXSW"),
    ("io.cpp", "nds_dma_run", "NDS_EMU_DMA_ARM9"),
    ("io.cpp", "nds_tick_display", "NDS_EMU_DISPLAY"),
    ("io.cpp", "nds_tick_timers", "NDS_EMU_TIMERS_ARM9"),
    ("io.cpp", "nds_tick_rtc", "NDS_EMU_RTC"),
    ("io.cpp", "nds_run_system_events", "NDS_EMU_SYSEV"),
    ("spu.cpp", "nds_tick_spu", "NDS_EMU_SPU"),
    ("wifi_net.cpp", "nds_wifi_run_events", "NDS_EMU_WIFI"),
    ("gpu3d.cpp", "nds_gpu3d_run", "NDS_EMU_GEOM"),
    ("gpu3d.cpp", "nds_gpu3d_start_frame", "NDS_EMU_GPU3D_FRAME"),
    ("gpu3d.cpp", "nds_gpu3d_vcount215", "NDS_EMU_GPU3D_FRAME"),
    ("gpu3d.cpp", "nds_gpu3d_vcount144", "NDS_EMU_GPU3D_FRAME"),
    ("gpu2d.cpp", "nds_gpu2d_render_scanline", "NDS_EMU_RASTER2D"),
    ("tier3.cpp", "tier3_run", "NDS_EMU_TIER3_ARM9"),
    # The poll BODY, which live_overlay_poll() now reaches through a countdown
    # gate. The region must sit here, at the work, and not at the gate: timing
    # a gated-out call would put two rdtsc reads back on every scheduler round
    # and make the bucket report instrumentation instead of work.
    ("live_overlay.cpp", "live_overlay_poll_now", "NDS_EMU_OVERLAY"),
]

print("emu-attribution region placement")
for filename, func, bucket in SITES:
    path = SRC / filename
    try:
        text = body(read(path), func, path)
    except AssertionError as exc:
        check(f"{filename}:{func} region", False, str(exc))
        continue
    has_scope = "NdsEmuScope" in text
    check(f"{filename}:{func} opens a region", has_scope,
          "no NdsEmuScope/NdsEmuScopeIf in the function body")
    check(f"{filename}:{func} targets {bucket}", bucket in text,
          f"body does not mention {bucket}")

# NEXTEV is a region around one expression rather than a whole function.
sched = read(SRC / "scheduler.cpp")
check("scheduler.cpp times next_scheduled_event_time",
      re.search(r"NdsEmuScope\s+\w+\(NDS_EMU_NEXTEV\);\s*\n\s*return\s+"
                r"next_scheduled_event_time\(\);", sched) is not None,
      "the NEXTEV region no longer wraps next_scheduled_event_time()")

# The scheduler's per-round hook must stay a GATE: no region, no work, just
# the enabled early-out and a countdown to the real body. Putting either the
# region or the body back here is a 0.2-0.4 ms/frame regression (measured,
# NDS_EMU_OVERLAY bucket, MPH Kanden) that nothing else in the suite would go
# red for.
overlay = read(SRC / "live_overlay.cpp")
poll_gate = body(overlay, "live_overlay_poll", SRC / "live_overlay.cpp")
check("live_overlay_poll stays a cheap gate",
      "NdsEmuScope" not in poll_gate and "live_overlay_poll_now()" in poll_gate,
      "the per-round hook must open no region and must delegate the body")
check("live_overlay_poll keeps the enabled early-out first",
      re.match(r"\s*if\s*\(!g_live\.enabled\)\s*return;", poll_gate)
      is not None,
      "a disabled overlay must not even reach the countdown")
check("live_overlay_poll counts down before delegating",
      re.search(r"static\s+uint\d+_t\s+\w+\s*=\s*0;", poll_gate) is not None
      and re.search(r"\w+\+\+\s*%\s*\w+\)\s*!=\s*0u\)\s*return;", poll_gate)
      is not None,
      "the delegation must stay behind a counter -- an unconditional call to "
      "live_overlay_poll_now() is the pre-gate cost with extra steps")

# ── 2. The round region is opened, and opened first ─────────────────────────
round_body = body(sched, "scheduler_run_round", SRC / "scheduler.cpp")
statements = [l.strip() for l in round_body.splitlines()
              if l.strip() and not l.strip().startswith("//")]
# Everything before NdsEmuRound runs UNATTRIBUTED, so the allowlist is the
# assertion: only the activity guard and the pc-profile note block (whose cost
# must NOT land in SCHED_OTHER -- the 0.29 ms/f artifact the Kanden A/B
# measured when it lived inside the region) may precede it. Any other
# statement sneaking above the round region is scheduler work escaping the
# partition.
PRE_ROUND_ALLOWED = ("RoundActivityGuard", "{", "}", "static uint64_t pc_note_",
                     "if ((pc_note_", "nds_pc_profile_note(", ": g_slot[")
pre_round = []
for stmt in statements:
    if stmt.startswith("NdsEmuRound"):
        break
    pre_round.append(stmt)
else:
    pre_round = None  # NdsEmuRound never constructed at all
check("scheduler_run_round constructs NdsEmuRound",
      pre_round is not None, "NdsEmuRound not found in round body")
stray = [s for s in (pre_round or [])
         if not any(s.startswith(p) for p in PRE_ROUND_ALLOWED)]
check("only the activity guard and the pc-profile note precede NdsEmuRound",
      pre_round is not None and not stray,
      f"unattributed statements before the round region: {stray!r}")
check("the pc-profile note is sampled OUTSIDE the emu round region",
      pre_round is not None and
      any("nds_pc_profile_note(" in s for s in pre_round) and
      "nds_pc_profile_note" not in round_body.split("NdsEmuRound", 1)[1],
      "pc note missing before the region, or a second one inside it")

# ── 2b. The EXEC population covers every dispatch entry ─────────────────────
#
# The park population above is a halt-share measurement (MPH Kanden: ARM9 42
# percent in the CP15 idle loop, ARM7 99 percent in the BIOS halt loop) and
# cannot rank hot compute functions. The exec population is what ranks them,
# and its ONE claim is coverage: every way guest code is entered passes the
# dispatch_total increment at the top of runtime_dispatch_impl's loop. That
# claim is a property of WHERE the note sits, exactly like the region
# placements above -- a new increment site added without the note, or the note
# moved to one caller, silently narrows the population to whichever paths
# happen to still reach it, and every number keeps looking plausible.
DISPATCH_TOTAL = re.compile(
    r"\+\+\s*g_nds_dispatch_stats\[[^\]]+\]\.dispatch_total\s*;")
RESUME_DISPATCH = re.compile(
    r"\+\+\s*g_nds_dispatch_stats\[[^\]]+\]\.resume_dispatch\s*;")
EXEC_NOTE = "nds_pc_profile_note_exec("

total_sites = []
resume_sites = []
for path in sorted(SRC.rglob("*.cpp")):
    lines = read(path).splitlines()
    for index, text in enumerate(lines):
        if DISPATCH_TOTAL.search(text):
            total_sites.append((path, index, lines))
        if RESUME_DISPATCH.search(text):
            resume_sites.append((path, index, lines))

check("at least one dispatch_total site exists", bool(total_sites),
      "the dispatch funnel moved; this whole section is now checking nothing")
for path, index, lines in total_sites:
    window = "\n".join(lines[index:index + 30])
    check(f"{path.name}:{index + 1} dispatch_total site notes the exec PC",
          EXEC_NOTE in window,
          "an entry path that no exec sample can ever see")

# The scheduler's resume counter is the OTHER dispatch-increment site, and it
# must stay unhooked: a resume calls straight into the dispatch loop, so a
# second note here would count those entries twice and re-import the park
# population's slice-boundary bias into the population built to escape it.
check("at least one resume_dispatch site exists", bool(resume_sites))
for path, index, lines in resume_sites:
    window = "\n".join(lines[index:index + 10])
    check(f"{path.name}:{index + 1} resume funnels into runtime_dispatch",
          "runtime_dispatch(" in window,
          "a resume that does NOT reach the hooked loop would be unsampled")
    check(f"{path.name}:{index + 1} resume does not double-note",
          EXEC_NOTE not in window,
          "resumed entries would be counted twice")

pc_h = read(SRC / "pc_profile.h")
pc_cpp = read(SRC / "pc_profile.cpp")
# Neither population may read a clock or open a region: park is sampled outside
# the emu round on purpose (above), and exec sits on a ~4M/s path inside
# NDS_EMU_EXEC_*, where two tick reads per sample would measure the
# instrumentation instead of the guest.
for name, text in (("pc_profile.h", pc_h), ("pc_profile.cpp", pc_cpp)):
    check(f"{name} reads no clock and opens no region",
          not any(token in text for token in
                  ("tick_now", "NdsEmuScope", "steady_clock", "QueryPerf")),
          "the PC histogram must perturb nothing it measures")

# Primality of the exec gate is the anti-phase-lock property, and it is the
# kind of thing a later "round it to 128 for a cheap mask" edit destroys while
# every number still looks reasonable: a guest loop of 2^k dispatches would
# then be sampled at the SAME entry forever, reporting one PC at N times its
# share and its neighbours at zero.
gate = re.search(r"constexpr uint64_t kNdsPcExecGate\s*=\s*(\d+)u?;", pc_h)
check("kNdsPcExecGate is declared", gate is not None)
if gate:
    n = int(gate.group(1))
    prime = n >= 2 and all(n % d for d in range(2, int(n ** 0.5) + 1))
    check("kNdsPcExecGate is prime", prime,
          f"{n} is composite -- it can phase-lock to guest loop periodicity")
    check("kNdsPcExecGate resolves a top-8 within one interval",
          32 <= n <= 1009,
          f"{n} samples too rarely (or too often) for a 2 s interval")

diag_src = read(SRC / "diagnostics.cpp")
dbg_src = read(SRC / "debug_server.cpp")
check("both populations are reported per interval",
      '\\"pc_hot_delta\\":' in diag_src and '\\"pc_exec_delta\\":' in diag_src,
      "a perf record carrying only one population is how the idle loop got "
      "reported as the hottest code in the game")
check("the debug server can select a population",
      'cmd == "pc_hot"' in dbg_src and
      "nds_pc_profile_kind_from_name" in dbg_src,
      "pc_hot must expose kind=park|exec")

# ── 3. All six bus slow paths wrapped -- no partial conversion ──────────────
bus = read(SRC / "bus.cpp")
SLOW = ["bus_read_u32_slow", "bus_read_u16_slow", "bus_read_u8_slow",
        "bus_write_u32_slow", "bus_write_u16_slow", "bus_write_u8_slow"]
for func in SLOW:
    text = body(bus, func, SRC / "bus.cpp")
    check(f"bus.cpp:{func} wrapped", text.count("NdsEmuBusRegion") == 1,
          f"{text.count('NdsEmuBusRegion')} NdsEmuBusRegion declarations "
          f"(expected exactly 1)")

# ── 4. Bucket table integrity ──────────────────────────────────────────────
header = read(EMU_H)
impl = read(EMU_CPP)

enum_text = re.search(r"enum NdsEmuBucket\s*:\s*uint8_t\s*\{(.*?)\};",
                      header, re.S)
check("NdsEmuBucket enum found", enum_text is not None)
buckets = []
if enum_text:
    for token in re.findall(r"\b(NDS_EMU_[A-Z0-9_]+)\b", enum_text.group(1)):
        if token in ("NDS_EMU_SAMPLED_COUNT", "NDS_EMU_BUCKET_COUNT",
                     "NDS_EMU_NONE", "NDS_EMU_INACTIVE"):
            continue
        if token not in buckets:
            buckets.append(token)
check("bucket list non-empty", len(buckets) >= 15,
      f"only {len(buckets)} buckets parsed")

names = re.search(r"kBucketNames\[NDS_EMU_BUCKET_COUNT\]\s*=\s*\{(.*?)\};",
                  impl, re.S)
check("kBucketNames table found", names is not None)
if names:
    count = len(re.findall(r'"', names.group(1))) // 2
    check("kBucketNames has one name per bucket", count == len(buckets),
          f"{count} names for {len(buckets)} buckets -- a mismatch shifts "
          f"every bucket's label in the perf record")

# Every declared bucket must be referenced by a region somewhere. A bucket
# nobody opens is a hole in the partition wearing a name.
all_src = "\n".join(read(p) for p in SRC.rglob("*.cpp"))
for bucket in buckets:
    if bucket == "NDS_EMU_SCHED_OTHER":
        continue  # opened by NdsEmuRound, checked above
    check(f"{bucket} is opened somewhere",
          bucket in all_src or bucket in header,
          "declared but never used as a region bucket")

# The exact set is load-bearing: an exact bucket's total must NOT be scaled by
# rounds/sampled_rounds. Mislabelling one is a silent factor-of-31 error.
EXPECTED_EXACT = {
    "NDS_EMU_GEOM", "NDS_EMU_TIER3_ARM9", "NDS_EMU_TIER3_ARM7",
    "NDS_EMU_DMA_ARM9", "NDS_EMU_DMA_ARM7", "NDS_EMU_RASTER2D",
    "NDS_EMU_GPU3D_FRAME",
}
mask = re.search(r"constexpr uint64_t kExactMask\s*=(.*?);", header, re.S)
check("kExactMask found", mask is not None)
if mask:
    got = set(re.findall(r"\b(NDS_EMU_[A-Z0-9_]+)\b", mask.group(1)))
    check("kExactMask names exactly the exact buckets", got == EXPECTED_EXACT,
          f"extra={sorted(got - EXPECTED_EXACT)} "
          f"missing={sorted(EXPECTED_EXACT - got)}")

# ── 5. Reporting surfaces exist ────────────────────────────────────────────
diag = read(SRC / "diagnostics.cpp")
check("perf record emits emu_attrib", '\\"emu_attrib\\":' in diag,
      "the per-interval block is not written to the perf jsonl")
check("perf record emits profile_totals_delta",
      '\\"profile_totals_delta\\":' in diag,
      "profile_totals would again ship only cumulative run totals, which is "
      "the schema defect that made the field logs unreadable")
check("perf record reports the residual",
      '\\"residual_pct\\":' in diag,
      "the unaccounted share must be a number in the log, not a derivation")
dbg = read(SRC / "debug_server.cpp")
check("debug server exposes emu_attrib",
      'cmd == "emu_attrib"' in dbg,
      "no always-on query surface for the partition")

# ── 6. Always-on, not armed ────────────────────────────────────────────────
check("round sampler defaults on (no env required)",
      re.search(r'getenv\("NDS_PROFILE_EMU"\)', impl) is not None
      and re.search(r'if \(!v \|\| !v\[0\]\) return 31u;', impl) is not None,
      "the default must measure; env is an override, never the enable")
check("bus sampler defaults on",
      re.search(r'if \(!v \|\| !v\[0\]\) return 1009u;', impl) is not None,
      "the bus breakdown must default on")

print()
if failures:
    print(f"FAILED: {len(failures)} assertion(s): {', '.join(failures)}")
    sys.exit(1)
print("emu-attribution structural guards: all assertions hold")
sys.exit(0)
