#!/usr/bin/env python3
"""Offline symbolizer for the runner's always-on host CPU sampler.

WHY THIS IS A SEPARATE TOOL. The runner records raw host RIPs and the module
map (base/size/path) that gives them meaning for that ONE process -- ASLR moves
every base each launch, so the map must be captured with the samples and cannot
be reconstructed afterwards. What the runner deliberately does NOT do is carry a
symbol table: nds_runner.exe with a title's generated banks linked in has
hundreds of thousands of functions, and resolving names in-process would mean
either shipping dbghelp and a PDB or holding a parsed DWARF in memory for the
life of every session. Names are only ever wanted after the fact, by whoever is
reading a dump, and that is exactly where the mingw binutils already live.

WHAT IT READS. A dump written by `hostprof_dump` over the debug port or by the
diagnostics bundle at shutdown (runner/src/host_profile.h describes the format):
one UTF-8 JSON metadata line, then fixed-width NdsHostProfSample records.

WHAT IT PRINTS.
  * SELF time: the leaf frame of each sample. This is the number to trust --
    it is just RIP and does not depend on any unwind step succeeding.
  * INCLUSIVE time: samples in which a symbol appears ANYWHERE in the stack,
    counted once per sample. This is what answers "how much time is under the
    dispatch loop", and it is only as good as the unwinding, so the truncation
    figures are printed alongside rather than in a footnote.
  * CATEGORY: self time folded into the runner's actual subsystems, which is
    the shape the perf question was asked in.

Usage:
  python tools/hostprof_symbolize.py DUMP [--top 40] [--categories-only]
                                     [--nm PATH] [--addr2line PATH]
                                     [--role emu] [--json OUT.json]
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict

# ── Dump format ─────────────────────────────────────────────────────────
# Must match NdsHostProfSample in runner/src/host_prof_ring.h. '<' so there is
# no host-dependent padding: the struct is 16 header bytes plus N frames.
SAMPLE_HEADER = "<QIBBBB"
DEFAULT_FRAMES = 16

ROLE_NAMES = ["emu", "render", "audio", "live_compiler", "other"]
STOP_NAMES = ["depth", "root", "no_module", "no_pdata", "read_failed",
              "bad_info"]

DEFAULT_NM = r"C:\msys64\mingw64\bin\nm.exe"
DEFAULT_ADDR2LINE = r"C:\msys64\mingw64\bin\addr2line.exe"

# ── Classification ──────────────────────────────────────────────────────
#
# Source path wins over symbol name, and symbol name wins over module, because
# that is the order of specificity: a path names the translation unit the code
# actually came from, a name can be shared (operator new, memcpy), and a module
# is the coarsest fact available. Every rule is (regex, category).
#
# The categories are the ones the perf question was posed in: dispatch lookup
# and validation, bus decode, MMIO handlers, scheduler/CPU sync, gpu3d, gpu2d,
# audio, and the generated bodies -- plus the honest residue.
PATH_RULES = [
    # Generated guest code, whether statically linked into the exe from a
    # title's generated/ tree or compiled into a live shard DLL.
    (r"[/\\]generated[/\\]", "generated"),
    (r"[/\\]recomp[/\\]coverage_arm[79]_", "generated"),
    (r"[/\\]runtime_arm\.cpp", "dispatch"),
    (r"[/\\]runtime_abi_shims\.cpp", "dispatch"),
    (r"[/\\]dispatch_timing\.cpp", "dispatch"),
    (r"[/\\]tier3\.cpp", "tier3"),
    (r"[/\\]bus\.cpp", "bus/memory"),
    (r"[/\\]vram\.cpp", "bus/memory"),
    (r"[/\\]cp15\.cpp", "bus/memory"),
    (r"[/\\]io\.cpp", "io/MMIO"),
    (r"[/\\]scheduler\.cpp", "scheduler/sync"),
    (r"[/\\]gpu3d\.cpp", "gpu3d"),
    (r"[/\\]GPU3D", "gpu3d"),
    (r"[/\\]melonds_compute[/\\]", "gpu3d"),
    (r"[/\\]gpu2d\.cpp", "gpu2d"),
    (r"[/\\]spu\.cpp", "audio"),
    (r"[/\\]frontend\.cpp", "frontend/present"),
    (r"[/\\]live_overlay", "live_overlay"),
    (r"[/\\](wifi_net|net[/\\]|Wifi|slirp)", "network"),
    (r"[/\\](diagnostics|emu_profile|pc_profile|profile_report|host_prof"
     r"|host_profile|host_unwind)", "diagnostics"),
    (r"[/\\](savestate|battery_save|cart_backup|firmware)", "state/io"),
]

# Symbol-name rules. Matched against a NORMALIZED name: arguments stripped,
# and the `(anonymous namespace)::` qualifier removed -- without that
# normalization an anchored rule silently misses every static function, and the
# hottest sites in this runner (runtime_dispatch_impl, io_read, io_write) are
# exactly that. `(?:^|::)` rather than `^` for the same reason: a member or
# namespaced function is still that subsystem's code.
#
# Order is significance order: generated bodies first (they are the population
# every other rule must not steal from), then the runner's own subsystems, then
# the shared machinery (STL, libc) that belongs to whoever called it and can
# only be named as itself.
NAME_RULES = [
    # Generated guest bodies: <bank>_afunc_XXXXXXXX / _tfunc_ / the vectors.
    # These are the names a Release build with no DWARF still exposes, which is
    # why this rule -- not a source path -- is what actually classifies the
    # single biggest category in a title build.
    (r"_(afunc|tfunc)_[0-9A-Fa-f]{6,8}$", "generated"),
    (r"_(runtime_thumb|reset_vector|swi_vector_entry|dispatch_table)", "generated"),
    # Coverage banks name their bodies <bank>_coverage_<guest addr> rather than
    # _afunc_; same population, different generator.
    (r"_coverage_[0-9a-fA-F]{6,8}$", "generated"),
    # Two more generator naming schemes seen in a real MPH build: single-PC
    # transfer stubs (_sptra_) and the ARM7 WRAM-alias bodies (_alias_).
    (r"_(sptra|alias)_[0-9a-fA-F]{6,8}$", "generated"),
    (r"(?:^|::)runtime_dispatch", "dispatch"),
    (r"dispatch_validation|dispatch_lookup|(?:^|::)nds_dispatch"
     r"|(?:^|::)nds_(?:un)?register_dispatch|(?:^|::)nds_link"
     r"|(?:^|::)lookup_static_cached|(?:^|::)lookup_static", "dispatch"),
    (r"(?:^|::)tier3_|(?:^|::)nds_tier3", "tier3"),
    (r"(?:^|::)armv4t::", "arm interpreter"),
    # Cycle accounting and the per-block runtime helpers the generated bodies
    # call on every block: runtime_code_cycles / runtime_mem_cycles /
    # arm9_refill_cycles / arm7_cycle_combine / arm_cond_passes and friends.
    # These were the single largest slice of "other" on the first Kanden
    # profile, which is exactly the kind of thing a category table exists to
    # surface -- they are the timing model, not the guest code and not the
    # dispatcher.
    (r"(?:^|::)(runtime_(code|mem)_cycles|arm[79]_refill_cycles"
     r"|arm[79]_cycle_combine|arm_cond_passes|runtime_tick"
     r"|runtime_should_yield|runtime_call_should_return|runtime_live_transfer"
     r"|runtime_trace_event|runtime_code_generation|runtime_"
     r"|arm_set_nzcv|arm_set_flags|arm_shift|arm_ror|arm_lsl|arm_asr)",
     "runtime_arm (cycles/helpers)"),
    (r"(?:^|::)(nds_dma_run|nds_tick_timers|nds_dma|nds_timer|nds_irq"
     r"|nds_ipc|nds_tick_rtc|nds_tick_cart|nds_tick_wifi)",
     "devices (dma/timer/irq)"),
    # sha1.cpp lives in `namespace gba` (shared with gbarecomp): the content
    # hashing behind coverage capture and live-shard identity.
    (r"(?:^|::)gba::|sha1|Sha1|SHA1", "hashing/sha1"),
    (r"(?:^|::)bus_|(?:^|::)cp15_|(?:^|::)nds_vram|(?:^|::)vram_",
     "bus/memory"),
    (r"(?:^|::)io_(read|write)|(?:^|::)nds_io_", "io/MMIO"),
    (r"(?:^|::)scheduler_|(?:^|::)nds_scheduler"
     r"|melonDS::NDS::(Schedule|Cancel|RunPending|Register|Unregister)"
     r"|(?:^|::)next_scheduled_event_time|(?:^|::)nds_run_system_events"
     r"|(?:^|::)nds_next_system_event_time|(?:^|::)switch_to$"
     r"|(?:^|::)nds_cpu_halted|(?:^|::)publish_fast_limit"
     r"|(?:^|::)nds_event_break_hit", "scheduler/sync"),
    (r"(?:^|::)nds_gpu3d|melonDS::(GPU3D|SoftRenderer|Renderer3D"
     r"|ComputeRenderer|ClipAgainstPlane|YSort|Vertex|Polygon)"
     r"|(?:^|::)nds_compute|(?:^|::)nds_texture"
     r"|(?:^|::)compute_finish_readback|(?:^|::)compute_", "gpu3d"),
    (r"(?:^|::)nds_gpu2d|render_scanline|(?:^|::)compose|draw_bg"
     r"|draw_sprite|draw_obj|(?:^|::)decode_bg_line"
     r"|(?:^|::)render_engine_line|(?:^|::)render_obj_line"
     r"|(?:^|::)nds_tick_display", "gpu2d"),
    (r"(?:^|::)nds_spu|(?:^|::)spu_|audio_callback|SDL_Audio|(?:^|::)SPU"
     r"|(?:^|::)nds_tick_spu", "audio"),
    (r"(?:^|::)live_overlay", "live_overlay"),
    (r"(?:^|::)nds_frontend|nds_run_interactive|(?:^|::)present_"
     r"|(?:^|::)nds_relative_mouse|(?:^|::)nds_set_touch", "frontend/present"),
    (r"(?:^|::)wifi_net|melonDS::Wifi|slirp|(?:^|::)net_|(?:^|::)nds_wifi",
     "network"),
    (r"(?:^|::)nds_diagnostics|(?:^|::)nds_emu_profile|(?:^|::)nds_pc_profile"
     r"|(?:^|::)nds_hostprof|(?:^|::)nds_dispatch_timing"
     r"|(?:^|::)nds_host_unwind|(?:^|::)coverage_", "diagnostics"),
    (r"(?:^|::)(savestate|nds_savestate|battery_save|cart_backup"
     r"|firmware_state|nds_firmware)", "state/io"),
    # Not anchored at the end: glibc/msvcrt ship ISA-specialised bodies like
    # __memmove_avx_unaligned, and those are exactly the ones a memcpy-heavy
    # profile lands in.
    (r"(?:^|::)_{0,2}(mem(cpy|set|move|cmp)|str(len|cmp|cpy|chr)|wmem(cpy|set))",
     "libc/memory"),
    (r"operator new|operator delete|(?:^|::)_Znw|(?:^|::)_Zdl"
     r"|(?:^|::)(malloc|free|calloc|realloc)$|_int_malloc|_int_free",
     "libc/alloc"),
    (r"^std::|(?:^|::)__gnu_cxx::", "stl"),
]

# Blocking primitives, checked BEFORE everything else. An emu thread parked in
# NtWaitForSingleObject is not "OS overhead": it is the frame pacer doing its
# job, and folding it into a module bucket makes 45 percent of a healthy
# profile look like unexplained kernel time. Kept as its own category so the
# report can normalise the work breakdown over the samples where the thread was
# actually working.
WAIT_RULES = [
    (r"(?:^|::)(Nt|Zw)(WaitFor|DelayExecution|SignalAndWait|RemoveIoCompletion"
     r"|ReplyWaitReceivePort)", "wait/idle"),
    (r"(?:^|::)(WaitFor(Single|Multiple)Object|SleepEx|SwitchToThread"
     r"|SleepConditionVariable|RtlSleepConditionVariable"
     r"|RtlWaitOnAddress|NtYieldExecution)", "wait/idle"),
    (r"(?:^|::)SDL_(WaitSemaphore|Delay|WaitCondition|LockMutex)", "wait/idle"),
]
COMPILED_WAIT_RULES = [(re.compile(r), c) for r, c in WAIT_RULES]

MODULE_RULES = [
    (r"^(ntdll|kernel32|kernelbase|win32u|user32|gdi32|advapi32|sechost"
     r"|rpcrt4|msvcrt|ucrtbase|combase|ole32|shcore|bcrypt|cfgmgr32"
     r"|imm32|powrprof|umpdc|windows\.storage|wintypes|msctf)\.dll$", "os"),
    (r"^(opengl32|nvoglv64|ig[0-9a-z]+|amdvlk|vulkan|d3d|dxgi|nvapi"
     r"|atio6axx|nvgpucomp)", "gpu driver"),
    (r"^SDL[0-9]?\.dll$", "sdl"),
    (r"^(libgcc|libstdc\+\+|libwinpthread)", "libc/alloc"),
]

COMPILED_PATH_RULES = [(re.compile(p, re.I), c) for p, c in PATH_RULES]
COMPILED_NAME_RULES = [(re.compile(p), c) for p, c in NAME_RULES]
COMPILED_MODULE_RULES = [(re.compile(p, re.I), c) for p, c in MODULE_RULES]


ANON_RX = re.compile(r"\(anonymous namespace\)::")


def normalize_symbol(name: str) -> str:
    """Strip argument lists and the `(anonymous namespace)::` qualifier.

    nm --demangle prints a static C++ function as
    `(anonymous namespace)::runtime_dispatch_impl(unsigned int, NdsLinkSlot*)`.
    An anchored rule against that raw string matches nothing, so every static
    function -- which in this runner includes the dispatch loop and both MMIO
    handlers -- would land in "other". This is the difference between a category
    table that explains the profile and one that does not.
    """
    # The leading '~' marks an export-derived (approximate) name; it must not
    # defeat an anchored rule, or every system-DLL wait lands in "other".
    name = name.lstrip("~")
    name = ANON_RX.sub("", name)
    # Cut at the first '(' that opens an argument list, and at any lambda or
    # clone suffix, so `f(int) [clone .isra.0]` folds onto `f`.
    cut = name.find("(")
    if cut > 0:
        name = name[:cut]
    return name.strip()


def classify(module: str, name: str, path: str) -> str:
    if path:
        for rx, cat in COMPILED_PATH_RULES:
            if rx.search(path):
                return cat
    if name:
        norm = normalize_symbol(name)
        for rx, cat in COMPILED_WAIT_RULES:
            if rx.search(norm):
                return "wait/idle"
        for rx, cat in COMPILED_NAME_RULES:
            if rx.search(norm):
                return cat
    if module:
        for rx, cat in COMPILED_MODULE_RULES:
            if rx.search(module):
                return cat
    if not name:
        # Stated, not hidden: a frame with no symbol at all is a hole in the
        # answer and has to be visible in the category table so a reader can
        # bound how much of the profile is actually explained.
        return "unresolved"
    return "other"


# ── PE helpers ──────────────────────────────────────────────────────────
def pe_preferred_base(path: str) -> int | None:
    """The ImageBase the module was LINKED at.

    nm and addr2line report addresses in that space; the dump records the
    address the loader actually mapped it at. Getting this wrong shifts every
    symbol by the ASLR slide, which produces a plausible-looking and completely
    wrong report -- so it is read from the file rather than assumed.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(0x400)
    except OSError:
        return None
    if len(head) < 0x40 or head[:2] != b"MZ":
        return None
    lfanew = struct.unpack_from("<I", head, 0x3C)[0]
    if lfanew + 0x38 > len(head) or head[lfanew:lfanew + 4] != b"PE\0\0":
        return None
    magic = struct.unpack_from("<H", head, lfanew + 0x18)[0]
    if magic == 0x20B:      # PE32+
        return struct.unpack_from("<Q", head, lfanew + 0x18 + 0x18)[0]
    if magic == 0x10B:      # PE32
        return struct.unpack_from("<I", head, lfanew + 0x18 + 0x1C)[0]
    return None


def pe_exports(path: str) -> list[tuple[int, str]]:
    """[(rva, name)] from the PE export directory, sorted by rva.

    WHY THIS EXISTS. `nm` reads a symbol table, and the Windows system DLLs
    (ntdll, kernel32, win32u) and vendor DLLs (the GPU driver) ship none. Without
    this, every RIP in those modules collapses to one row called "ntdll.dll" --
    and on a healthy 60 FPS profile that one row is 45 percent of the samples,
    because it is the frame pacer's wait. A profile whose largest single entry is
    an unnamed module is not a profile anyone can act on.

    Exports are an APPROXIMATION: a RIP inside a non-exported function resolves
    to the nearest preceding export, which can be the wrong function. That is
    why the resolved name is suffixed with the offset and why this source is only
    consulted when nm found nothing -- an approximate name for kernel wait
    machinery is useful, and it never displaces a real symbol for our own code.
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    if len(data) < 0x40 or data[:2] != b"MZ":
        return []
    try:
        lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if data[lfanew:lfanew + 4] != b"PE\0\0":
            return []
        magic = struct.unpack_from("<H", data, lfanew + 0x18)[0]
        opt = lfanew + 0x18
        dir_off = opt + (0x70 if magic == 0x20B else 0x60)
        exp_rva, exp_size = struct.unpack_from("<II", data, dir_off)
        if not exp_rva or not exp_size:
            return []
        nsections = struct.unpack_from("<H", data, lfanew + 6)[0]
        sec_off = opt + struct.unpack_from("<H", data, lfanew + 0x14)[0]
        sections = []
        for i in range(nsections):
            o = sec_off + i * 40
            vaddr, = struct.unpack_from("<I", data, o + 12)
            vsize, = struct.unpack_from("<I", data, o + 8)
            praw, = struct.unpack_from("<I", data, o + 20)
            rsize, = struct.unpack_from("<I", data, o + 16)
            sections.append((vaddr, max(vsize, rsize), praw))

        def to_off(rva: int) -> int | None:
            for vaddr, vsize, praw in sections:
                if vaddr <= rva < vaddr + vsize:
                    return praw + (rva - vaddr)
            return None

        base = to_off(exp_rva)
        if base is None:
            return []
        n_names, = struct.unpack_from("<I", data, base + 0x18)
        addr_funcs, addr_names, addr_ords = struct.unpack_from(
            "<III", data, base + 0x1C)
        of, on, oo = to_off(addr_funcs), to_off(addr_names), to_off(addr_ords)
        if None in (of, on, oo):
            return []
        out = []
        for i in range(n_names):
            name_rva, = struct.unpack_from("<I", data, on + 4 * i)
            no = to_off(name_rva)
            if no is None:
                continue
            end = data.index(b"\0", no)
            name = data[no:end].decode("ascii", "replace")
            ordinal, = struct.unpack_from("<H", data, oo + 2 * i)
            func_rva, = struct.unpack_from("<I", data, of + 4 * ordinal)
            if func_rva:
                out.append((func_rva, name))
        out.sort()
        return out
    except (struct.error, ValueError, IndexError):
        return []


# ── Symbol resolution ───────────────────────────────────────────────────
class ModuleSymbols:
    """Sorted symbol table for one module, from `nm --demangle -n`.

    nm is the fallback and the fast bulk path: it works on any module with a
    symbol table, needs no debug info, and one invocation covers every address.
    addr2line is layered on top for the file/line and inline detail, which is
    what the source-path classification rules key on.
    """

    def __init__(self, name: str, path: str, nm: str):
        self.name = name
        self.path = path
        self.from_exports = False
        self.pref_base = pe_preferred_base(path)
        self.addrs: list[int] = []
        self.names: list[str] = []
        self.error: str | None = None
        if self.pref_base is None:
            self.error = "no PE header"
            return
        if not os.path.isfile(path):
            self.error = "module file missing"
            return
        try:
            out = subprocess.run(
                [nm, "--demangle", "-n", "--defined-only", path],
                capture_output=True, text=True, errors="replace", timeout=600)
        except (OSError, subprocess.SubprocessError) as exc:
            self.error = f"nm failed: {exc}"
            out = None
        pairs = []
        for line in (out.stdout.splitlines() if out else []):
            # "0000000140001000 T some_symbol"
            parts = line.split(maxsplit=2)
            if len(parts) < 3:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            if parts[1] not in "TtWwiI":
                continue
            pairs.append((addr, parts[2]))
        pairs.sort()
        self.addrs = [a for a, _ in pairs]
        self.names = [n for _, n in pairs]
        if not pairs:
            # No symbol table (every Windows system DLL, every vendor DLL). Use
            # the export directory instead; approximate, and flagged as such.
            exports = pe_exports(path)
            self.from_exports = True
            self.addrs = [self.pref_base + rva for rva, _ in exports]
            self.names = [n for _, n in exports]
            if not exports:
                self.error = "no symbol table and no exports"
            else:
                self.error = None

    def lookup(self, rva: int) -> tuple[str, int]:
        """(symbol name, offset into it) for a module-relative address."""
        if not self.addrs or self.pref_base is None:
            return ("", 0)
        va = self.pref_base + rva
        i = bisect.bisect_right(self.addrs, va) - 1
        if i < 0:
            return ("", 0)
        return (self.names[i], va - self.addrs[i])


def addr2line_batch(a2l: str, path: str, pref_base: int,
                    rvas: list[int]) -> dict[int, tuple[str, str]]:
    """{rva: (function, file:line)} via one addr2line invocation.

    -i so an inlined frame reports the INLINE chain; the innermost entry is
    what a self-time table wants, and the outermost is the real symbol, so both
    are kept and the innermost wins for classification.
    """
    if not rvas or pref_base is None:
        return {}
    args = [a2l, "-f", "-i", "-C", "-e", path]
    addrs = [f"0x{pref_base + r:x}" for r in rvas]
    try:
        out = subprocess.run(args + addrs, capture_output=True, text=True,
                             errors="replace", timeout=900)
    except (OSError, subprocess.SubprocessError):
        return {}
    lines = [ln.rstrip("\n") for ln in out.stdout.splitlines()]
    # addr2line -i emits a variable number of (func, loc) pairs per address and
    # no separator, which makes a positional split unreliable. Re-run without
    # -i for a fixed 2-lines-per-address mapping and use -i only as detail when
    # the counts happen to line up.
    if len(lines) != 2 * len(rvas):
        try:
            out = subprocess.run([a2l, "-f", "-C", "-e", path] + addrs,
                                 capture_output=True, text=True,
                                 errors="replace", timeout=900)
        except (OSError, subprocess.SubprocessError):
            return {}
        lines = [ln.rstrip("\n") for ln in out.stdout.splitlines()]
    if len(lines) != 2 * len(rvas):
        return {}
    result = {}
    for i, rva in enumerate(rvas):
        func = lines[2 * i].strip()
        loc = lines[2 * i + 1].strip()
        if func in ("??", ""):
            func = ""
        if loc.startswith("??"):
            loc = ""
        result[rva] = (func, loc)
    return result


# ── Dump reading ────────────────────────────────────────────────────────
class Dump:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            header_line = f.readline()
            body = f.read()
        try:
            self.meta = json.loads(header_line.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise SystemExit(f"{path}: not a hostprof dump ({exc})")
        if self.meta.get("format") != "ndsrecomp-hostprof":
            raise SystemExit(f"{path}: unexpected format "
                             f"{self.meta.get('format')!r}")
        self.frames_per_sample = int(self.meta.get("frames_per_sample",
                                                   DEFAULT_FRAMES))
        self.sample_bytes = int(self.meta.get("sample_bytes",
                                             16 + 8 * self.frames_per_sample))
        expect = 16 + 8 * self.frames_per_sample
        if self.sample_bytes != expect:
            raise SystemExit(f"{path}: sample_bytes {self.sample_bytes} does "
                             f"not match {self.frames_per_sample} frames")
        self.count = len(body) // self.sample_bytes
        self.trailing = len(body) % self.sample_bytes
        self.body = body
        declared = self.meta.get("sample_count")
        self.declared = int(declared) if declared not in (None, "") else None
        self.modules = sorted(
            ({"name": m["name"], "path": m["path"], "base": int(m["base"]),
              "size": int(m["size"]), "main": bool(m.get("main"))}
             for m in self.meta.get("modules", [])),
            key=lambda m: m["base"])
        self._bases = [m["base"] for m in self.modules]
        self.fmt = struct.Struct(SAMPLE_HEADER +
                                 f"{self.frames_per_sample}Q")

    def samples(self, role: int | None = None):
        for i in range(self.count):
            off = i * self.sample_bytes
            vals = self.fmt.unpack_from(self.body, off)
            qpc, tid, depth, r, stop = vals[0], vals[1], vals[2], vals[3], \
                vals[4]
            if role is not None and r != role:
                continue
            frames = vals[6:6 + depth]
            yield qpc, tid, depth, r, stop, frames

    def module_of(self, addr: int):
        i = bisect.bisect_right(self._bases, addr) - 1
        if i < 0:
            return None
        m = self.modules[i]
        if addr < m["base"] or addr >= m["base"] + m["size"]:
            return None
        return m


# ── Report ──────────────────────────────────────────────────────────────
def fmt_row(rank, count, total, label, extra=""):
    share = 100.0 * count / total if total else 0.0
    return f"{rank:>4}  {count:>9}  {share:6.2f}%  {label}{extra}"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Symbolize an ndsrecomp hostprof dump.")
    ap.add_argument("dump")
    ap.add_argument("--top", type=int, default=40,
                    help="rows in the self and inclusive tables (default 40)")
    ap.add_argument("--nm", default=DEFAULT_NM)
    ap.add_argument("--addr2line", default=DEFAULT_ADDR2LINE)
    ap.add_argument("--role", default="emu",
                    help="role to report, or 'all' (default emu)")
    ap.add_argument("--categories-only", action="store_true")
    ap.add_argument("--no-addr2line", action="store_true",
                    help="skip addr2line; classify from symbol names only")
    ap.add_argument("--json", help="also write the tables as JSON here")
    ap.add_argument("--callers", metavar="SYMBOL",
                    help="attribute a leaf: print the parent-frame chains of "
                         "every sample whose LEAF symbol matches SYMBOL "
                         "(substring, case-insensitive). Answers 'who calls "
                         "this' for a symbol the source grep cannot find -- "
                         "an ntdll or SDL entry point reached from somewhere "
                         "in our own code. Reads the same always-on ring "
                         "dump; nothing is armed or re-run.")
    ap.add_argument("--caller-depth", type=int, default=6,
                    help="how many parent frames of each chain to print "
                         "(default 6)")
    args = ap.parse_args()

    dump = Dump(args.dump)
    role = None
    if args.role != "all":
        if args.role not in ROLE_NAMES:
            raise SystemExit(f"unknown role {args.role!r}; "
                             f"one of {ROLE_NAMES} or 'all'")
        role = ROLE_NAMES.index(args.role)

    # Pass 1: which addresses need resolving, and the raw tallies.
    self_counts: Counter[int] = Counter()
    incl_counts: Counter[int] = Counter()
    stop_counts: Counter[int] = Counter()
    depth_hist: Counter[int] = Counter()
    role_counts: Counter[int] = Counter()
    caller_stacks: list[tuple[int, ...]] = []
    total = 0
    qpc_lo = qpc_hi = None
    for qpc, _tid, depth, r, stop, frames in dump.samples(role):
        total += 1
        role_counts[r] += 1
        stop_counts[stop] += 1
        depth_hist[depth] += 1
        if qpc_lo is None or qpc < qpc_lo:
            qpc_lo = qpc
        if qpc_hi is None or qpc > qpc_hi:
            qpc_hi = qpc
        if depth:
            self_counts[frames[0]] += 1
            if args.callers:
                # Keep the WHOLE stack of every sample, keyed by leaf address.
                # Resolution happens after the symbol tables are built, so the
                # match is on names, not on a guessed address.
                caller_stacks.append(tuple(frames))
        # Inclusive: once per sample per distinct address, so a recursive frame
        # is not counted N times for one sample.
        for addr in set(frames):
            incl_counts[addr] += 1

    if not total:
        print(f"{args.dump}: no samples for role {args.role}")
        return 1

    # Group every address that appears at all by module.
    wanted = set(self_counts) | set(incl_counts)
    for chain in caller_stacks:
        wanted.update(chain)
    by_module: dict[str, list[int]] = defaultdict(list)
    addr_module: dict[int, dict | None] = {}
    for addr in wanted:
        m = dump.module_of(addr)
        addr_module[addr] = m
        if m:
            by_module[m["path"]].append(addr - m["base"])

    # Resolve. nm gives every module a bulk symbol table; addr2line adds the
    # source path the classification rules prefer.
    symtabs: dict[str, ModuleSymbols] = {}
    a2l: dict[str, dict[int, tuple[str, str]]] = {}
    for path, rvas in by_module.items():
        name = next(m["name"] for m in dump.modules if m["path"] == path)
        st = ModuleSymbols(name, path, args.nm)
        symtabs[path] = st
        if st.error:
            print(f"[warn] {name}: {st.error}", file=sys.stderr)
            continue
        if not args.no_addr2line:
            a2l[path] = addr2line_batch(args.addr2line, path, st.pref_base,
                                        sorted(set(rvas)))

    # (module, fold key, source location, category). The FOLD KEY is what the
    # tables group by, and it is deliberately not the display label: samples
    # inside one function must merge (a hot function spreads over its whole
    # body), while samples with NO symbol must NOT merge (collapsing every
    # unnamed RIP in a module onto the module name produced a single 45 percent
    # row called "ntdll.dll" -- technically true and useless).
    resolved_cache: dict[int, tuple[str, str, str, str]] = {}

    def resolve(addr: int) -> tuple[str, str, str, str]:
        hit = resolved_cache.get(addr)
        if hit is not None:
            return hit
        m = addr_module.get(addr)
        if not m:
            out = ("", f"<no module 0x{addr:x}>", "", "unresolved")
            resolved_cache[addr] = out
            return out
        rva = addr - m["base"]
        st = symtabs.get(m["path"])
        name, off = st.lookup(rva) if st else ("", 0)
        func, loc = a2l.get(m["path"], {}).get(rva, ("", ""))
        # addr2line's name wins when it has one (it sees inlines and statics);
        # nm's symbol table is the fallback; the PE export directory is the
        # fallback's fallback, marked with a leading ~ because the nearest
        # preceding export can be the wrong function.
        symbol = func or name
        if symbol and st is not None and st.from_exports:
            symbol = "~" + symbol
        key = symbol if symbol else f"{m['name']}+0x{rva:x}"
        out = (m["name"], key, loc, classify(m["name"], symbol, loc))
        resolved_cache[addr] = out
        return out

    # Fold by symbol rather than by address: a hot function samples across many
    # instructions and a per-RIP table buries it under its own inner loop.
    self_by_symbol: Counter[tuple[str, str]] = Counter()
    self_by_category: Counter[str] = Counter()
    wait_samples = 0
    for addr, c in self_counts.items():
        module, key, loc, cat = resolve(addr)
        self_by_symbol[(module, key)] += c
        self_by_category[cat] += c
        if cat == "wait/idle":
            wait_samples += c

    incl_by_symbol: Counter[tuple[str, str]] = Counter()
    for addr, c in incl_counts.items():
        module, key, _loc, _cat = resolve(addr)
        incl_by_symbol[(module, key)] += c

    freq = int(dump.meta.get("qpc_freq") or 1)
    span_s = (qpc_hi - qpc_lo) / freq if (qpc_hi and qpc_lo and freq) else 0.0

    print(f"hostprof dump      {args.dump}")
    print(f"format/version     {dump.meta.get('format')} "
          f"v{dump.meta.get('version')}")
    print(f"samples            {total} (role={args.role}, "
          f"file holds {dump.count})")
    if dump.declared is not None and dump.declared != dump.count:
        print(f"  [warn] header declares {dump.declared} samples but the file "
              f"holds {dump.count} -- dump may be truncated")
    if dump.trailing:
        print(f"  [warn] {dump.trailing} trailing bytes are not a whole sample")
    print(f"sampler            {dump.meta.get('hz')} Hz, ring "
          f"{dump.meta.get('ring_capacity')} "
          f"(written {dump.meta.get('ring_written')}, "
          f"evicted {dump.meta.get('ring_evicted')})")
    print(f"window             {span_s:.2f} s of samples")
    print(f"modules            {len(dump.modules)}")
    ov = dump.meta.get("overhead") or {}
    if ov:
        print(f"observer cost      {ov.get('suspended_us_per_sample', 0):.2f} "
              f"us suspended per sample, "
              f"{ov.get('suspended_ms', 0):.1f} ms total suspended, "
              f"{ov.get('walk_ms', 0):.1f} ms sampler-side, "
              f"{ov.get('suspend_fail', 0)} suspend failures")
    print("stop reasons       " + ", ".join(
        f"{STOP_NAMES[k] if k < len(STOP_NAMES) else k}={v} "
        f"({100.0 * v / total:.1f}%)"
        for k, v in sorted(stop_counts.items(), key=lambda kv: -kv[1])))
    mean_depth = sum(d * c for d, c in depth_hist.items()) / total
    print(f"mean stack depth   {mean_depth:.2f} of "
          f"{dump.frames_per_sample} captured")
    unresolved = self_by_category.get("unresolved", 0)
    print(f"unresolved (self)  {unresolved} "
          f"({100.0 * unresolved / total:.2f}%)")
    print()

    print("── SELF time by category "
          "─────────────────────────────────────────────")
    print(f"{'rank':>4}  {'samples':>9}  {'share':>7}  category")
    for i, (cat, c) in enumerate(self_by_category.most_common(), 1):
        print(fmt_row(i, c, total, cat))
    print()

    # The same table normalised over BUSY samples. A thread parked in the frame
    # pacer's wait is not overhead, but leaving it in the denominator shrinks
    # every real category by the same factor and makes a healthy 60 FPS profile
    # read as "half the time is in the OS". Both views are printed because the
    # first answers "is there headroom" and only the second answers "where does
    # the work go".
    busy = total - wait_samples
    if wait_samples and busy > 0:
        print(f"── SELF time by category, BUSY samples only "
              f"({busy} of {total}, "
              f"{100.0 * wait_samples / total:.1f}% was wait/idle) ───")
        print(f"{'rank':>4}  {'samples':>9}  {'share':>7}  category")
        rank = 0
        for cat, c in self_by_category.most_common():
            if cat == "wait/idle":
                continue
            rank += 1
            print(fmt_row(rank, c, busy, cat))
        print()

    if not args.categories_only:
        print(f"── SELF time, top {args.top} host symbols "
              "──────────────────────────────────")
        print(f"{'rank':>4}  {'samples':>9}  {'share':>7}  symbol")
        for i, ((module, label), c) in enumerate(
                self_by_symbol.most_common(args.top), 1):
            print(fmt_row(i, c, total, label,
                          f"   [{module}]" if module else ""))
        print()

        print(f"── INCLUSIVE (stack-based), top {args.top} "
              "─────────────────────────────────")
        print("(a sample counts once for every distinct symbol on its stack; "
              "only as")
        print(" good as the unwinding -- see the stop reasons above)")
        print(f"{'rank':>4}  {'samples':>9}  {'share':>7}  symbol")
        for i, ((module, label), c) in enumerate(
                incl_by_symbol.most_common(args.top), 1):
            print(fmt_row(i, c, total, label,
                          f"   [{module}]" if module else ""))
        print()

    caller_report: list[dict] = []
    if args.callers:
        # WHY THIS EXISTS. A leaf symbol with self time and no caller in the
        # source tree (a bare ntdll/SDL entry point) is unactionable: you
        # cannot hoist a call you cannot find. The always-on ring already
        # holds the parent frames of every sample it took, so the attribution
        # is a QUERY over the recorded window -- never an arm-then-capture.
        needle = args.callers.lower()
        chains: Counter[tuple[str, ...]] = Counter()
        leaf_labels: Counter[str] = Counter()
        matched = 0
        for chain in caller_stacks:
            leaf_label = resolve(chain[0])[1]
            if needle not in leaf_label.lower():
                continue
            matched += 1
            leaf_labels[leaf_label] += 1
            parents = []
            for addr in chain[1:1 + max(args.caller_depth, 0)]:
                module, label, _loc, _cat = resolve(addr)
                parents.append(f"{label} [{module}]" if module else label)
            chains[tuple(parents)] += 1
        print(f"── CALLERS of {args.callers!r} "
              "───────────────────────────────────────────")
        print(f"leaf samples       {matched} "
              f"({100.0 * matched / total:.2f}% of role samples)")
        if not matched:
            print("(no sample in this dump has a leaf matching that symbol; "
                  "note --role defaults to emu)")
        for label, c in leaf_labels.most_common():
            print(f"  leaf  {c:>8}  {100.0 * c / total:6.2f}%  {label}")
        print()
        for i, (parents, c) in enumerate(chains.most_common(args.top), 1):
            share = 100.0 * c / total
            print(f"  #{i} {c} sample(s), {share:.2f}% of role samples")
            if not parents:
                print("      <no parent frame -- unwind stopped at the leaf>")
            for d, label in enumerate(parents, 1):
                print(f"      ^{d} {label}")
            caller_report.append({"samples": c, "share_pct": share,
                                  "parents": list(parents)})
        print()

    if args.json:
        out = {
            "dump": args.dump,
            "role": args.role,
            "samples": total,
            "window_sec": span_s,
            "meta": dump.meta,
            "stops": {STOP_NAMES[k] if k < len(STOP_NAMES) else str(k): v
                      for k, v in stop_counts.items()},
            "self_by_category": dict(self_by_category),
            "self_by_symbol": [
                {"module": m, "symbol": s, "samples": c}
                for (m, s), c in self_by_symbol.most_common()],
            "inclusive_by_symbol": [
                {"module": m, "symbol": s, "samples": c}
                for (m, s), c in incl_by_symbol.most_common(args.top * 4)],
        }
        if args.callers:
            out["callers_of"] = args.callers
            out["caller_chains"] = caller_report
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"json               {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
