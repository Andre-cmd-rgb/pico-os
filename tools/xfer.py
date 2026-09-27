#!/usr/bin/env python3
"""Move files between the PC and a running PocketType over its serial port.

    make push FILE=hello.pico               -> ~/hello.pico on the board
    make push FILE=hello.pico DEST=/tmp/x.pico
    make pull FILE=/tmp/x.pico [DEST=x.pico]

It goes beside the console, not through it: nothing is typed on the board
and nothing waits there for it, so the shell on the first terminal stays
free and the screen shows nothing. Every line of the transfer starts with
RS (0x1e); the board hands such lines to its transfer code
(pico-os/drivers/misc/xfer.c has the whole of the protocol) and marks its
answers the same way, and they are picked out of whatever the console is
printing meanwhile.

The file goes in numbered chunks of base64, each with a CRC-32; a chunk
that is not answered in time, or arrives damaged, is sent again. The board writes to DEST.part and puts it in place only once
the whole file's CRC matches; the PC checks length and CRC before it writes
anything.

With no DEST, `push` puts the file in the board's home directory, and so
does a DEST that does not start with /; ~/ is the home directory too.
"""
import base64
import binascii
import os
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board import Board  # noqa: E402

PROMPT = rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$"
RS = b"\x1e"
CHUNK = 512             # bytes a chunk, as the board takes them
# Chunks in flight at once. Going to the board, one: its USB port holds
# about 1 KB of what arrives, and while it writes to the card whatever
# does not fit is lost -- eight at a time lost three in six. Coming back
# it is only the short requests that go to it.
PUSH_WINDOW = 1
PULL_WINDOW = 8
WAIT = 1.5              # seconds without an answer before sending again
TRIES = 20


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


class Replies:
    """The board's marked lines, picked out of what the console prints."""

    def __init__(self, board):
        self.b = board
        self.buf = b""

    def _read(self):
        """Bytes available now, without waiting to fill a big buffer.

        pyserial's read(4096) keeps blocking until it has 4096 bytes or the
        timeout expires, so a short answer would cost the whole timeout.
        Reading one byte and then whatever is queued returns as soon as data
        is there.
        """
        if self.b.ser:
            first = self.b.ser.read(1)
            if not first:
                return b""
            n = self.b.ser.in_waiting or 0
            return first + (self.b.ser.read(n) if n else b"")
        return self.b.recv() or b""

    def get(self, timeout):
        """The next marked line, without its mark, or None after `timeout`."""
        end = time.time() + timeout
        while True:
            i = self.buf.find(RS)
            if i < 0:
                self.buf = b""                  # the console's, not ours
            else:
                j = self.buf.find(b"\n", i)
                if j >= 0:
                    line = self.buf[i + 1:j].rstrip(b"\r")
                    self.buf = self.buf[j + 1:]
                    return line
                self.buf = self.buf[i:]
            if time.time() >= end:
                return None
            self.buf += self._read()


def send_raw(b, data):
    if b.ser:
        b.ser.write(data)
    else:
        b.sock.sendall(data)


def send_line(b, text):
    send_raw(b, RS + text + b"\n")


def ask(b, r, text, *answers, tries=3, timeout=3.0):
    """Sends `text` until an answer starting with one of `answers` comes."""
    for _ in range(tries):
        send_line(b, text)
        end = time.time() + timeout
        while time.time() < end:
            line = r.get(max(0.0, end - time.time()))
            if line and line.startswith(answers):
                return line
    return None


def chunk_line(data, k):
    piece = data[k * CHUNK:(k + 1) * CHUNK]
    return b"D %d %08x %s" % (k, crc32(piece), base64.b64encode(piece))


def push(b, local, remote, shown):
    data = open(local, "rb").read()
    progress = sys.stdout.isatty()
    r = Replies(b)
    got = ask(b, r, b"PUT %d %08x %s" % (len(data), crc32(data), remote.encode()), b"RDY", b"ERR")
    if not got or got.startswith(b"ERR"):
        print("push failed:", got.decode(errors="replace") if got else "the board does not answer")
        return False

    count = -(-len(data) // CHUNK)
    done, sent, tries, resent = set(), {}, {}, 0
    following = 0
    while len(done) < count:
        now = time.time()
        waiting = [k for k in sent if k not in done]
        for k in waiting:
            if now - sent[k] > WAIT:            # no answer: again
                tries[k] = tries.get(k, 0) + 1
                if tries[k] > TRIES:
                    print(f"\npush failed: no answer from the board for chunk {k}")
                    return False
                send_line(b, chunk_line(data, k))
                sent[k] = now
                resent += 1
        while len(waiting) < PUSH_WINDOW and following < count:
            send_line(b, chunk_line(data, following))
            sent[following] = now
            waiting.append(following)
            following += 1
        line = r.get(0.2)
        if not line:
            continue
        if line.startswith(b"ok "):
            done.add(int(line[3:]))
            if progress:
                print(f"\r{min(len(done) * CHUNK, len(data))}/{len(data)} bytes", end="", flush=True)
        elif line.startswith(b"re "):
            k = int(line[3:])
            send_line(b, chunk_line(data, k))
            sent[k] = time.time()
            resent += 1
        elif line.startswith(b"ERR"):
            print("\npush failed:", line.decode(errors="replace"))
            return False
    got = ask(b, r, b"END", b"OK", b"ERR", timeout=10.0)
    if progress:
        print()
    if not got or not got.startswith(b"OK"):
        print("push failed:", got.decode(errors="replace") if got else "no answer from the board")
        return False
    print(f"pushed {len(data)} bytes to {shown}" + (f" ({resent} chunks sent again)" if resent else ""))
    return True


def pull(b, remote, local, shown):
    progress = sys.stdout.isatty()
    r = Replies(b)
    got = ask(b, r, b"GET " + remote.encode(), b"SIZE", b"ERR", timeout=10.0)
    if not got or got.startswith(b"ERR"):
        print("pull failed:", got.decode(errors="replace") if got else "the board does not answer")
        return False
    size, crc = got.split()[1:3]
    size, crc = int(size), int(crc, 16)

    count = -(-size // CHUNK)
    pieces, asked, tries = {}, {}, {}
    following = 0
    while len(pieces) < count:
        now = time.time()
        waiting = [k for k in asked if k not in pieces]
        for k in waiting:
            if now - asked[k] > WAIT:
                tries[k] = tries.get(k, 0) + 1
                if tries[k] > TRIES:
                    print(f"\npull failed: the board stopped sending (chunk {k})")
                    return False
                send_line(b, b"R %d" % k)
                asked[k] = now
        while len(waiting) < PULL_WINDOW and following < count:
            send_line(b, b"R %d" % following)
            asked[following] = now
            waiting.append(following)
            following += 1
        line = r.get(0.2)
        if not line:
            continue
        if line.startswith(b"ERR"):
            print("\npull failed:", line.decode(errors="replace"))
            return False
        part = line.split(b" ", 3)
        if len(part) != 4 or part[0] != b"D":
            continue
        try:
            k, piece = int(part[1]), base64.b64decode(part[3], validate=True)
        except (ValueError, binascii.Error):
            continue                            # damaged: asked for again
        if crc32(piece) == int(part[2], 16) and k < count:
            pieces[k] = piece
            if progress:
                print(f"\r{min(len(pieces) * CHUNK, size)}/{size} bytes", end="", flush=True)
    send_line(b, b"BYE")
    if progress:
        print()
    data = b"".join(pieces[k] for k in range(count))
    if len(data) != size or crc32(data) != crc:
        print("pull failed: the pieces arrived but do not make the file; try again")
        return False
    with open(local, "wb") as f:
        f.write(data)
    print(f"pulled {len(data)} bytes from {shown} to {local}")
    return True


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("push", "pull") or not sys.argv[3]:
        print(__doc__)
        return 2
    action, port, src = sys.argv[1], sys.argv[2], sys.argv[3]
    dest = sys.argv[4] if len(sys.argv) > 4 else ""
    if not port:
        print("no serial port found: plug the board in or pass PORT=/dev/ttyACM0")
        return 2
    if action == "push":
        if not os.path.isfile(src):
            print(f"push: {src}: no such file")
            return 1
        dest = dest or "~/" + os.path.basename(src)
        remote, shown = dest, dest
    else:
        remote, shown = src, src
        dest = dest or os.path.basename(src.rstrip("/"))

    board = Board(port)
    try:
        if action == "push":
            ok = push(board, src, remote, shown)
        else:
            ok = pull(board, remote, dest, shown)
    finally:
        board.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
