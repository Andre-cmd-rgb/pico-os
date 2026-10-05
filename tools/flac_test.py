#!/usr/bin/env python3
"""Decode independent FLAC fixtures and reject malformed headers/predictors.

Uses eight concurrent readers of the actual decoder under ASan/UBSan;
no audio, board or encoder needed. Everything is decoded twice: also with
a read buffer of a few bytes, so that the bit reader's refills fall on
every kind of field, check sum and frame boundary.
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


def frame(block, subframe, damaged=None):
    # Variable block length in the header, mono, 16-bit, STREAMINFO's rate.
    header = bytes([0xff, 0xf8, 0x60, 0x00, 0, block - 1])
    data = header + bytes([crc(header, 8, 0x07) ^ (damaged == "crc8")]) + subframe.bytes()
    return data + (crc(data, 16, 0x8005) ^ (damaged == "crc16")).to_bytes(2, "big")


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


def rice(value, param, block=16, wide=False):
    """Fixed order 0, every residual `value`, Rice coded with `param`
    (five-bit parameters when wide)."""
    bits = Bits().add(8 << 1, 8).add(wide, 2).add(0, 4).add(param, 5 if wide else 4)
    folded = 2 * value if value >= 0 else -2 * value - 1
    for _ in range(block):
        bits.bits += "0" * (folded >> param) + "1"
        if param:
            bits.add(folded, param)
    return bits


def escaped(value, width, block=16):
    """Fixed order 0, the residual written out plainly in `width` bits."""
    bits = Bits().add(8 << 1, 8).add(0, 2).add(0, 4).add(15, 4).add(width, 5)
    for _ in range(block):
        bits.add(value, width)
    return bits


def wasted(value, shift, bps=16):
    """A constant subframe whose low `shift` bits the encoder dropped."""
    bits = Bits().add(1, 8)
    bits.bits += "0" * (shift - 1) + "1"
    return bits.add(value, bps - shift)


def main():
    stream = b"fLaC" + metadata(0, info(), True)
    cases = [
        # samples come out as 24 bits: a 16-bit file's shifted up by 8
        ("constant", stream + frame(16, constant(-1234)), -1234 * 256, 16, "ok"),
        ("fixed", stream + frame(16, predictor(1, 16)), 10 * 256, 16, "ok"),
        ("lpc", stream + frame(16, predictor(2, 16, True)), 10 * 256, 16, "ok"),
        ("8-bit-negative", b"fLaC" + metadata(0, info(bps=8), True)
         + frame(16, constant(-128, 8)), -8388608, 16, "ok"),
        ("24-bit-as-it-is", b"fLaC" + metadata(0, info(bps=24), True)
         + frame(16, constant(-1234567, 24)), -1234567, 16, "ok"),
        ("24-bit-fixed", b"fLaC" + metadata(0, info(bps=24), True)
         + frame(16, predictor(2, 16, bps=24, value=8388607)), 8388607, 16, "ok"),
        ("32-bit-fixed", b"fLaC" + metadata(0, info(bps=32), True)
         + frame(16, predictor(2, 16, bps=32, value=2147483647)), 8388607, 16, "ok"),
        ("large-frame-count", b"fLaC" + metadata(0, info(frames=0x80000000), True)
         + frame(16, constant(1)), 256, 16, "ok"),
        # a run of zeros longer than the bit reader holds, each code
        ("rice-long-unary", stream + frame(16, rice(-300, 0)), -300 * 256, 16, "ok"),
        ("rice-parameter-14", stream + frame(16, rice(12345, 14)), 12345 * 256, 16, "ok"),
        ("rice-parameter-28", b"fLaC" + metadata(0, info(bps=32), True)
         + frame(16, rice(-0x12345678, 28, wide=True)), -0x12345678 >> 8, 16, "ok"),
        ("escaped-residual", stream + frame(16, escaped(-1234, 12)), -1234 * 256, 16, "ok"),
        ("wasted-bits", stream + frame(16, wasted(-5, 3)), -40 * 256, 16, "ok"),
        ("damaged-crc8", stream + frame(16, constant(7), "crc8"), 0, 0, "read-error"),
        ("damaged-crc16", stream + frame(16, constant(7), "crc16"), 0, 0, "read-error"),
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
        exes = []
        for name, flags in (("flac_test", []), ("flac_test_7", ["-DBUF_BYTES=7"])):
            exes.append(str(Path(tmp) / name))
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", *flags,
                "-I", str(ROOT / "kernel/include"), str(ROOT / "tools/flac_test.c"),
                "-o", exes[-1],
            ], check=True)
        for name, data, value, frames, result in cases:
            path = Path(tmp) / (name + ".flac")
            path.write_bytes(data)
            for exe in exes:
                subprocess.run([exe, str(path), str(value), str(frames), result],
                               check=True, timeout=10)
            print("PASS", name)
        encoded = against_ffmpeg(exes, Path(tmp))
    print(f"flac: {len(cases)} fixtures passed with eight concurrent readers (ASan/UBSan); "
          f"{encoded} files from ffmpeg's encoder decoded to the same 24 bits")


# Music-like: two tones, noise, and a quiet tail where only the low bits move.
TONES = ("aevalsrc=0.6*sin(2*PI*440*t)+0.2*sin(2*PI*3001*t)+0.05*(random(0)-0.5)"
         "|0.5*sin(2*PI*660*t)*lt(t\\,1)+0.00002*sin(2*PI*1000*t)")
# Two noises shared by both sides in three mixes, for which the encoder
# codes the pair as left and difference, difference and right, then
# average and difference: random(0) gives each side the same sequence.
PAIRS = ("aevalsrc=0.3*(random(0)-0.5)+(0.1*between(t\\,0.5\\,1)+0.15*gt(t\\,1))*(random(0)-0.5)"
         "|0.3*(random(0)-0.5)+(0.1*lt(t\\,0.5)-0.15*gt(t\\,1))*(random(0)-0.5)")


def against_ffmpeg(exes, tmp):
    """Real encoder output -- the fixed predictors (level 0), LPC up to
    order 12, every stereo mode, 16 and 24 bits, CD to 192 kHz rates --
    checked sample for sample against ffmpeg's own decoder."""
    count = 0
    for rate, bits, level, sound in ((44100, 16, 0, TONES), (44100, 16, 5, TONES),
                                     (44100, 16, 12, TONES), (48000, 24, 0, TONES),
                                     (48000, 24, 12, TONES), (96000, 24, 5, TONES),
                                     (192000, 24, 12, TONES), (44100, 16, 5, PAIRS),
                                     (96000, 24, 5, PAIRS)):
        name = "pairs" if sound == PAIRS else "tones"
        src = tmp / f"{rate}-{bits}-{level}-{name}.flac"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                        f"{sound}:s={rate}:d=1.5",
                        "-sample_fmt", "s16" if bits == 16 else "s32",
                        "-bits_per_raw_sample", str(bits), "-compression_level", str(level),
                        str(src)], check=True)
        reference = subprocess.run(["ffmpeg", "-v", "error", "-i", str(src), "-f", "s32le",
                                    "-acodec", "pcm_s32le", "-"],
                                   check=True, capture_output=True).stdout
        want = [v >> 8 for v in memoryview(reference).cast("i")]
        for exe in exes:
            ours = tmp / "ours.raw"
            head = subprocess.run([exe, "dump", str(src), str(ours)], check=True,
                                  capture_output=True, text=True, timeout=60).stdout.split()
            assert head == [str(rate), "2", str(bits)], head
            got = list(memoryview(ours.read_bytes()).cast("i"))
            assert len(got) == len(want) and got == want, \
                f"{rate} Hz {bits}-bit level {level} {name} differs ({Path(exe).name})"
            assert any(v & 0xff for v in got) == (bits == 24)
        print(f"PASS ffmpeg {rate} Hz {bits}-bit level {level} {name}, {len(got) // 2} frames")
        count += 1
    return count


if __name__ == "__main__":
    main()
