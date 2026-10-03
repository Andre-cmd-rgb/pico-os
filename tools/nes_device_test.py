#!/usr/bin/env python3
"""Exercise NES forced exit/restart and silent foreground playback on a board.

    python3 tools/nes_device_test.py /dev/ttyACM0 /home/user/roms/game.nes

Use a supported, locally owned ROM. Every run uses -q: no audio output.
Scratch output is unique; existing ROMs and settings are retained.
"""
import argparse
import re
import shlex
import time
import uuid

from board import Board, PROMPT, clean


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("rom")
    parser.add_argument("--rounds", type=int, default=9)
    parser.add_argument("--seconds", type=float, default=6)
    args = parser.parse_args()
    if args.rounds < 1 or args.seconds <= 0:
        parser.error("rounds and seconds must be positive")
    b = Board(args.port)
    scratch = "/tmp/nes-test-" + uuid.uuid4().hex[:8]
    rom = shlex.quote(args.rom)
    foreground = False
    try:
        b.send(b"\x15\r")
        b.read_until(PROMPT, 5)
        print("before:", b.run("free; cat /proc/uptime"), flush=True)
        for i in range(args.rounds):
            delay = ["0.01", "0.1", "0.5"][i % 3]
            out = b.run(f"nes -q -v -f 0 {rom} > {scratch} 2>&1 & n=$!; "
                        f"sleep {delay}; kill -9 $n; wait $n; echo killed=$?", 15)
            assert "killed=137" in out, (i, out)
            out = b.run("echo NES_RESTART_OK; ps", 5)
            assert "NES_RESTART_OK" in out, (i, out)
            assert not re.search(r"^\s*\d+\s+\d+\s+[RT]\s+.*\bnes\b", out, re.M), out
            print(f"SIGKILL {i + 1} ({delay}s): reaped and shell restored", flush=True)
        # Restarting the real core after kills catches stale vendor pointers.
        for key in (b"q", b"\x03"):
            foreground = True
            b.send(f"nes -q -v -f 0 {rom}\r".encode())
            time.sleep(args.seconds)
            b.send(key)
            raw = b.read_until(PROMPT, 10)
            assert re.search(PROMPT, raw), ("NES exit failed to restore the shell", key, raw[-3000:])
            foreground = False
            out = clean(raw)
            assert re.search(r"\d+ frames in \d+ s:", out), out
            assert "drawn" in out and "nothing was drawn" not in out, out
            print(out, flush=True)
            assert "NES_EXIT_OK" in b.run("echo NES_EXIT_OK"), "TTY not restored"
        print("after:", b.run("free; cat /proc/uptime"), flush=True)
        print("NES device: silent SIGKILL/restart, q and Ctrl-C passed", flush=True)
    finally:
        if foreground:
            b.send(b"\x03")
            b.read_until(PROMPT, 5)
        b.run(f"rm -f {scratch}")
        b.close()


if __name__ == "__main__":
    main()
