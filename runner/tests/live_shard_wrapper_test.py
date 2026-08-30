#!/usr/bin/env python3
"""The generated shard wrapper must report its real NDS_STATIC_CPU.

compile_live_shards.py hands each shard its CPU identity twice: as bank
metadata and as -DNDS_STATIC_CPU, which folds the ARM9/ARM7 timing ternaries
into the generated bodies. The runner cross-checks the two (live_overlay.cpp
preflight_live_bank), so this pins the producer side: the wrapper the tool
emits compiles under the real ABI header and reports the CPU it was actually
built with, not one re-derived from the metadata.
"""

from __future__ import annotations

import argparse
import ctypes
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import compile_live_shards as tool  # noqa: E402

ROM_SHA1 = "0123456789abcdef0123456789abcdef01234567"

# A minimal stand-in for the generated bank: one dispatch row plus the data
# imports every live bank binds to runner storage.
STUB = """
#include "runtime_arm.h"

ArmCpuState g_cpu;
unsigned long long g_runtime_cycles;
NdsBusFastWin g_busf_main;
NdsBusFastWin g_busf_itcm;

static void body(void) {}
static const unsigned char expected[4] = {0, 0, 0, 0};
static const NdsStaticValidation validation = {
    0x02000000u, sizeof(expected), expected, 0, 0};
const NdsDispatchEntry g_dispatch_%(bank)s[] = {
    {0x02000000u, 0u, body, &validation},
};
const unsigned g_dispatch_%(bank)s_len = 1u;
"""


class BankInfo(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("bank_id", ctypes.c_char_p),
        ("candidate_id", ctypes.c_char_p),
        ("title_sha1", ctypes.c_char_p),
        ("cpu", ctypes.c_int),
        ("static_cpu", ctypes.c_uint32),
        ("exc_base", ctypes.c_uint32),
        ("dispatch", ctypes.c_void_p),
        ("dispatch_len", ctypes.c_uint),
        ("linked_g_cpu", ctypes.c_void_p),
        ("linked_busf_main", ctypes.c_void_p),
        ("linked_busf_itcm", ctypes.c_void_p),
        ("linked_runtime_cycles", ctypes.c_void_p),
    ]


def build(work: Path, gcc: str, cpu: int, static_cpu: int) -> Path:
    name = "arm9" if cpu == 9 else "arm7"
    bank = f"nds_live_{name}_02000000"
    src = work / f"cpu{cpu}_static{static_cpu}"
    src.mkdir(parents=True, exist_ok=True)
    (src / "stub.c").write_text(STUB % {"bank": bank}, encoding="utf-8")
    wrapper = src / f"{bank}_live.c"
    tool.write_wrapper(wrapper, bank, "candidate-test", "generation-test",
                       ROM_SHA1, cpu)
    suffix = ".dll" if sys.platform == "win32" else ".so"
    dll = src / f"{bank}{suffix}"
    command = [
        gcc, "-shared", "-O0", "-g0",
        f"-DNDS_STATIC_CPU={static_cpu}",
        "-I", str(ROOT / "recompiler" / "armv4t"),
        # A real shard build gets runtime_arm_types.h from the recompiler's
        # own output directory; this stub build has no recompiler run, so it
        # reads the header from arm-recomp-core directly.
        "-I", str(ROOT / "external" / "arm-recomp-core" / "common"),
        "-o", str(dll), str(src / "stub.c"), str(wrapper),
    ]
    if sys.platform != "win32":
        command.insert(2, "-fPIC")
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0:
        print(result.stdout, end="")
        raise AssertionError(f"shard wrapper failed to compile for {name}")
    return dll


def info(dll: Path) -> BankInfo:
    module = ctypes.CDLL(str(dll))
    module.nds_live_bank_info.restype = ctypes.POINTER(BankInfo)
    return module.nds_live_bank_info().contents


def check_tcc_command(work: Path) -> None:
    commands: list[list[str]] = []
    original_run = tool.run
    original_include = tool.tcc_include_dir
    original_import = tool.tcc_import_dir
    try:
        tool.run = lambda command: (
            commands.append(command) or subprocess.CompletedProcess(
                command, returncode=0))
        tool.tcc_include_dir = lambda args: work / "tcc-include"
        tool.tcc_import_dir = lambda args: work / "tcc-import"
        args = SimpleNamespace(
            compiler="tcc", tcc=Path("tcc"), runner_exe=Path("nds_runner"))
        ok = tool.compile_shard_dll(
            args, [work / "body.c"], work, work / "bank.stage" /
            f"bank{tool.SHARED_LIBRARY_SUFFIX}", 9)
        assert ok and len(commands) == 1
        command = commands[0]
        assert command[:2] == ["tcc", "-shared"]
        assert "-DNDS_STATIC_CPU=0" in command
        if sys.platform == "win32":
            assert any(arg.startswith("-L") for arg in command)
            assert "-lnds_runner" in command
        else:
            assert not any(arg.startswith("-L") for arg in command)
            assert not any(arg.startswith("-l") for arg in command)
    finally:
        tool.run = original_run
        tool.tcc_include_dir = original_include
        tool.tcc_import_dir = original_import


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gcc", default="gcc")
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    check_tcc_command(args.work)

    for cpu, expected_static in ((9, 0), (7, 1)):
        bank = info(build(args.work, args.gcc, cpu, expected_static))
        assert bank.abi_version == tool.ABI_VERSION, (
            f"wrapper published ABI {bank.abi_version}, tool mirrors "
            f"{tool.ABI_VERSION}")
        assert bank.cpu == expected_static, "metadata cpu should be 0/1"
        assert bank.static_cpu == expected_static, (
            f"cpu {cpu} wrapper reported static_cpu {bank.static_cpu}")

    # The whole point of the field: it follows -DNDS_STATIC_CPU, not the
    # metadata, so a build that disagrees is visible to the runner instead
    # of silently running under the other CPU's timing model.
    skewed = info(build(args.work, args.gcc, 9, 1))
    assert skewed.cpu == 0 and skewed.static_cpu == 1, (
        "static_cpu must report the compiled-with CPU, not re-derive it "
        f"from metadata (got cpu={skewed.cpu} static_cpu={skewed.static_cpu})")

    print("PASS: shard wrapper reports the NDS_STATIC_CPU it was built with")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
