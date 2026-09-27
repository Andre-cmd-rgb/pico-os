#!/usr/bin/env python3
"""Set a PocketType's clock from the PC's.

    python3 tools/settime.py PORT [--boot]

`make time` runs it, and `make flash` after flashing, with --boot to wait
for the board to come up first. The time goes over as seconds since 1970
(`date -s @SECONDS`), so the time zones of the two do not matter, and the
digits are sent last, in one write, with the moment they will arrive in
them: the board is right to a few hundredths of a second.
"""
import sys
import time

from board import Board

PROMPT = rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$"


def wait_for_shell(b, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if "shell-up" in b.run("echo shell-up", timeout=2):
            return True
    return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    b = Board(sys.argv[1])
    try:
        if not wait_for_shell(b, 20 if "--boot" in sys.argv else 5):
            print("settime: the board did not answer; its clock is as it was")
            return 0
        b.send(b"date -s @")
        at = time.time() + 0.01	# a write of 20 bytes, and the shell's work
        b.ser.write(f"{at:.3f}\r".encode())
        b.read_until(PROMPT, 5)
        # the kernel may log a line in the middle of the answer: the
        # time is the line that is nothing but a number of seconds
        answer = b.run("date +%s").split("\n")
        board = [int(l) for l in map(str.strip, answer) if l.isdigit() and len(l) >= 9]
        if board:
            print(f"board clock set; it is {board[-1] - int(time.time()):+d} s from the PC's")
        else:
            print("board clock set")
    finally:
        b.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
