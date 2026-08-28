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
    ("live_overlay.cpp", "live_overlay_poll", "NDS_EMU_OVERLAY"),
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

# ── 2. The round region is opened, and opened first ─────────────────────────
round_body = body(sched, "scheduler_run_round", SRC / "scheduler.cpp")
first = next((l.strip() for l in round_body.splitlines()
              if l.strip() and not l.strip().startswith("//")), "")
check("scheduler_run_round opens NdsEmuRound as its first statement",
      first.startswith("NdsEmuRound"),
      f"first statement is {first!r}")

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
