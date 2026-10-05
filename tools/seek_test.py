#!/usr/bin/env python3
"""codec_seek() in every decoder, back and forth on one open file.

WAV and FLAC must land on the very frame asked for and go on exactly as a
straight decode does. MP3 frames carry no numbers, so there it is measured:
minimp3 is deterministic, and a few frames after a seek its output is the
straight decode's again, byte for byte, so finding it there says where the
seek really landed -- which must be within a quarter of a second of where
codec_seek() said. Real files from ffmpeg (CBR, VBR with a Xing table, a
big cover in the ID3 tag, a big block before a FLAC's frames) and a FLAC
built by hand whose frames are numbered. ASan/UBSan, no board.
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from flac_test import constant, crc, info, metadata  # noqa: E402

AFTER = 8192
MP3_FRAME = 1152
MP3_SLACK = 0.25        # seconds codec_seek() may be off by in an MP3


def build(tmp):
    exe = tmp / "seek_test"
    flags = ["-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-I", str(ROOT / "third_party/minimp3"),
             "-I", str(ROOT / "kernel/include"), "-I", str(ROOT / "codec")]
    subprocess.run(["cc", *flags, "-w", "-c", str(ROOT / "third_party/minimp3/decoders.c"),
                    "-o", str(tmp / "minimp3.o")], check=True)
    subprocess.run(["cc", *flags, "-Wall", "-Wextra", "-Werror", str(ROOT / "tools/seek_test.c"),
                    *(str(ROOT / "codec" / f) for f in ("codec.c", "wav.c", "flac.c", "mp3.c")),
                    str(tmp / "minimp3.o"), "-lm", "-o", str(exe)], check=True)
    return str(exe)


def run(exe, *args):
    out = subprocess.run([exe, *map(str, args)], check=True, capture_output=True, text=True,
                         timeout=120).stdout.split()
    return int(out[0]), int(out[1]), int(out[2])


def dump(exe, path, tmp):
    raw = tmp / "dump.raw"
    rate, channels, frames = run(exe, "dump", path, raw)
    return rate, channels, frames, raw.read_bytes()


def seeks(exe, path, tmp, targets):
    raw = tmp / "seeks.raw"
    run(exe, path, raw, *targets)
    data, pos, out = raw.read_bytes(), 0, []
    for _ in targets:
        at, n = struct.unpack_from("<qi", data, pos)
        pos += 12
        out.append((at, n, pos))
        pos += n * 4 * channels_of[path]
    return out, data


channels_of = {}


def exact(exe, path, tmp, name):
    """Lands where asked, and reads on exactly as the straight decode."""
    rate, channels, frames, ref = dump(exe, path, tmp)
    channels_of[path] = channels
    total = len(ref) // (4 * channels)
    assert frames in (0, total), f"{name}: says {frames} frames, has {total}"
    targets = [0, 1, 4607, 4608, rate * 2 + 123, total // 2, rate, total - 1, total,
               total + 1000, 3, total // 3]
    results, data = seeks(exe, path, tmp, targets)
    for target, (at, n, pos) in zip(targets, results):
        want = min(target, total)
        assert at == want, f"{name}: seek to {target} says {at}"
        assert n == min(AFTER, total - want), f"{name}: {n} frames read after {target}"
        size = n * 4 * channels
        assert data[pos:pos + size] == ref[want * 4 * channels:want * 4 * channels + size], \
            f"{name}: the frames after {target} differ"
    print(f"PASS {name}: {len(targets)} seeks exact, {total} frames")


def measured(exe, path, tmp, name, check_total=True):
    """Where an MP3 seek really lands, found in the straight decode."""
    rate, channels, frames, ref = dump(exe, path, tmp)
    channels_of[path] = channels
    total = len(ref) // (4 * channels)
    if check_total:
        assert abs(frames - total) <= 3 * MP3_FRAME, f"{name}: Xing says {frames}, has {total}"
    step = MP3_FRAME * 4 * channels
    targets = [rate * s for s in (1, 7, 3, 12, 15, 5)] + [0]
    results, data = seeks(exe, path, tmp, targets)
    worst = 0.0
    for target, (at, n, pos) in zip(targets, results):
        assert abs(at - target) <= 1, f"{name}: seek to {target} says {at}"
        assert n == AFTER, f"{name}: only {n} frames after {target}"
        probe = data[pos + 4 * step:pos + 5 * step]     # past the frames with no history
        found = ref.find(probe)
        assert found >= 0 and found % (4 * channels) == 0, f"{name}: lost after {target}"
        landed = found // (4 * channels) - 4 * MP3_FRAME
        off = abs(landed - at) / rate
        worst = max(worst, off)
        assert off <= MP3_SLACK, f"{name}: seek to {target} landed at {landed}, {off:.2f} s out"
    print(f"PASS {name}: {len(targets)} seeks within {worst:.3f} s")


def numbered_flac(path, frames=300, block=16):
    """Hand-made: frame k holds the constant k, and its header says k."""
    def frame(k):
        number = bytes([k]) if k < 0x80 else bytes([0xc0 | k >> 6, 0x80 | (k & 0x3f)])
        header = bytes([0xff, 0xf8, 0x60, 0x00]) + number + bytes([block - 1])
        data = header + bytes([crc(header, 8, 0x07)]) + constant(k).bytes()
        return data + crc(data, 16, 0x8005).to_bytes(2, "big")
    path.write_bytes(b"fLaC" + metadata(0, info(block=block, frames=frames * block), True)
                     + b"".join(frame(k) for k in range(frames)))


def padded(src, dst, size=300_000):
    """The same FLAC with a big PADDING block before its frames."""
    data = src.read_bytes()
    pos, last = 4, False
    while not last:
        last = bool(data[pos] & 0x80)
        if last:
            data = data[:pos] + bytes([data[pos] & 0x7f]) + data[pos + 1:]
        pos += 4 + int.from_bytes(data[pos + 1:pos + 4], "big")
    dst.write_bytes(data[:pos] + metadata(1, bytes(size), True) + data[pos:])


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *map(str, args)], check=True)


def main():
    music = ("aevalsrc=0.4*sin(2*PI*440*t)+0.2*sin(2*PI*(300+40*t)*t)+0.2*(random(0)-0.5)"
             "|0.4*sin(2*PI*660*t)+0.2*(random(1)-0.5):s=44100:d=20")
    with tempfile.TemporaryDirectory(prefix="pico-seek-test-") as tmp:
        tmp = Path(tmp)
        exe = build(tmp)

        hand = tmp / "numbered.flac"
        numbered_flac(hand)
        rate, channels, frames, ref = dump(exe, hand, tmp)
        channels_of[hand] = channels
        targets = [0, 15, 16, 17, 1000, 4799, 4800, 9999, 33, 2400]
        results, data = seeks(exe, hand, tmp, targets)
        for target, (at, n, pos) in zip(targets, results):
            want = min(target, 4800)
            assert at == want and n == min(AFTER, 4800 - want), (target, at, n)
            if n:
                assert struct.unpack_from("<i", data, pos)[0] == (want // 16) * 256, target
        print(f"PASS numbered frames: {len(targets)} seeks land on the right sample")

        for name, args in (("flac 16-bit", ["-sample_fmt", "s16", "-compression_level", "5"]),
                           ("flac 24-bit", ["-sample_fmt", "s32", "-bits_per_raw_sample", "24",
                                            "-compression_level", "8"])):
            path = tmp / (name.replace(" ", "-") + ".flac")
            ffmpeg("-f", "lavfi", "-i", music, *args, path)
            exact(exe, path, tmp, name)
        big = tmp / "padded.flac"
        padded(tmp / "flac-16-bit.flac", big)
        exact(exe, big, tmp, "flac after a 300 KB block")

        wav = tmp / "s24.wav"
        ffmpeg("-f", "lavfi", "-i", music, "-c:a", "pcm_s24le", wav)
        exact(exe, wav, tmp, "wav 24-bit")

        cover = tmp / "cover.jpg"
        ffmpeg("-f", "lavfi", "-i", "testsrc2=s=1400x1400:d=1", "-frames:v", "1", "-q:v", "2",
               cover)
        for name, args, check_total in (
                ("mp3 CBR, no Xing", ["-b:a", "128k", "-write_xing", "0"], False),
                ("mp3 CBR with Info", ["-b:a", "192k"], True),
                ("mp3 VBR with Xing", ["-q:a", "4"], True)):
            path = tmp / (name.split(",")[0].replace(" ", "-") + ".mp3")
            ffmpeg("-f", "lavfi", "-i", music, "-c:a", "libmp3lame", *args, path)
            measured(exe, path, tmp, name, check_total)
        tagged = tmp / "cover.mp3"
        ffmpeg("-f", "lavfi", "-i", music, "-i", cover, "-map", "0:a", "-map", "1:v",
               "-c:a", "libmp3lame", "-q:a", "2", "-c:v", "copy", "-id3v2_version", "3",
               "-metadata:s:v", "comment=Cover (front)", tagged)
        assert tagged.stat().st_size > cover.stat().st_size
        measured(exe, tagged, tmp, f"mp3 after a {cover.stat().st_size // 1024} KB cover")
    print("seek: WAV and FLAC exact, MP3 measured (ASan/UBSan)")


if __name__ == "__main__":
    main()
