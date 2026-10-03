#!/usr/bin/env python3
"""Decode independent FLAC fixtures and reject malformed headers/predictors.

Uses eight concurrent readers of the actual decoder under ASan/UBSan;
no audio, board or encoder needed.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def crc(data, width, polynomial):
    value = 0
    mask = (1 << width) - 1
    for byte in data:
        value ^= byte << (width - 8)
        for _ in range(8):
            value = ((value << 1) ^ polynomial if value & (1 << (width - 1))
                     else value << 1) & mask
    return value


class Bits:
    def __init__(self):
        self.bits = ""

    def add(self, value, width):
        self.bits += f"{value & ((1 << width) - 1):0{width}b}"
        return self

    def bytes(self):
        text = self.bits + "0" * (-len(self.bits) % 8)
        return bytes(int(text[i:i + 8], 2) for i in range(0, len(text), 8))


def info(block=16, frames=16, bps=16):
    packed = (16000 << 44) | ((bps - 1) << 36) | frames
    return (block.to_bytes(2, "big") * 2 + bytes(6) + packed.to_bytes(8, "big")
            + bytes(16))


def metadata(kind, payload, last):
    return bytes([kind | (128 if last else 0)]) + len(payload).to_bytes(3, "big") + payload


def frame(block, subframe):
    # Variable block length in the header, mono, 16-bit, STREAMINFO's rate.
    header = bytes([0xff, 0xf8, 0x60, 0x00, 0, block - 1])
    data = header + bytes([crc(header, 8, 0x07)]) + subframe.bytes()
    return data + crc(data, 16, 0x8005).to_bytes(2, "big")


def constant(value, bps=16):
    return Bits().add(0, 8).add(value, bps)


def predictor(order, block, lpc=False, bps=16, value=10):
    bits = Bits().add((31 + order if lpc else 8 + order) << 1, 8)
    for _ in range(order):
        bits.add(value, bps)
    if lpc:
        # Two-bit coefficients, shift zero, s[i] = s[i-1].
        bits.add(1, 4).add(0, 5)
        for n in range(order):
            bits.add(1 if n == 0 else 0, 2)
    bits.add(0, 2).add(0, 4).add(0, 4)  # Rice method/partition/parameter zero.
    for _ in range(max(0, block - order)):
        bits.add(1, 1)                 # Zero residual = unary terminator.
    return bits


def main():
    stream = b"fLaC" + metadata(0, info(), True)
    cases = [
        ("constant", stream + frame(16, constant(-1234)), -1234, 16, "ok"),
        ("fixed", stream + frame(16, predictor(1, 16)), 10, 16, "ok"),
        ("lpc", stream + frame(16, predictor(2, 16, True)), 10, 16, "ok"),
        ("8-bit-negative", b"fLaC" + metadata(0, info(bps=8), True)
         + frame(16, constant(-128, 8)), -32768, 16, "ok"),
        ("32-bit-fixed", b"fLaC" + metadata(0, info(bps=32), True)
         + frame(16, predictor(2, 16, bps=32, value=2147483647)), 32767, 16, "ok"),
        ("large-frame-count", b"fLaC" + metadata(0, info(frames=0x80000000), True)
         + frame(16, constant(1)), 1, 16, "ok"),
        ("missing-streaminfo", b"fLaC" + metadata(1, b"", True), 0, 0, "open-error"),
        ("streaminfo-not-first", b"fLaC" + metadata(1, b"", False)
         + metadata(0, info(), True), 0, 0, "open-error"),
        ("duplicate-streaminfo", b"fLaC" + metadata(0, info(), False)
         + metadata(0, info(), True), 0, 0, "open-error"),
        ("oversized-streaminfo", b"fLaC" + metadata(0, info() + b"x", True),
         0, 0, "open-error"),
        ("truncated-streaminfo", stream[:-1], 0, 0, "open-error"),
        ("truncated-skipped-metadata", b"fLaC" + metadata(0, info(), False)
         + metadata(1, b"abc", True)[:-1], 0, 0, "open-error"),
        ("fixed-order-past-block", stream + frame(1, predictor(4, 1)),
         0, 0, "read-error"),
        ("lpc-order-past-allocation", stream + frame(16, predictor(32, 16, True)),
         0, 0, "read-error"),
    ]
    with tempfile.TemporaryDirectory(prefix="pico-flac-test-") as tmp:
        exe = str(Path(tmp) / "flac_test")
        subprocess.run([
            "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(ROOT / "kernel/include"), str(ROOT / "tools/flac_test.c"),
            "-o", exe,
        ], check=True)
        for name, data, value, frames, result in cases:
            path = Path(tmp) / (name + ".flac")
            path.write_bytes(data)
            subprocess.run([exe, str(path), str(value), str(frames), result],
                           check=True, timeout=10)
            print("PASS", name)
    print(f"flac: {len(cases)} fixtures passed with eight concurrent readers (ASan/UBSan)")


if __name__ == "__main__":
    main()
