#!/usr/bin/env python3
"""Turn a video into something PocketType can play.

    python3 tools/mkvideo.py clip.mp4 clip.ptv        (or: make video FILE=clip.mp4)
    python3 tools/mkvideo.py clip.mp4 clip.ptv --fps 24 --quality 4

There is no H.264 decoder on a 240 MHz chip and there is not going to be
one, so a clip becomes what the chip decodes quickly: one baseline JPEG per
frame, cut into two strips so that both cores work on each frame
(pico-os/bin/jpeg.c), and raw 16-bit mono PCM, which the codec plays
directly. ffmpeg does the work here, on a machine that has the cycles.

What makes a clip look its best on the board, all on by default:

- only the picture is stored, as big as fits, in whole 16-pixel blocks;
  the player paints the black round it once, so bars cost nothing to
  decode or send, and carry no compression noise;
- black bars the source has in its own picture (a 4:3 video posted as
  16:9) are found with ffmpeg's cropdetect and cut (--crop none keeps
  them; --fill fills the screen and cuts what does not fit instead);
- the colours are converted to what the decoder assumes, full-range
  BT.601, where web video is limited-range BT.709;
- the source's own frame rate, if it is 30 or less (60 becomes 30):
  taking 30 from 25 repeats every fifth frame, and motion judders;
- JPEG quality 3 (a 320x240 frame decodes in about 14 ms, so 30 fps
  still has room), scaled with lanczos and lightly sharpened;
- sound at 44.1 kHz, where it used to be 16.

The container is described in pico-os/bin/video.c. It interleaves each
frame's picture and its sound, so the player reads straight through the
file without seeking, which is what an SD card is good at; an index of
where each frame starts comes last, for jumping about in it.

Needs ffmpeg and ffprobe on PATH.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

MAGIC = b"PTV2"
HEADER = 32
RATE = 44100            # about what the sound is made at; see sound_rate()
MAX_FPS = 30            # what the board decodes with time to spare
DEFAULT_SIZE = "320x240"
DEFAULT_SLICES = 2      # one per core: the chip decodes them at the same time


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def has_audio(path):
    out = run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", path]).stdout
    return any(s["codec_type"] == "audio" for s in json.loads(out)["streams"])


def sound_rate(fps, target=RATE):
    """A whole number of samples in every frame, as near RATE as that allows.

    The player takes one frame's sound per frame. At 16000 Hz and 30 fps a
    frame would be 533.3 samples; rounding to 533 and playing at 16000 puts
    the sound a third of a sample further behind the picture every frame,
    a tenth of a second by the end of a song. 533 * 30 = 15990 Hz instead
    is 0.06% off in pitch, which nobody can hear, and never drifts at all.
    """
    return (target // fps) * fps


def source_fps(path):
    """The source's own frame rate, as a fraction's value."""
    out = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
               "stream=avg_frame_rate,r_frame_rate", "-of", "json", path]).stdout
    st = json.loads(out)["streams"][0]
    for key in ("avg_frame_rate", "r_frame_rate"):
        num, _, den = st.get(key, "0/1").partition("/")
        if float(den or 1) and float(num) > 0:
            return float(num) / float(den or 1)
    return MAX_FPS


def pick_fps(src):
    """The source's rate if the board can keep up with it, or an even
    fraction of it: 25 stays 25 and 60 becomes 30. Taking 30 from a 25
    source repeats every fifth frame, and the motion judders."""
    fps = src
    while fps > MAX_FPS + 0.5:
        fps /= 2
    return max(1, round(fps))


def source_size(path):
    """Width and height as shown, square pixels (the sample aspect applied)."""
    out = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
               "stream=width,height,sample_aspect_ratio", "-of", "json", path]).stdout
    st = json.loads(out)["streams"][0]
    w, h = st["width"], st["height"]
    num, _, den = (st.get("sample_aspect_ratio") or "1:1").partition(":")
    if num.isdigit() and den.isdigit() and int(num) and int(den):
        w = w * int(num) // int(den)
    return w, h


def picture_size(w, h, sw, sh, fill):
    """What the picture is scaled to: as big as fits the screen (or covers
    it, with --fill), in whole 16-pixel blocks -- the decoder's unit, and
    what lets the height split into two strips of whole blocks."""
    if fill:
        return sw, sh
    f = min(sw / w, sh / h)
    return max(16, int(w * f) // 16 * 16), max(16, int(h * f) // 16 * 16)


def detect_crop(path, start, length):
    """The black bars the source carries in its own picture -- a 4:3 film
    posted as 16:9 has them left and right -- found by ffmpeg's cropdetect
    over a stretch from the middle, so they are cut rather than letterboxed
    again inside the screen's own bars."""
    probe = run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of",
                 "json", path]).stdout
    duration = float(json.loads(probe)["format"].get("duration", 0) or 0)
    at = start or (f"{duration / 3:.1f}" if duration > 60 else "0")
    out = subprocess.run(["ffmpeg", "-v", "info", "-ss", at, "-i", path, "-t", length or "20",
                          "-vf", "cropdetect=limit=24:round=2:reset=0", "-an", "-f", "null",
                          "-"], capture_output=True, text=True).stderr
    found = re.findall(r"crop=(\d+):(\d+):(\d+):(\d+)", out)
    return "crop=" + ":".join(found[-1]) if found else None


def split_jpegs(blob):
    """The frames out of ffmpeg's mjpeg stream, split on the markers."""
    frames, start = [], blob.find(b"\xff\xd8")
    if start < 0:
        raise SystemExit("ffmpeg produced no JPEG frames")
    while True:
        nxt = blob.find(b"\xff\xd8", start + 2)
        frames.append(blob[start:nxt if nxt > 0 else len(blob)])
        if nxt < 0:
            return frames
        start = nxt


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--fps", type=int, default=None,
                    help="frames a second; the source's own if it is 30 or less")
    ap.add_argument("--rate", type=int, default=RATE, help="sound samples a second")
    ap.add_argument("--crop", default="auto",
                    help="auto finds the source's own black bars and cuts them; "
                         "none keeps the picture whole; or W:H:X:Y")
    ap.add_argument("--fill", action="store_true",
                    help="fill the screen and cut what does not fit, rather "
                         "than letterbox")
    ap.add_argument("--sharpen", type=float, default=0.5,
                    help="unsharp mask after scaling down, 0 for none")
    ap.add_argument("--size", default=DEFAULT_SIZE,
                    help="WxH of the screen the picture has to fit. Only the "
                         "picture is stored, not bars round it: the player "
                         "paints the rest black once")
    ap.add_argument("--quality", type=int, default=3,
                    help="ffmpeg -q:v, 2 (best) to 31 (smallest)")
    ap.add_argument("--slices", type=int, default=DEFAULT_SLICES,
                    help="horizontal strips per frame, decoded one per core. "
                         "2 is the chip's core count; 1 makes an ordinary "
                         "single-picture file")
    ap.add_argument("--start", default=None, help="seek before converting, e.g. 0:30")
    ap.add_argument("--length", default=None, help="how much to take, e.g. 20")
    args = ap.parse_args()

    out_path = args.output or os.path.splitext(args.input)[0] + ".ptv"
    m = re.fullmatch(r"(\d+)x(\d+)", args.size)
    if not m:
        raise SystemExit("--size wants WxH, like 320x240")
    w, h = int(m.group(1)), int(m.group(2))
    if w % 8 or h % 8:
        raise SystemExit("--size wants multiples of 8: the decoder works in blocks")
    if args.slices < 1 or h % (8 * args.slices):
        raise SystemExit(f"--slices must divide the height into whole blocks of 8; "
                         f"{h} does not divide by {args.slices}")

    sound = has_audio(args.input)
    if not args.fps:
        args.fps = pick_fps(source_fps(args.input))
    rate = sound_rate(args.fps, args.rate)
    seek = ["-ss", args.start] if args.start else []
    take = ["-t", args.length] if args.length else []

    crop = None
    if args.crop == "auto":
        crop = detect_crop(args.input, args.start, args.length)
    elif args.crop != "none":
        crop = "crop=" + args.crop
    cw, ch = (tuple(map(int, crop[5:].split(":")[:2])) if crop
              else source_size(args.input))
    # Only the picture, as big as fits: never stretched (a face should not
    # get wider on a small screen), and no black bars stored round it --
    # the player paints those once. Scaled with lanczos, and into the
    # colours the board's decoder assumes: JFIF's full range and BT.601
    # matrix, where a web video is limited range BT.709 -- left to ffmpeg's
    # defaults the greens and reds came out a little wrong. A light unsharp
    # mask afterwards puts back the edge that shrinking a picture softens.
    screen_w, screen_h = w, h
    w, h = picture_size(cw, ch, screen_w, screen_h, args.fill)
    if args.fill:
        scale = (f"scale={w}:{h}:flags=lanczos:force_original_aspect_ratio=increase:"
                 f"out_color_matrix=bt601:out_range=full,crop={w}:{h}")
    else:
        scale = f"scale={w}:{h}:flags=lanczos:out_color_matrix=bt601:out_range=full"
    picture = (f"fps={args.fps},{crop + ',' if crop else ''}{scale},"
               f"{f'unsharp=5:5:{args.sharpen}:5:5:0,' if args.sharpen else ''}"
               f"format=yuvj420p")

    # One pass makes every strip and the sound. Each strip is its own little
    # video, so the board can hand one to each core and decode a frame in
    # half the time; making them all from one decode of the source, and the
    # sound from the same one, keeps the three lined up with each other.
    sh = h // args.slices
    with tempfile.TemporaryDirectory() as tmp:
        names = [os.path.join(tmp, f"v{i}.mjpeg") for i in range(args.slices)]
        audio = os.path.join(tmp, "a.raw")
        if args.slices > 1:
            graph = (f"[0:v]{picture},split={args.slices}" +
                     "".join(f"[s{i}]" for i in range(args.slices)) + ";" +
                     ";".join(f"[s{i}]crop={w}:{sh}:0:{i * sh}[v{i}]"
                              for i in range(args.slices)))
        else:
            graph = f"[0:v]{picture}[v0]"
        cmd = ["ffmpeg", "-v", "error", "-y", *seek, "-i", args.input, *take,
               "-filter_complex", graph]
        for i, name in enumerate(names):
            cmd += ["-map", f"[v{i}]", "-q:v", str(args.quality), "-f", "mjpeg", name]
        if sound:
            cmd += ["-map", "0:a:0", "-ac", "1", "-ar", str(rate), "-f", "s16le", audio]
        run(cmd)
        slices = []
        for name in names:
            with open(name, "rb") as f:
                slices.append(split_jpegs(f.read()))
        pcm = b""
        if sound:
            with open(audio, "rb") as f:
                pcm = f.read()
        count = min(len(s) for s in slices)

    per_frame = rate // args.fps * 2 if sound else 0
    offsets = []
    with open(out_path, "wb") as f:
        f.write(struct.pack("<4sHHHHIIIHHI", MAGIC, w, h, args.fps,
                            1 if sound else 0, count, rate if sound else 0,
                            per_frame, args.slices, 0, 0))
        for i in range(count):
            offsets.append(f.tell())
            for part in slices:
                f.write(struct.pack("<I", len(part[i])))
                f.write(part[i])
            if per_frame:
                chunk = pcm[i * per_frame:(i + 1) * per_frame]
                f.write(chunk + b"\0" * (per_frame - len(chunk)))
        # The index, so that the player can jump: where each frame starts.
        # The header says where it is, in what was a spare word.
        index_at = f.tell()
        f.write(b"PTVI" + struct.pack(f"<I{count}I", count, *offsets))
        f.seek(28)
        f.write(struct.pack("<I", index_at))

    size = os.path.getsize(out_path)
    secs = count / args.fps
    print(f"{out_path}: {count} frames, {w}x{h} at {args.fps} fps in "
          f"{args.slices} slice{'s' if args.slices > 1 else ''}, "
          f"{f'sound at {rate} Hz' if sound else 'silent'}, {secs:.1f} s, "
          f"{crop or 'not cropped'}, "
          f"{size / 1024:.0f} KB ({size / max(secs, 0.001) / 1024:.0f} KB/s)")


if __name__ == "__main__":
    main()
