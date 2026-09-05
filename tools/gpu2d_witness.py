"""Read gpu2d threading/fence counters and display_ns from a serve-mode runner.

Usage: gpu2d_witness.py <exe> <bios> <port> <insn9-target> [--rom PATH]
       [--boot direct|lle] [--config PATH] [extra runner args...]
Env is inherited, so set NDS_GPU2D_THREADED / NDS_GPU2D_WORKERS / NDS_PROFILE_*
before invoking.
"""
import json
import os
import pathlib
import socket
import subprocess
import sys
import time


class Client:
    def __init__(self, port, timeout=600.0):
        deadline = time.time() + 60.0
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 5.0)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.s.settimeout(timeout)
        self.f = self.s.makefile("rwb")

    def cmd(self, name, **kw):
        payload = dict(kw)
        payload["cmd"] = name
        self.f.write((json.dumps(payload) + "\n").encode())
        self.f.flush()
        line = self.f.readline()
        return json.loads(line.decode())

    def close(self):
        try:
            self.f.close()
            self.s.close()
        except OSError:
            pass


def main():
    exe, bios, port, target = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    rest = sys.argv[5:]
    cmd = [exe, bios, "--serve", "--port", str(port), "--no-save"] + rest
    log = pathlib.Path(os.environ.get("TEMP", ".")) / f"gpu2d-witness-{port}.log"
    with open(log, "wb") as handle:
        proc = subprocess.Popen(cmd, stdout=handle, stderr=subprocess.STDOUT)
        try:
            c = Client(port)
            reply = c.cmd("run_to_event", event="insn9", count=target,
                          max_rounds=100000000)
            if not reply.get("reached", False):
                print("NOT REACHED", reply)
                return 1
            prof = c.cmd("profile")
            counts = c.cmd("event_counts")
            g = prof.get("gpu2d", {})
            sched = prof.get("sched", {})
            out = {
                "threaded_lines": g.get("threaded_lines"),
                "inline_lines": g.get("inline_lines"),
                "fence_helped_lines": g.get("fence_helped_lines"),
                "fence_wait_ns": g.get("fence_wait_ns"),
                "fence_drains": g.get("fence_drains"),
                "fenced_lines": g.get("fenced_lines"),
                "gpu2d_render_ns": g.get("render_ns"),
                "gpu2d_scanlines": g.get("scanlines"),
                "sched_display_ns": sched.get("display_ns"),
                "sched_devices_ns": sched.get("devices_ns"),
                "sched_sampled_round_ns": sched.get("sampled_round_ns"),
                "sched_sampled_rounds": sched.get("sampled_rounds"),
                "sched_arm9_ns": sched.get("arm9_ns"),
                "sched_arm7_ns": sched.get("arm7_ns"),
                "insn9": counts.get("insn9"),
                "vblank9": counts.get("vblank9"),
            }
            print(json.dumps(out, indent=2))
            c.close()
        finally:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
