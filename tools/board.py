#!/usr/bin/env python3
"""Talking to a PocketType board over a byte stream.

The serial port of a real board, or QEMU's TCP serial port: the same
shell either way. Used by the file transfer here and by the test suites
in pico-os/tools/, which is why it lives on its own.
"""
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import time


def find_qemu():
    candidates = [os.environ.get("QEMU"), shutil.which("qemu-system-xtensa")]
    candidates += sorted(glob.glob(os.path.expanduser(
        "~/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa")), reverse=True)
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise SystemExit("qemu-system-xtensa not found; see the note at the top of this file")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78c]")
PROMPT = rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$"


def clean(raw: bytes) -> str:
    text = ANSI.sub(b"", raw).replace(b"\r\n", b"\n")
    lines = []
    for line in text.split(b"\n"):
        # a bare \r means the line editor redrew: keep what was drawn last
        lines.append(line.split(b"\r")[-1])
    return "\n".join(l.decode("utf-8", "replace") for l in lines)


class Board:
    """The shell over a byte stream: QEMU's TCP serial port or a real one."""

    def __init__(self, target):
        self.transcript = b""
        self.pending = b""
        self.proc = self.sock = self.ser = None
        if target.startswith("/dev/"):
            import serial
            self.ser = serial.Serial()
            self.ser.port = target
            # the COM-port console's speed (sdkconfig.defaults); the native
            # USB port ignores it
            self.ser.baudrate = 460800
            self.ser.timeout = 0.2
            # On the native USB port RTS and DTR drive the chip's reset and
            # boot pins. pyserial lowers DTR before RTS on open, which is the
            # reset sequence; claiming hardware flow control skips that and
            # leaves the board running.
            self.ser.rtscts = True
            self.ser.dsrdtr = True
            self.ser.open()
            self.ser.rtscts = False
            self.ser.dsrdtr = False
            return
        port = free_port()
        self.log = open(os.path.join(os.path.dirname(target), "qemu.log"), "wb")
        self.proc = subprocess.Popen([
            find_qemu(), "-nographic", "-machine", "esp32s3", "-m", "8M",
            "-drive", f"file={target},if=mtd,format=raw",
            "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
            "-serial", f"tcp:127.0.0.1:{port},server,nowait", "-monitor", "none",
        ], stdout=self.log, stderr=self.log)
        for _ in range(100):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit("qemu did not open the serial port")
        self.sock.settimeout(0.2)

    def recv(self):
        """Some bytes, b"" when the line was quiet, None when it closed."""
        if self.pending:
            got, self.pending = self.pending, b""
            return got
        if self.ser:
            return self.ser.read(4096)
        try:
            chunk = self.sock.recv(4096)
            return chunk if chunk else None
        except socket.timeout:
            return b""

    def read_until(self, pattern: bytes, timeout: float) -> bytes:
        """Read until `pattern` ends the output and the line has been quiet
        for a moment: a redraw can pause right after the prompt text."""
        got = b""
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.recv()
            if chunk is None:
                break
            if chunk:
                got += chunk
            elif got and re.search(pattern, got):
                break
        self.transcript += got
        return got

    def send(self, data: bytes):
        for i in range(0, len(data), 16):
            if self.ser:
                self.ser.write(data[i:i + 16])
            else:
                self.sock.sendall(data[i:i + 16])
            time.sleep(0.02)
            # The device redraws the editable command after every key. Long
            # commands can fill the PC's USB receive queue before read_until
            # starts, causing the bounded firmware writer to drop output.
            if self.ser and self.ser.in_waiting:
                self.pending += self.ser.read(self.ser.in_waiting)

    def wait_prompt(self, timeout: float = 60.0) -> bytes:
        """A fresh QEMU RAM disk starts in setup. Leave its defaults in place
        before the suites start; a real board's setup is left to its user."""
        if self.ser:
            return self.read_until(PROMPT, timeout)
        name_prompt = rb"Name \[[a-z_][a-z0-9_-]*\]: $"
        deadline = time.monotonic() + timeout
        got = self.read_until(rb"(?:" + PROMPT + rb"|" + name_prompt + rb")", timeout)
        if re.search(name_prompt, got):
            self.send(b"\x03")
            got += self.read_until(PROMPT, max(0.0, deadline - time.monotonic()))
        return got

    def run(self, cmd: str, timeout: float = 8.0) -> str:
        self.send(cmd.encode() + b"\r")
        out = self.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", timeout)
        self.last_raw = out
        return clean(out)

    def close(self):
        if self.ser:
            self.ser.close()
        else:
            self.proc.kill()
            self.log.close()
