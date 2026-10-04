#!/usr/bin/env python3
"""Decode WAV files of every sample format play takes, and refuse the rest.

8-bit unsigned, 16, 24 and 32-bit integers, 32-bit float and the
extensible header, mono and stereo, each checked sample by sample as the
24 bits the decoder hands out, under ASan/UBSan.
"""
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FULL = 1 << 23


def wav(channels, bits, data, fmt=1, extensible=False):
    width = (bits + 7) // 8
    body = struct.pack("<HHIIHH", 0xfffe if extensible else fmt, channels, 44100,
                       44100 * channels * width, channels * width, bits)
    if extensible:
        body += struct.pack("<HHI", 22, bits, 3 if channels == 2 else 4)
        body += struct.pack("<H", fmt) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
    chunks = b"fmt " + struct.pack("<I", len(body)) + body
    chunks += b"LIST" + struct.pack("<I", 3) + b"abc\x00"     # odd length, padded
    chunks += b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks


def main():
    values = [0, 1, -1, 1000, -1000, FULL - 1, -FULL, 4321, -4321, 77, -77, 300000, -300000]
    cases = []
    for channels in (1, 2):
        n = len(values) - len(values) % channels
        v = values[:n]
        cases += [
            ("8-bit", channels, 8, bytes((x >> 16) + 128 for x in v), [x >> 16 << 16 for x in v], {}),
            ("16-bit", channels, 16, b"".join(struct.pack("<h", x >> 8) for x in v),
             [x >> 8 << 8 for x in v], {}),
            ("24-bit", channels, 24, b"".join(struct.pack("<i", x)[:3] for x in v), v, {}),
            ("32-bit", channels, 32, b"".join(struct.pack("<i", x * 256 + 99) for x in v), v, {}),
            ("float", channels, 32, b"".join(struct.pack("<f", x / FULL) for x in v), v,
             {"fmt": 3}),
            ("extensible 24-bit", channels, 24, b"".join(struct.pack("<i", x)[:3] for x in v),
             v, {"extensible": True}),
        ]
    over = [2.5, -3.0, 1000.0, float("nan")]
    cases.append(("float over full scale, and NaN", 1, 32,
                  b"".join(struct.pack("<f", x) for x in over),
                  [int(2.5 * FULL), int(-3.0 * FULL), 1 << 28, 0], {"fmt": 3}))
    with tempfile.TemporaryDirectory(prefix="pico-wav-test-") as tmp:
        exe = str(Path(tmp) / "wav_test")
        subprocess.run([
            "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(ROOT / "kernel/include"), str(ROOT / "tools/wav_test.c"), "-o", exe, "-lm",
        ], check=True)
        for name, channels, bits, data, want, kw in cases:
            path = Path(tmp) / "t.wav"
            path.write_bytes(wav(channels, bits, data, **kw))
            subprocess.run([exe, str(path), str(channels), str(bits), *map(str, want)],
                           check=True, timeout=10)
        for name, data in (("ADPCM", wav(1, 4, b"\x00" * 8, fmt=0x11)),
                           ("64-bit float", wav(1, 64, b"\x00" * 16, fmt=3)),
                           ("three channels", wav(3, 16, b"\x00" * 12))):
            path = Path(tmp) / "t.wav"
            path.write_bytes(data)
            subprocess.run([exe, str(path), "0", "bad"], check=True, timeout=10)
            cases.append((name, 0, 0, b"", [], {}))
    print(f"wav: {len(cases)} files, every sample format and the refused ones (ASan/UBSan)")


if __name__ == "__main__":
    main()
