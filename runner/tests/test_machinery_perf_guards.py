#!/usr/bin/env python3
"""Structural regression checks for the deadline-bounded per-instruction
machinery (beads-yjp.42 phase 1).

WHY A STRUCTURAL TEST
---------------------
This optimization is not a computation anyone can assert a value for. Its
correctness is a set of STRUCTURAL properties: a fast path exists and is
bounded by the scheduler's own slice cap; every site that can make a service
condition come due earlier drops the deadline back to zero; the faithful path
stays linked and reachable; and the deadline does not leak into live-shard
state in a way that gives shards different semantics from static banks.

Each of those is one named assertion below. A refactor that quietly deletes
one is exactly the failure mode this file exists to catch: it would not break
a build, would not fail a byte-lock on the machine it was written on, and
would silently mis-time a guest somewhere else. Ported from psxrecomp's
runtime/tests/test_interpreter_perf_guards.py, whose brace-matching body
extraction this reuses.

Run standalone or through ctest (`machinery_perf_guards_test`).
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]

RUNTIME_H = ROOT / "recompiler/armv4t/runtime_arm.h"
RUNTIME_CPP = ROOT / "runner/src/runtime_arm.cpp"
SHIMS_CPP = ROOT / "runner/src/runtime_abi_shims.cpp"
IO_CPP = ROOT / "runner/src/io.cpp"
DEBUG_CPP = ROOT / "runner/src/debug_server.cpp"
EXPORTS_DEF = ROOT / "runner/src/runtime_exports.def"
SHARD_TOOL = ROOT / "tools/compile_live_shards.py"


def body(source, name):
    """Return the brace-matched body of function `name`."""
    match = re.search(
        r"\b(?:extern\s+\"C\"\s+)?(?:static\s+)?(?:inline\s+)?"
        r"(?:void|bool|int|uint32_t|uint64_t|unsigned\s+long\s+long)"
        r"\s*\*?\s*" + name + r"\s*\([^;{]*?\)\s*\{",
        source, re.S)
    if not match:
        raise AssertionError(f"missing function {name}")
    start, depth = match.end(), 1
    for pos in range(start, len(source)):
        depth += source[pos] == "{"
        depth -= source[pos] == "}"
        if depth == 0:
            return source[start:pos]
    raise AssertionError(f"unterminated function {name}")


def fail(msg):
    raise AssertionError(msg)


def main():
    header = RUNTIME_H.read_text(encoding="utf-8", errors="replace")
    runtime = RUNTIME_CPP.read_text(encoding="utf-8", errors="replace")
    shims = SHIMS_CPP.read_text(encoding="utf-8", errors="replace")
    io = IO_CPP.read_text(encoding="utf-8", errors="replace")
    debug = DEBUG_CPP.read_text(encoding="utf-8", errors="replace")
    exports = EXPORTS_DEF.read_text(encoding="utf-8", errors="replace")

    # ── 1. The fast path exists, in the header, on the callers' side ──────
    # If these stop being inline the whole candidate is gone: the call
    # overhead it removes is most of what it removes.
    for name in ("runtime_tick", "runtime_should_yield", "runtime_unwinding"):
        if not re.search(r"NDS_MACHINERY_INLINE\s+\w+\s+" + name + r"\s*\(",
                         header):
            fail(f"{name} is no longer a header fast path "
                 "(NDS_MACHINERY_INLINE)")
    # The regression this catches actually happened: with plain `static
    # inline`, GCC -O3 declined to inline into the huge generated bank bodies
    # and emitted a local out-of-line copy per TU, keeping the per-instruction
    # call the deadline exists to remove. Losing the attribute would silently
    # undo most of the optimization while every other check still passed.
    if "always_inline" not in header:
        fail("the machinery fast paths no longer force inlining; GCC's "
             "inline-unit-growth heuristic will emit a local out-of-line "
             "copy in the generated banks and the call survives")

    tick = body(header, "runtime_tick")
    if "g_nds_fast_limit" not in tick or "runtime_tick_slow" not in tick:
        fail("runtime_tick fast path lost its deadline compare or its "
             "faithful tail")
    if "runtime_irq" in tick or "irq_pending" in tick:
        fail("runtime_tick fast path derives IRQ state inline; the point of "
             "the deadline is that it does not have to")

    yield_body = body(header, "runtime_should_yield")
    if ("g_runtime_cycles < g_nds_fast_limit" not in yield_body or
            "runtime_should_yield_slow" not in yield_body):
        fail("runtime_should_yield fast path lost its deadline compare or "
             "its faithful tail")
    for derived in ("nds_cpu_halted", "nds_event_break_hit", "g_cycle_cap",
                    "active_static_code_changed"):
        if derived in yield_body:
            fail(f"runtime_should_yield fast path derives {derived} inline")

    # ── 2. The deadline never exceeds the scheduler's slice bound ─────────
    # This is the dual-CPU safety property. publish_fast_limit may assign the
    # limit from g_cycle_cap and from nothing else: any other source could
    # carry one CPU past a rendezvous the peer can observe.
    publish = body(runtime, "publish_fast_limit")
    assigns = re.findall(r"g_nds_fast_limit\s*=\s*([^;]+);", publish)
    if not assigns:
        fail("publish_fast_limit assigns no deadline")
    for value in assigns:
        value = value.strip()
        if value not in ("0u", "0", "g_cycle_cap"):
            fail("publish_fast_limit sources the deadline from "
                 f"'{value}'; only the scheduler's live slice cap "
                 "(g_cycle_cap) bounds a cross-CPU rendezvous")
    if "g_cycle_cap == 0u" not in publish:
        fail("publish_fast_limit does not refuse an unbounded slice; an "
             "unbounded cap must mean zero deadline, not an infinite one")
    # Every disqualifier the faithful path checks per instruction but the
    # fast path does not.
    for condition in ("g_cycle_fast_limit_enabled",   # the selector
                      "g_cpu_fast_poll",              # faithful poll mode
                      "g_yield_poll_hint",            # existing eager hint
                      "g_runtime_break_pc",           # per-PC, not cyclic
                      "g_nds_terminal",
                      "g_deferred_cycles",            # HALT debt owed
                      "active_static_code_changed",   # guest rewrote code
                      "g_nds_irq_pending_cache"):     # IRQ due next tick
        if condition not in publish:
            fail(f"publish_fast_limit no longer disqualifies on {condition}")

    # publish is only ever reached from the faithful scan's all-clear exits.
    slow = body(runtime, "runtime_should_yield_slow")
    if slow.count("publish_fast_limit()") != 2:
        fail("the deadline must be published from exactly the two 'nothing "
             "to service' exits of the faithful scan (fast-hint exit and "
             "full-scan exit)")
    for line in re.findall(r"^.*publish_fast_limit\(\).*$", runtime, re.M):
        if "//" in line.split("publish_fast_limit")[0]:
            continue
        if not re.search(r"(void publish_fast_limit|publish_fast_limit\(\);)",
                         line):
            fail(f"unexpected publish_fast_limit use: {line.strip()}")

    # ── 3. Every re-arm site drops the deadline ──────────────────────────
    # The funnels first. request_yield_poll is the pre-existing eager hint,
    # so every site that already used it is covered transitively.
    rearm = body(runtime, "request_yield_poll")
    if "g_nds_fast_limit = 0" not in rearm:
        fail("request_yield_poll no longer clears the deadline; every site "
             "that arms the yield hint would keep a stale deadline")
    clear = body(runtime, "runtime_clear_fast_limit")
    if "g_nds_fast_limit = 0" not in clear:
        fail("runtime_clear_fast_limit does not clear the deadline")

    # Sites reached by neither funnel, each named for why it matters.
    named_sites = [
        (body(runtime, "nds_reschedule_slice"), "g_nds_fast_limit = 0",
         "a device scheduled an EARLIER deadline than the published limit"),
        (body(runtime, "nds_set_cycle_cap"), "g_nds_fast_limit = 0",
         "the slice cap the deadline is derived from was replaced"),
        (body(runtime, "runtime_deferred_cycles_set"), "g_nds_fast_limit = 0",
         "HALT/DMA debt must be committed by the next faithful tick"),
        (body(io, "irq_recompute"), "runtime_clear_fast_limit",
         "an IRQ became pending and the fast tick path does not look"),
    ]
    for src, needle, why in named_sites:
        if needle not in src:
            fail(f"re-arm site lost its deadline reset ({why})")

    if "runtime_clear_fast_limit" not in body(io, "nds_io_reset"):
        fail("machine reset does not clear the deadline")
    # Debug-server intervention from another thread.
    if debug.count("runtime_clear_fast_limit()") < 2:
        fail("run_to_pc must clear the deadline when it both sets and clears "
             "g_runtime_break_pc; the break-PC predicate is not cycle-based")

    # ── 4. The faithful path stays linked and selectable (LLE floor) ─────
    if "NDS_CYCLE_FAST_LIMIT" not in runtime:
        fail("the NDS_CYCLE_FAST_LIMIT selector is gone; the faithful path "
             "must stay reachable in the SAME binary")
    selector = body(runtime, "configured_cycle_fast_limit")
    if "getenv" not in selector or "NDS_CYCLE_FAST_LIMIT" not in selector:
        fail("the selector no longer reads NDS_CYCLE_FAST_LIMIT")
    for name in ("runtime_tick_slow", "runtime_should_yield_slow"):
        if f"extern \"C\" void {name}" not in runtime and \
                f"extern \"C\" bool {name}" not in runtime:
            fail(f"the faithful out-of-line path {name} is not defined")
    # Both witnesses a harness needs to prove which leg actually ran.
    for witness in ("cycle_fast_limit", "fast_limit_publishes"):
        if witness not in runtime:
            fail(f"dispatch_stats no longer reports {witness}; a selector "
                 "flag with no counter proves nothing")

    # ── 5. No divergent semantics for live shards ────────────────────────
    # Shards charge cycles per instruction exactly like static banks (they do
    # NOT batch), so they must get the same deadline, not a bypass. Shards
    # compiled before the change import the symbols by name, so the exported
    # out-of-line definitions must survive AND must be the same bodies.
    if "NDS_RUNTIME_ABI_SHIMS" not in header or \
            "NDS_RUNTIME_ABI_SHIMS" not in shims:
        fail("the shard ABI shim TU is gone; pre-existing live shard DLLs "
             "import runtime_tick/should_yield/unwinding by name")
    for name in ("runtime_tick", "runtime_should_yield", "runtime_unwinding"):
        if f"extern \"C\"" not in shims or name not in shims:
            fail(f"{name} is no longer exported out-of-line for old shards")
    # The two spellings must be one contract.
    shim_tick = body(shims, "runtime_tick")
    shim_yield = body(shims, "runtime_should_yield")
    norm = lambda s: re.sub(r"\s+|//[^\n]*", "", s)
    if norm(shim_tick) != norm(tick):
        fail("the exported runtime_tick shim has drifted from the header "
             "inline; shards and banks would time differently")
    if norm(shim_yield) != norm(yield_body):
        fail("the exported runtime_should_yield shim has drifted from the "
             "header inline")
    # A recompiled shard inlines the header, so its storage must resolve to
    # the runner's. tcc's shard header rewrite only marks line-initial
    # `extern` as dllimport, so the declarations must start at column 0.
    for symbol in ("g_nds_fast_limit", "g_nds_unwinding"):
        if not re.search(r"^extern\s[^\n]*\b" + symbol + r"\b", header, re.M):
            fail(f"{symbol} is not declared at column 0 with a leading "
                 "`extern`; tcc-built shards would not import it")
        if not re.search(r"^\s*" + symbol + r"\s+DATA\s*$", exports, re.M):
            fail(f"{symbol} is not a DATA export; a shard would get a "
                 "private copy instead of the runner's storage")
    for symbol in ("runtime_tick_slow", "runtime_should_yield_slow",
                   "runtime_clear_fast_limit"):
        if not re.search(r"^\s*" + symbol + r"\s*$", exports, re.M):
            fail(f"{symbol} is not exported; a recompiled shard inlining the "
                 "header could not reach the faithful tail")
    # No shard ever WRITES the deadline: it is runner-published state.
    if SHARD_TOOL.exists():
        tool = SHARD_TOOL.read_text(encoding="utf-8", errors="replace")
        if re.search(r"g_nds_fast_limit\s*=", tool):
            fail("the shard compiler writes the deadline; it is published "
                 "only by the runner")

    print("machinery perf guards: OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"machinery perf guards FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
