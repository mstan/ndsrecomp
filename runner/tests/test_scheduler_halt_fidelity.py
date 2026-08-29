#!/usr/bin/env python3
"""Structural regression checks for halted-CPU scheduling fidelity
(beads-yjp.48, runner/src/scheduler.cpp).

WHY A STRUCTURAL TEST
---------------------
The property this file protects is WHEN a halted core's clock is allowed to
stop, and that is not a number any unit test can assert -- it is a property of
the shape of scheduler_run_round(). The failure mode is silent and expensive:

  62dbbc7 shortened the ARM7 catch-up target for a guest-halted ARM7 to that
  core's next timer overflow and ticked the timers there. The runner still
  built, still booted, still rendered, and every frame-hash checkpoint still
  matched -- but the halted ARM7 now left HALT at the exact overflow cycle
  instead of at the round rendezvous, up to one rendezvous early. melonDS
  ARMv4::Execute snaps a halted core straight to ARM7Target and NDS::RunFrame
  calls RunTimers(1) only there, so the oracle wakes later. The G1
  calibration_save gate went red for five days with an SPU output mismatch at
  audio frame 17402, ~0.15 s of guest time downstream of the actual defect
  (first divergent ARM7 retired-instruction ordinal 2849672: native cyc7
  5087805 vs oracle 5087820). 5d5d07d then patched one symptom of the same
  shortcut (a DMA-stalled ARM7 never waking) without removing it.

So the invariants below are named assertions:

  * the ARM7 catch-up loop hands every state to run_slice -- no per-CPU
    deadline may shorten the target of a halted core;
  * run_slice's guest-halt branch still consumes the WHOLE quantum;
  * the timer tick happens once per catch-up iteration, after the slice
    (melonDS RunTimers ordering);
  * the whole-console idle fast-forward still snaps a timer overflow onto the
    kIterCap rendezvous grid -- that snap is the only reason a timer overflow
    is allowed to influence a deadline at all;
  * no per-CPU next-timer-overflow query is reintroduced.

Run standalone or through ctest (`scheduler_halt_fidelity_test`).
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "runner/src"

failures = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name}: {detail}")
        failures.append(name)


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def brace_body(source, start_index):
    """Body text of the block whose opening brace is at/after start_index."""
    i = source.index("{", start_index)
    depth, j = 1, i + 1
    while j < len(source) and depth:
        if source[j] == "{":
            depth += 1
        elif source[j] == "}":
            depth -= 1
        j += 1
    return source[i + 1:j - 1]


def function_body(source, name, path):
    match = re.search(
        r"\b(?:void|bool|uint64_t|uint32_t)\s+" + re.escape(name) +
        r"\s*\([^;{]*?\)\s*\{", source, re.S)
    if not match:
        raise AssertionError(f"missing function {name} in {path}")
    return brace_body(source, match.start())


sched_path = SRC / "scheduler.cpp"
sched = read(sched_path)

# Strip comments so a comment that merely NAMES the removed shortcut (this
# file's own rationale lives in scheduler.cpp) cannot satisfy or trip a check.
no_comments = re.sub(r"//[^\n]*", "", sched)
no_comments = re.sub(r"/\*.*?\*/", "", no_comments, flags=re.S)

round_body = function_body(no_comments, "scheduler_run_round", sched_path)

# The ARM7 catch-up loop: `while (g_slot[1].started ... )`.
loop_match = re.search(r"while\s*\(\s*g_slot\[1\]\.started[^{]*\{", round_body,
                       re.S)
check("ARM7 catch-up loop found", loop_match is not None,
      "scheduler_run_round no longer has the g_slot[1] catch-up loop")

if loop_match:
    loop = brace_body(round_body, loop_match.start())

    # 1. Every state goes through run_slice. A `continue`/assignment path that
    #    parks g_slot[1].cycles somewhere other than run_slice's result is the
    #    exact shape of the yjp.48 defect.
    check("ARM7 catch-up loop calls run_slice exactly once",
          len(re.findall(r"\brun_slice\s*\(\s*1\s*,", loop)) == 1,
          f"{len(re.findall(r'run_slice', loop))} run_slice call(s) in the loop")
    assigns = re.findall(r"g_slot\[1\]\.cycles\s*=(?![=>])", loop)
    check("ARM7 catch-up loop never assigns g_slot[1].cycles directly",
          not assigns,
          "the loop writes the ARM7 timestamp itself instead of letting "
          "run_slice produce it -- a halted core must be snapped to the "
          "rendezvous, never to an intra-round deadline")
    check("ARM7 catch-up loop has no early continue",
          "continue" not in loop,
          "a `continue` here means some state skips run_slice")
    check("ARM7 catch-up loop does not consult a timer deadline",
          "timer_overflow" not in loop,
          "a per-CPU timer overflow must not shorten a halted core's target")

    # 2. Timer tick ordering: run_slice, then nds_tick_timers(1, ...), once.
    order = re.search(
        r"run_slice\s*\(\s*1\s*,[^;]*;\s*nds_tick_timers\s*\(\s*1\s*,", loop,
        re.S)
    check("nds_tick_timers(1, ...) follows run_slice(1, ...)",
          order is not None,
          "melonDS runs RunTimers(1) immediately after ARM7.Execute()")
    check("exactly one ARM7 timer tick per catch-up iteration",
          len(re.findall(r"nds_tick_timers\s*\(\s*1\s*,", loop)) == 1)

    # 3. The 64-bit gap must be clamped, not truncated, before run_slice.
    check("catch-up quantum is clamped to UINT32_MAX",
          "UINT32_MAX" in loop,
          "a raw static_cast<uint32_t> of the gap truncates on a long jump")

# 4. run_slice's guest-halt branch still burns the whole quantum.
slice_body = function_body(no_comments, "run_slice", sched_path)
halt_branch = re.search(
    r"if\s*\(\s*nds_cpu_halted\(cpu\)\s*&&\s*!\s*nds_halt_wake_pending\(cpu\)\s*\)"
    r"\s*\{(.*?)\}", slice_body, re.S)
check("run_slice keeps the guest-halt branch", halt_branch is not None,
      "the halted-CPU timestamp snap is gone from run_slice")
if halt_branch:
    check("run_slice's halt branch consumes the whole quantum",
          re.search(r"g_slot\[cpu\]\.cycles\s*\+=\s*quantum\s*;",
                    halt_branch.group(1)) is not None,
          "melonDS ARMv4::Execute sets ARM7Timestamp = ARM7Target for a "
          "halted core; a partial advance wakes it in the wrong round")

# 5. The whole-console idle fast-forward may use a timer overflow ONLY because
#    it snaps back onto the kIterCap rendezvous grid.
idle = re.search(
    r"nds_next_timer_overflow_time\(\)(.*?)if\s*\(\s*wake\s*>\s*planned\s*\)",
    round_body, re.S)
check("idle fast-forward found", idle is not None,
      "the both-halted fast-forward no longer reads the console timer deadline")
if idle:
    check("idle fast-forward snaps a timer overflow to the kIterCap grid",
          re.search(r"steps\s*\*\s*kIterCap", idle.group(1)) is not None,
          "without the grid snap this path delivers a timer IRQ earlier than "
          "the non-jumping scheduler, which is yjp.48 at console scope")
    check("idle fast-forward requires both cores halted with no wake pending",
          re.search(
              r"nds_cpu_halted\(0\)\s*&&\s*!\s*nds_halt_wake_pending\(0\)\s*&&"
              r"\s*nds_cpu_halted\(1\)\s*&&\s*!\s*nds_halt_wake_pending\(1\)",
              round_body, re.S) is not None,
          "one running core means there is no idle interval to skip")
    check("idle fast-forward excludes a DMA-stalled core",
          re.search(r"!\s*nds_dma_cpu_stalled\(0\)\s*&&"
                    r"\s*!\s*nds_dma_cpu_stalled\(1\)", round_body,
                    re.S) is not None,
          "a DMA-stalled core is not idle: its wake source is the transfer "
          "completion event, not a scheduled deadline")

# 6. No per-CPU timer-overflow deadline query anywhere in the runner.
offenders = []
for path in sorted(SRC.rglob("*.h")) + sorted(SRC.rglob("*.cpp")):
    text = read(path)
    stripped = re.sub(r"//[^\n]*", "", text)
    stripped = re.sub(r"/\*.*?\*/", "", stripped, flags=re.S)
    if "nds_next_timer_overflow_time_for_cpu" in stripped:
        offenders.append(path.name)
check("no per-CPU next-timer-overflow deadline helper", not offenders,
      f"reintroduced in {offenders}; per-timer deadlines belong in "
      f"nds_timer_debug_state(), never in a per-CPU scheduling target")

print()
if failures:
    print(f"FAILED: {len(failures)} assertion(s): {', '.join(failures)}")
    sys.exit(1)
print("scheduler halt-fidelity structural guards: all assertions hold")
sys.exit(0)
