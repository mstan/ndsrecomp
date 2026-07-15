#!/usr/bin/env python3
"""Verify SM64DS secure-area cartridge and BIOS behavior against ndsref."""

from concurrent.futures import ThreadPoolExecutor

from _client import DebugClient, first_divergence


SECURE_RAM = 0x02004000
SECURE_SIZE = 0x800
CPU_SET_SWI = 0x0200438A


def sample(port):
    client = DebugClient(port=port, timeout=120.0)
    try:
        client.cmd("reset")
        hit = client.cmd("run_to_event", event="ipcsync_w", count=10,
                         stall=300_000)
        memory = bytes.fromhex(client.cmd(
            "read_mem", cpu=9, addr=SECURE_RAM, len=SECURE_SIZE)["hex"])
        cartridge = client.cmd("cartridge", max=8192)
        response = next(event for event in cartridge["events"]
                        if event["seq"] == 2206)
        return hit, memory, response
    finally:
        client.close()


def main():
    with ThreadPoolExecutor(max_workers=2) as pool:
        native_future = pool.submit(sample, 19842)
        oracle_future = pool.submit(sample, 19843)
        native = native_future.result()
        oracle = oracle_future.result()

    if not native[0].get("reached") or not oracle[0].get("reached"):
        raise SystemExit("firmware did not reach IPCSYNC 10")

    difference = first_divergence(native[1], oracle[1])
    if difference is not None:
        raise SystemExit(f"secure-area RAM differs: {difference}")

    swi_offset = CPU_SET_SWI - SECURE_RAM
    if native[1][swi_offset:swi_offset + 2] != b"\x0b\xdf":
        raise SystemExit("CpuSet SWI is absent at 0x0200438A")

    if native[2]["word"] != oracle[2]["word"]:
        raise SystemExit(
            f"first secure-area card word differs: "
            f"{native[2]['word']:08x}/{oracle[2]['word']:08x}")

    print("secure-area RAM: byte-exact")
    print("CpuSet SWI @ 0x0200438A: 0B DF")
    print(f"first card word: {native[2]['word']:08X}")


if __name__ == "__main__":
    main()
