#!/usr/bin/env python3
"""Sample real-board playback from the start, keeping its transfer statistics."""
import argparse
import re
import shlex
import uuid

from board import Board, PROMPT, clean

Q_AT_PROMPT = rb"\$ q(\x1b\[K)?(\r\x1b\[\d+C)?$"
PROMPT_OR_Q = PROMPT + rb"|" + Q_AT_PROMPT


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("port")
    p.add_argument("clip")
    p.add_argument("--seconds", type=float, default=20)
    args = p.parse_args()
    if args.seconds <= 0:
        p.error("--seconds must be positive")
    b = Board(args.port)
    stats = "/tmp/video-test-" + uuid.uuid4().hex[:8]
    playing = False
    try:
        b.send(b"\x15\r")
        b.read_until(PROMPT, 5)
        assert "VIDEO_TEST_READY" in b.run("echo VIDEO_TEST_READY")
        clip = ('"$HOME"/' + shlex.quote(args.clip[2:]) if args.clip.startswith("~/")
                else shlex.quote(args.clip))
        b.send(f"video -v {clip} > {stats} 2>&1\r".encode())
        playing = True
        stopped = b.read_until(PROMPT, 1)
        if not re.search(PROMPT, stopped):
            b.send(b"0")
            stopped = b.read_until(PROMPT, args.seconds)
        if not re.search(PROMPT, stopped):
            b.send(b"q")
            stopped = b.read_until(PROMPT_OR_Q, 15)
            if re.search(Q_AT_PROMPT, stopped):
                # the clip had ended just before: the q went to the shell
                b.send(b"\x15")
                stopped = b.read_until(PROMPT, 5)
        assert re.search(PROMPT, stopped), "playback did not stop"
        playing = False
        out = b.run(f"cat {stats}")
        print(out)
        match = re.search(r"(\d+)x(\d+) at (\d+) fps: (\d+) frames shown, (\d+) dropped", out)
        assert match, "playback did not report frame statistics"
        shown, dropped = map(int, match.groups()[-2:])
        print(f"drop rate: {100 * dropped / max(1, shown + dropped):.2f}%")
        assert shown > 0, "no frames reached the panel"
    finally:
        if playing:
            b.send(b"\x03")
            print(clean(b.read_until(PROMPT, 5)))
        b.run(f"rm -f {stats}")
        b.close()


if __name__ == "__main__":
    main()
