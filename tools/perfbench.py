#!/usr/bin/env python3
"""The decoders' cost on the ESP32-S3's own instruction set, under QEMU.

Makes sound and pictures with ffmpeg and cjpeg, builds tools/perfbench/
(the real codec/, minimp3, bin/jpeg.c and sink.c's 2:1 filter, with ESP-IDF
for the S3) and runs it in QEMU with -icount, where the cycle counter
follows the instructions run: the same code gives the same count every
time. QEMU has no caches and no PSRAM waits, and its count is not the
board's, so a number here is for comparing one version of the code with
another, not for the time a decode takes on the board. Each result
also has a CRC-32 of everything decoded, which a change that only makes
things faster leaves alone.

    tools/perfbench.py [--json FILE] [--build DIR]

Prints, for each: millions of QEMU cycles, how much was decoded, the
cycles a second of sound or a picture costs, and the CRC.
"""
import argparse
import glob
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
PROJECT = ROOT / "tools/perfbench"
IDF = os.environ.get("IDF_PATH", os.path.expanduser("~/esp/esp-idf"))

AUDIO, JPEG_SLICE, JPEG_DC = 1, 2, 3

# Something like music for the encoders: a chord with a beat, pink noise.
MUSIC = ("aevalsrc='0.25*sin(2*PI*220*t)*(1+0.5*sin(2*PI*2*t))+0.15*sin(2*PI*277*t)"
         "+0.12*sin(2*PI*330*t+sin(2*PI*0.5*t))|0.25*sin(2*PI*165*t)+0.15*sin(2*PI*440*t)"
         "*(1+0.5*sin(2*PI*3*t))+0.1*sin(2*PI*554*t)':s={rate}:d={seconds}")
NOISE = "anoisesrc=c=pink:a=0.12:r={rate}:d={seconds}:seed=7"

SOUNDS = [  # name, rate, seconds, channels, ffmpeg output options
    ("flac 44.1k/16", 44100, 6, 2, ["-sample_fmt", "s16", "-c:a", "flac"], "flac"),
    ("flac 96k/24", 96000, 3, 2, ["-sample_fmt", "s32", "-bits_per_raw_sample", "24",
                                  "-c:a", "flac"], "flac"),
    ("mp3 44.1k 192k", 44100, 6, 2, ["-c:a", "libmp3lame", "-b:a", "192k"], "mp3"),
    ("mp3 44.1k mono 96k", 44100, 6, 1, ["-c:a", "libmp3lame", "-b:a", "96k"], "mp3"),
    ("wav 44.1k/16", 44100, 6, 2, ["-c:a", "pcm_s16le"], "wav"),
    ("wav 48k/24", 48000, 3, 2, ["-c:a", "pcm_s24le"], "wav"),
]

# A clip's frames as tools/mkvideo.py makes them: 320x240, two slices of
# 320x120, quality 2, 4:2:0, ffmpeg's own Huffman tables per frame.
CLIP = ("mandelbrot=s=320x240:r=24,noise=alls=12:allf=t,unsharp=5:5:0.5:5:5:0,"
        "eq=saturation=1.5,format=yuvj420p")
CLIP2 = "testsrc2=s=320x240:r=24,noise=alls=8:allf=t,format=yuvj420p"
FRAMES = 6


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def ffmpeg(*args):
    run(["ffmpeg", "-v", "error", "-y", "-nostdin", *args])


def make_media(gen):
    entries = []
    for name, rate, seconds, channels, opts, ext in SOUNDS:
        path = gen / f"{name.replace(' ', '_').replace('/', '_')}.{ext}"
        if not path.exists():
            music = MUSIC.format(rate=rate, seconds=seconds)
            noise = NOISE.format(rate=rate, seconds=seconds)
            ffmpeg("-f", "lavfi", "-i", music, "-f", "lavfi", "-i", noise, "-filter_complex",
                   "[1]asplit[l][r];[l][r]join=inputs=2:channel_layout=stereo[n];"
                   "[0][n]amix=inputs=2:normalize=0[m]", "-map", "[m]",
                   "-ac", str(channels), "-ar", str(rate), *opts, str(path))
        entries.append((name, AUDIO, path.read_bytes()))
    for n, source in enumerate((CLIP, CLIP2)):
        for s in range(2):
            pattern = gen / f"clip{n}_{s}_%02d.jpg"
            if not (gen / f"clip{n}_{s}_01.jpg").exists():
                ffmpeg("-f", "lavfi", "-i", source, "-frames:v", str(FRAMES), "-vf",
                       f"crop=320:120:0:{s * 120}", "-q:v", "2", str(pattern))
    for f in range(1, FRAMES + 1):
        for n in range(2):
            for s in range(2):
                path = gen / f"clip{n}_{s}_{f:02d}.jpg"
                entries.append(("jpeg clip slices", JPEG_SLICE, path.read_bytes()))
    cover = gen / "cover.jpg"
    if not cover.exists():
        ppm = gen / "cover.ppm"
        ffmpeg("-f", "lavfi", "-i", "mandelbrot=s=1400x1400", "-frames:v", "1", str(ppm))
        with open(cover, "wb") as out:
            run(["cjpeg", "-progressive", "-quality", "90", "-sample", "2x2", str(ppm)],
                stdout=out)
    entries.append(("jpeg progressive 1/8", JPEG_DC, cover.read_bytes()))

    # "PBMEDIA1" count { name[32] kind offset length } data...
    head = 12 + 44 * len(entries)
    table, data = b"", b""
    for name, kind, blob in entries:
        table += struct.pack("<32sIII", name.encode(), kind, head + len(data), len(blob))
        data += blob + b"\0" * (-len(blob) % 4)
    (gen / "media.bin").write_bytes(b"PBMEDIA1" + struct.pack("<I", len(entries)) + table + data)
    return {name: blob for name, kind, blob in entries if kind == AUDIO}


def cut_half(gen):
    """sink.c's 2:1 filter, cut out as tools/audio_test.py does."""
    sink = (ROOT / "bin/sink.c").read_text()
    half = sink[sink.index("#define HALF_TAPS"):sink.index("int sink_open")]
    (gen / "half.h").write_text("#include <math.h>\n#include <stdbool.h>\n" + half)


def qemu():
    found = sorted(glob.glob(os.path.expanduser(
        "~/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa")), reverse=True)
    return os.environ.get("QEMU") or (found[0] if found else "qemu-system-xtensa")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", help="also write the results here")
    ap.add_argument("--build", default=str(ROOT / "build/perfbench"))
    args = ap.parse_args()
    build = Path(args.build).resolve()
    gen = build / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    sounds = make_media(gen)
    cut_half(gen)

    env = dict(os.environ, IDF_PATH=IDF)
    sh = (f". {IDF}/export.sh >/dev/null 2>&1 && idf.py -C {PROJECT} -B {build / 'idf'} "
          f"-D SDKCONFIG={build / 'sdkconfig'} -D PB_GEN={gen} build >/dev/null && "
          f"cd {build / 'idf'} && esptool --chip esp32s3 merge-bin --pad-to-size 16MB "
          f"-o flash.bin @flash_args >/dev/null")
    run(["bash", "-c", sh], env=env)
    log = build / "qemu.log"
    with open(log, "w") as f:
        proc = subprocess.Popen([qemu(), "-nographic", "-machine", "esp32s3", "-m", "8M",
                                 "-icount", "shift=0", "-drive",
                                 f"file={build / 'idf/flash.bin'},if=mtd,format=raw",
                                 "-serial", "mon:stdio"], stdin=subprocess.DEVNULL,
                                stdout=f, stderr=subprocess.STDOUT)
        try:
            while proc.poll() is None:
                try:
                    proc.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
                text = log.read_text(errors="replace")
                if "PB-DONE" in text or "abort()" in text or "Guru Meditation" in text:
                    break
        finally:
            proc.kill()
            proc.wait()
    text = log.read_text(errors="replace")
    if "PB-DONE" not in text or "PB-ERROR" in text:
        sys.exit(f"perfbench failed; see {log}")

    results = {}
    print(f"{'':24} {'Mcycles':>9} {'decoded':>14} {'per second/picture':>19}  crc")
    for line in text.splitlines():
        if not line.startswith("PB "):
            continue
        name, cycles, units, crc = line[3:].strip().split("|")
        cycles, units = int(cycles), int(units)
        if name in sounds or name.startswith("half_run"):
            rate = 192000 if name.startswith("half_run") else next(
                r for n, r, *_ in SOUNDS if n == name)
            seconds = units / rate
            per = cycles / seconds
            what = f"{seconds:.1f} s"
        else:
            per = cycles / units
            what = f"{units} pictures"
        results[name] = {"cycles": cycles, "units": units, "per": round(per),
                         "crc": crc}
        print(f"{name:24} {cycles / 1e6:9.2f} {what:>14} {per / 1e6:16.2f} M  {crc}")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1) + "\n")


if __name__ == "__main__":
    main()
