#!/usr/bin/env python3
"""Check real-board video completion, SIGKILL/startup cancellation and memory.

Supply a short clip on the board with NO AUDIO when silence is required.
This test does not mute audio. It creates unique scratch files only.
"""
import argparse
import re
import shlex
import time
import uuid

from board import Board, PROMPT

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("port")
p.add_argument("clip")
p.add_argument("--rounds", type=int, default=12)
args = p.parse_args()
if args.rounds < 1:
    p.error("--rounds must be positive")
b = Board(args.port)
stats = "/tmp/video-life-" + uuid.uuid4().hex[:8]
clip = shlex.quote(args.clip)
started = False
try:
    b.send(b"\x15\r")
    b.read_until(PROMPT, 5)
    baseline = b.run("free; cpufreq; ps")
    print("before:", baseline)
    assert "held at" not in baseline
    out = b.run(f"video -v {clip} < /dev/null > {stats} 2>&1; echo complete=$?", 30)
    assert "complete=0" in out, out
    print("natural completion:", b.run(f"cat {stats}"))
    for i in range(args.rounds):
        delay = ["0.01", "0.1", "0.5"][i % 3]
        out = b.run(f"video -v {clip} < /dev/null > {stats} 2>&1 & v=$!; "
                    f"sleep {delay}; kill -9 $v; wait $v; echo killed=$?", 15)
        assert "killed=137" in out, (i, out)
        time.sleep(0.2)
        out = b.run("cpufreq; ps; echo VIDEO_LIFE_OK")
        assert "held at" not in out and "VIDEO_LIFE_OK" in out, (i, out)
        assert not re.search(r"^\s*\d+\s+\d+\s+[RT]\s+.*\bvideo\b", out, re.M), out
        print(f"SIGKILL round {i + 1} ({delay}s): restored terminal and boost, process reaped")
    for delay in (0.02, 0.15, 0.5):
        b.send(f"video -v {clip} > {stats} 2>&1\r".encode())
        started = True
        time.sleep(delay)
        b.send(b"\x03")
        out = b.read_until(PROMPT, 10)
        assert re.search(PROMPT, out), "Ctrl-C failed to restore shell"
        started = False
        out = b.run("cpufreq; echo VIDEO_CTRL_C_OK")
        assert "held at" not in out and "VIDEO_CTRL_C_OK" in out, out
        print(f"Ctrl-C ({delay}s): restored terminal and boost")
    final = b.run("free; cpufreq; ps; cat /proc/uptime")
    print("after:", final)
    for kind in ("internal", "psram"):
        pattern = rf"{kind}\s+\d+\s+\d+\s+(\d+)"
        before = int(re.search(pattern, baseline)[1])
        after = int(re.search(pattern, final)[1])
        assert after >= before - 8, (kind, before, after)
    print("video lifecycle passed; forced-reaper memory ordering is separately host-tested")
finally:
    if started:
        b.send(b"\x03")
        b.read_until(PROMPT, 10)
    b.run(f"rm -f {stats}")
    b.close()
