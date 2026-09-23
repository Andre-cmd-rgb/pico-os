#!/usr/bin/env python3
"""The JPEG decoder (bin/jpeg.c), checked on the PC against libjpeg.

    python3 tools/jpeg_test.py            (part of make hosttest)

Pictures are made with ffmpeg and encoded with cjpeg in every shape the
decoder claims to take: 4:2:0, 4:2:2, 4:4:0, 4:4:4 and greyscale, odd
sizes, qualities from 20 to 100, restart markers, optimised Huffman
tables. Each is decoded here and by `djpeg -dct fast -nosmooth`, which uses
the same IDCT (AAN) and the same simple chroma upsampling, and the two must
agree to within one step of RGB565 -- the rounding differs slightly, the
pictures do not. The smaller scales are checked against
djpeg's own less closely: it averages where the decoder here samples.

Then the decoder is fed damaged and truncated copies under the address and
undefined-behaviour sanitizers: it may produce any picture at all, but it
must not read or write anywhere it should not.

Needs cc, ffmpeg, cjpeg and djpeg (libjpeg-turbo) on PATH.
"""
import math
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCES = [os.path.join(HERE, "jpeg_test.c"), os.path.join(ROOT, "bin", "jpeg.c")]

SHAPES = [                      # size, source, cjpeg options
    ("320x240", "testsrc2", ["-sample", "2x2"]),
    ("320x240", "mandelbrot", ["-sample", "2x2", "-quality", "95"]),
    ("321x239", "testsrc2", ["-sample", "2x2", "-quality", "50"]),
    ("333x77", "mandelbrot", ["-sample", "2x1"]),
    ("64x61", "testsrc2", ["-sample", "1x2"]),
    ("97x33", "testsrc2", ["-sample", "1x1", "-quality", "100"]),
    ("640x480", "mandelbrot", ["-sample", "2x2", "-restart", "1"]),
    ("321x239", "testsrc2", ["-sample", "2x2", "-restart", "3B"]),
    ("200x150", "testsrc2", ["-grayscale", "-quality", "20"]),
    ("16x16", "testsrc2", ["-sample", "2x2", "-optimize"]),
    ("7x5", "mandelbrot", ["-sample", "1x1"]),
]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def build(tmp, name, flags):
    exe = os.path.join(tmp, name)
    run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, "-o", exe, *SOURCES, "-lm"])
    return exe


def ppm(path):
    """Width, height and the RGB bytes of a binary PPM."""
    data = open(path, "rb").read()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        if data[at:at + 1] == b"#":
            at = data.index(b"\n", at)
            continue
        start = at
        while not data[at:at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    pixels = data[at + 1:]
    if fields[0] == b"P5":                      # greyscale: widen to RGB
        pixels = bytes(v for v in pixels for _ in range(3))
    return int(fields[1]), int(fields[2]), pixels


def to565(rgb):
    """RGB888 narrowed to RGB565 and widened back, as the decoder's output is."""
    out = bytearray(len(rgb))
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i] >> 3, rgb[i + 1] >> 2, rgb[i + 2] >> 3
        out[i] = r << 3 | r >> 2
        out[i + 1] = g << 2 | g >> 4
        out[i + 2] = b << 3 | b >> 2
    return bytes(out)


def shrink(rgb, w, h, scale):
    """A picture made 2^scale times smaller by averaging, as the decoder does."""
    if not scale:
        return rgb
    n = 1 << scale
    out = bytearray()
    for oy in range(-(-h // n)):
        for ox in range(-(-w // n)):
            for ch in range(3):
                total = count = 0
                for y in range(oy * n, min(oy * n + n, h)):
                    for x in range(ox * n, min(ox * n + n, w)):
                        total += rgb[(y * w + x) * 3 + ch]
                        count += 1
                out.append((total + count // 2) // count)
    return bytes(out)


def compare(a, b):
    """PSNR in dB, and the largest difference in any channel."""
    worst, total = 0, 0
    for x, y in zip(a, b):
        d = abs(x - y)
        total += d * d
        worst = max(worst, d)
    mse = total / max(len(a), 1)
    return (99.0 if not mse else 10 * math.log10(255 * 255 / mse)), worst


def ours(exe, path, scale):
    out = run([exe, path, str(scale)]).stdout
    head, _, rgb = out.partition(b"\n")
    w, h = map(int, head.split())
    return w, h, rgb


def main():
    for tool in ("cc", "ffmpeg", "cjpeg", "djpeg"):
        if not shutil.which(tool):
            raise SystemExit(f"jpeg_test: needs {tool}")
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        checked = build(tmp, "jpeg_test_asan", ["-O1", "-g", "-fsanitize=address,undefined",
                                                 "-fno-sanitize-recover=all"])
        fast = build(tmp, "jpeg_test", ["-O2"])
        pictures = []
        for n, (size, source, opts) in enumerate(SHAPES):
            src = os.path.join(tmp, f"s{n}.ppm")
            jpg = os.path.join(tmp, f"s{n}.jpg")
            run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", f"{source}=size={size}",
                 "-frames:v", "1", src])
            run(["cjpeg", *opts, "-outfile", jpg, src])
            pictures.append((f"{size} {source} {' '.join(opts)}", jpg))

        # A frame the way tools/mkvideo.py makes them, from ffmpeg's encoder.
        clip = os.path.join(tmp, "f.jpg")
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x240",
             "-frames:v", "1", "-vf", "format=yuvj420p", "-q:v", "6", clip])
        pictures.append(("320x240 ffmpeg mjpeg -q:v 6", clip))

        for name, jpg in pictures:
            ref = os.path.join(tmp, "ref.ppm")
            run(["djpeg", "-dct", "fast", "-nosmooth", "-ppm", "-outfile", ref, jpg])
            rw, rh, full = ppm(ref)
            for scale in range(4):
                w, h, got = ours(checked, jpg, scale)
                want = (-(-rw >> scale), -(-rh >> scale))
                if (w, h) != want:
                    print(f"FAIL {name} 1/{1 << scale}: {w}x{h}, not {want[0]}x{want[1]}")
                    failed += 1
                    continue
                psnr, worst = compare(got, to565(shrink(full, rw, rh, scale)))
                # At full size the arithmetic is libjpeg's fast IDCT, which
                # truncates where this one rounds: within a step of RGB565 (9,
                # once widened) everywhere. Scaled down, each output pixel is
                # the average of the pixels it stands for, which is what the
                # full-size picture shrunk by averaging is -- give or take the
                # order of averaging and clipping bright colours to RGB, and
                # the edges, where the encoder's padding joins the average. In
                # a picture a few blocks across, those are most of it.
                tiny = min(rw, rh) < 64
                ok = worst <= 9 and psnr >= 38 if scale == 0 else psnr >= (20 if tiny else 36)
                failed += not ok
                print(f"{'ok  ' if ok else 'FAIL'} {name} 1/{1 << scale}: "
                      f"{psnr:.1f} dB, worst {worst}")

        for name, jpg in pictures[:3] + pictures[6:8]:
            out = run([checked, "-f", jpg, "400"]).stdout.decode().strip()
            print(f"ok   {name}: {out}")
        print(run([fast, "-t", clip, "500"]).stdout.decode().strip(), "(this PC, 320x240)")
    if failed:
        raise SystemExit(f"{failed} failed")
    print("all passed")


if __name__ == "__main__":
    main()
