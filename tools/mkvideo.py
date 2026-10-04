#!/usr/bin/env python3
"""Turn a video into a clip pico-os plays (.ptv).

    tools/mkvideo.py film.mkv                  film.ptv, beside it
    tools/mkvideo.py film.mkv clips/film.ptv
    make video FILE=film.mkv                   the same, then sent to ~/video

There is no H.264 decoder on a 240 MHz chip and there is not going to be
one, so a clip becomes what the chip decodes quickly: one baseline JPEG per
frame, cut into two strips so that both cores work on each frame
(bin/jpeg.c), and raw 16-bit PCM, which the codec plays directly. ffmpeg
does the work here, on a machine that has the cycles.

The defaults are what looked and sounded best on the board's panel and
speaker, side by side with the alternatives:

- the screen filled, the edges that do not fit cut off (--fit keeps the
  whole picture and letterboxes it); black bars the source carries in its
  own picture, a 4:3 film posted as 16:9, are found and cut first;
- the source's frame rate up to 24, anything faster 24: the board shows
  24 a second in step with its refresh, each frame for two refreshes
  (48 Hz), and leaves frames out of a quicker clip;
- JPEG quality 2, the best (a frame decodes in about 29 ms);
- 50% more colour than the source and the shadows lifted a little: the TN
  panel washes colour out and shows its first steps of grey as black;
- the English sound track when there is one, evened out and brought up
  to -14 LUFS for a small speaker, in stereo for headphones (the speaker
  hears the two sides mixed);
- full-range BT.601 colours, what the board's decoder assumes, converted
  from whatever matrix the source says it uses.

Decoding the source is most of the work. Where the PC's graphics can
decode it (VA-API: AMD and Intel on Linux), they do, and scale it down
part of the way; --cpu does it all on the processor. Either way the last
scaling step is the same lanczos filter.

The container is described in bin/video.c. It interleaves each frame's
picture and its sound, so the player reads straight through the file
without seeking, which is what an SD card is good at; an index of where
each frame starts comes last, for jumping about in it.

Needs ffmpeg and ffprobe on PATH.
"""
import argparse
import glob
import json
import mmap
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor

MAGIC = b"PTV2"
SOUND, STEREO = 1, 2            # the header's flags
RATE = 48000                    # the sound's rate when the source does not say; see sound_rate()
MAX_RATE = 48000                # what the player takes
MAX_FPS = 24
DEFAULT_SIZE = "320x240"
DEFAULT_SLICES = 2              # one per core: the chip decodes them at the same time


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def seconds(text):
    """0:30, 1:02:03 or 90 as seconds."""
    if text is None:
        return None
    total = 0.0
    for part in text.split(":"):
        total = total * 60 + float(part)
    return total


def clock(s):
    s = int(s)
    return f"{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}" if s >= 3600 else f"{s // 60}:{s % 60:02d}"


class Source:
    """What ffprobe says about the file, asked once."""

    def __init__(self, path):
        info = json.loads(run(["ffprobe", "-v", "error", "-show_streams", "-show_format",
                               "-of", "json", path]))
        video = [s for s in info["streams"] if s["codec_type"] == "video"
                 and not s.get("disposition", {}).get("attached_pic")]
        if not video:
            raise SystemExit(f"{path}: no picture in it")
        self.video = video[0]
        self.audio = [s for s in info["streams"] if s["codec_type"] == "audio"]
        self.duration = float(info["format"].get("duration") or 0)
        self.w, self.h = self.video["width"], self.video["height"]
        num, _, den = (self.video.get("sample_aspect_ratio") or "1:1").partition(":")
        self.sar = int(num) / int(den) if num.isdigit() and den.isdigit() and \
            int(num) and int(den) else 1.0

    def fps(self):
        for key in ("avg_frame_rate", "r_frame_rate"):
            num, _, den = self.video.get(key, "0/1").partition("/")
            if float(den or 1) and float(num) > 0:
                return float(num) / float(den or 1)
        return 24.0

    def matrix(self):
        """The colour matrix the picture was made with: what it says, or
        for a file that does not, what its size makes likely (ffmpeg would
        take BT.601 for all of them, and HD greens and reds come out off)."""
        space = self.video.get("color_space", "")
        if space in ("smpte170m", "bt470bg", "fcc"):
            return "bt601"
        if space.startswith("bt2020"):
            return "bt2020"
        if space == "bt709":
            return "bt709"
        return "bt709" if self.h >= 600 else "bt601"

    def full_range(self):
        return self.video.get("color_range") in ("pc", "jpeg")

    def hdr(self):
        return self.video.get("color_transfer") in ("smpte2084", "arib-std-b67")

    def sound_rate(self, track):
        """The track's own rate, as near as the player goes: resampling it
        to some other rate only loses a little."""
        try:
            hz = int(self.audio[track]["sample_rate"])
        except (TypeError, KeyError, ValueError, IndexError):
            return RATE
        return min(hz, MAX_RATE) if hz >= 8000 else RATE

    def track(self, want):
        """Which sound track, counted among the sound tracks: a number,
        or a language matched against their tags; English if none is
        asked for and there is one, else the first."""
        if not self.audio:
            return None
        if want is not None and want.isdigit():
            if int(want) >= len(self.audio):
                raise SystemExit(f"there are {len(self.audio)} sound tracks, counted from 0")
            return int(want)
        names = []
        for i, st in enumerate(self.audio):
            tags = {k.lower(): v.lower() for k, v in st.get("tags", {}).items()}
            lang, title = tags.get("language", ""), tags.get("title", "").strip("[]")
            names.append(lang or "?")
            if want is None and (lang in ("eng", "en") or "english" in title):
                return i
            if want is not None and want.lower() in (lang, title):
                return i
        if want is None:
            return 0
        raise SystemExit(f"no {want} sound in the file; it has: {', '.join(names)}")


def pick_fps(src):
    """Keep slower sources; anything quicker becomes 24. The board shows a
    whole screen of 24 a second in step with its refresh, two refreshes
    each (48 Hz); more than that it leaves frames out to keep up."""
    if src <= MAX_FPS + 0.5:
        return max(1, round(src))
    return MAX_FPS


def sound_rate(fps, target=RATE):
    """A whole number of samples in every frame, as near the target as that allows.

    The player takes one frame's sound per frame. At 16000 Hz and 30 fps a
    frame would be 533.3 samples; rounding to 533 and playing at 16000 puts
    the sound a third of a sample further behind the picture every frame,
    a tenth of a second by the end of a song. 533 * 30 = 15990 Hz instead
    is 0.06% off in pitch, which nobody can hear, and never drifts at all.
    """
    return (target // fps) * fps


def detect_crop(path, src, start, length):
    """The black bars the source carries in its own picture -- a 4:3 film
    posted as 16:9 has them left and right -- found by ffmpeg's cropdetect
    in three places at once, and the largest box of the three kept: one
    dark scene alone would have cut into the picture."""
    begin = start or 0.0
    span = length or max(0.0, (src.duration or 60) - begin)

    def look(at):
        out = subprocess.run(["ffmpeg", "-v", "info", "-ss", f"{at:.2f}", "-i", path, "-t", "4",
                              "-map", "0:v:0", "-vf", "cropdetect=limit=24:round=2:reset=0",
                              "-an", "-f", "null", "-"], capture_output=True, text=True).stderr
        found = re.findall(r"crop=(\d+):(\d+):(\d+):(\d+)", out)
        return tuple(map(int, found[-1])) if found else None

    with ThreadPoolExecutor(3) as pool:
        boxes = [b for b in pool.map(look, [begin + span * f for f in (0.25, 0.5, 0.75)]) if b]
    if not boxes:
        return None
    x0 = min(b[2] for b in boxes)
    y0 = min(b[3] for b in boxes)
    x1 = max(b[2] + b[0] for b in boxes)
    y1 = max(b[3] + b[1] for b in boxes)
    if (x1 - x0, y1 - y0) == (src.w, src.h):
        return None                     # no bars
    return x1 - x0, y1 - y0, x0, y0


def picture_size(cw, ch, sw, sh, fit):
    """What the picture is scaled to: covering the screen, or as big as
    fits it with --fit, in whole 16-pixel blocks -- the decoder's unit,
    and what lets the height split into two strips of whole blocks."""
    if not fit:
        return sw, sh
    f = min(sw / cw, sh / ch)
    return max(16, int(cw * f) // 16 * 16), max(16, int(ch * f) // 16 * 16)


def vaapi_device():
    nodes = sorted(glob.glob("/dev/dri/renderD*"))
    return nodes[0] if nodes and os.access(nodes[0], os.R_OK | os.W_OK) else None


def picture_filter(args, src, crop, w, h, gpu):
    """The picture's way from the source to the strips' JPEGs.

    Only the picture, as big as fits: never stretched (a face should not
    get wider on a small screen), and no black bars stored round it --
    the player paints those once. Scaled with lanczos, and into the
    colours the board's decoder assumes. A light unsharp mask afterwards
    puts back the edge that shrinking a picture softens.

    On the graphics card the frames are dropped to the clip's rate first,
    then scaled to twice what is wanted, which is all the lanczos step
    needs to work from, and only that much comes back over the bus.
    """
    cw, ch, cx, cy = crop or (src.w, src.h, 0, 0)
    steps = [f"fps={args.fps}"]
    if gpu:
        k = min(1.0, 2 * max(w / (cw * src.sar), h / ch))
        gw, gh = max(2, round(src.w * k / 2) * 2), max(2, round(src.h * k / 2) * 2)
        steps += [f"scale_vaapi=w={gw}:h={gh}:mode=hq:format=nv12", "hwdownload", "format=nv12"]
        cw, ch, cx, cy = (round(v * k / 2) * 2 for v in (cw, ch, cx, cy))
        cw, ch = min(cw, gw - cx), min(ch, gh - cy)
    if crop or gpu:
        steps.append(f"crop={cw}:{ch}:{cx}:{cy}")
    colours = (f"in_color_matrix={src.matrix()}:in_range={'full' if src.full_range() else 'limited'}"
               f":out_color_matrix=bt601:out_range=full")
    if src.sar != 1.0 and not gpu:
        steps.append(f"scale=iw*{src.sar}:ih")
    if args.fit:
        steps.append(f"scale={w}:{h}:flags=lanczos:{colours}")
    else:
        steps.append(f"scale={w}:{h}:flags=lanczos:force_original_aspect_ratio=increase:{colours}")
        steps.append(f"crop={w}:{h}")
    if args.denoise:
        d = args.denoise
        steps.append(f"hqdn3d={d}:{d}:{d * 2}:{d * 2}")
    if args.sharpen:
        steps.append(f"unsharp=5:5:{args.sharpen}:5:5:0")
    if args.saturation != 1:
        steps.append(f"eq=saturation={args.saturation}")
    steps.append("format=yuvj420p")
    # Shadows lifted out of the panel's first steps of grey, by up to
    # `lift` times themselves and fading out by 50 or so:
    # y + A * y * exp(-y / 16), which keeps black black and never swaps
    # two lumas round. --black does the opposite and loses the shadows.
    if args.lift > 0:
        steps.append(f"lutyuv=y='clip(val+{args.lift}*val*exp(-val/16),0,255)'")
    if args.black > 0:
        k = args.black
        steps.append(f"lutyuv=y='clip((val-{k})*255/(255-{k}),0,255)'")
    return ",".join(steps)


def bounds(args, probe=False):
    """Where to start and how much to take, on the input: every output
    stops there."""
    cmd = ["-ss", args.start] if args.start else []
    if probe:
        return cmd + ["-t", "2"]
    return cmd + (["-t", args.length] if args.length else [])


def picture_command(args, src, crop, w, h, names, gpu, probe=False):
    sh = h // args.slices
    picture = picture_filter(args, src, crop, w, h, gpu)
    if args.slices > 1:
        graph = (f"[0:v:0]{picture},split={args.slices}" +
                 "".join(f"[s{i}]" for i in range(args.slices)) + ";" +
                 ";".join(f"[s{i}]crop={w}:{sh}:0:{i * sh}[v{i}]" for i in range(args.slices)))
    else:
        graph = f"[0:v:0]{picture}[v0]"
    cmd = ["ffmpeg", "-v", "error", "-y", "-nostdin"]
    if gpu:
        cmd += ["-hwaccel", "vaapi", "-hwaccel_device", gpu, "-hwaccel_output_format", "vaapi"]
    cmd += bounds(args, probe) + ["-i", args.input, "-filter_complex", graph]
    for i, name in enumerate(names):
        cmd += ["-map", f"[v{i}]", "-q:v", str(args.quality), "-f", "mjpeg", name]
    return cmd


def sound_command(args, track, rate, audio):
    """The sound, in a process of its own: levelling it is one thread's
    work, and inside the picture's ffmpeg the two took turns.

    Measured and levelled on what is played, not on a 5.1 mix, whose
    downmix comes out several dB under the target. One pass, so it rides
    the level as it goes: dialogue comes up, explosions come down, for a
    speaker this size."""
    af = f"aformat=channel_layouts={'mono' if args.mono else 'stereo'}"
    if args.loud is not None:
        af += f",loudnorm=I={args.loud}:LRA=9:TP=-1.5"
    return (["ffmpeg", "-v", "error", "-y", "-nostdin"] + bounds(args) +
            ["-i", args.input, "-map", f"0:a:{track}", "-af", af, "-ar", str(rate),
             "-f", "s16le", audio])


def convert(cmd, total, quiet):
    """ffmpeg, with a line saying how far it has got."""
    cmd = cmd[:1] + ["-nostats", "-progress", "pipe:1"] + cmd[1:]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    err = []
    reader = threading.Thread(target=lambda: err.extend(p.stderr), daemon=True)
    reader.start()
    started, done = time.monotonic(), 0.0
    show = not quiet and sys.stderr.isatty()
    for line in p.stdout:
        key, _, value = line.strip().partition("=")
        if key == "out_time_us" and value.isdigit():
            done = int(value) / 1e6
        elif key == "progress" and show and total:
            took = time.monotonic() - started
            speed = done / took if took > 0 else 0
            left = (total - done) / speed if speed > 0 else 0
            pct = min(100, 100 * done / total)
            sys.stderr.write(f"\r  {pct:3.0f}%  {clock(done)} of {clock(total)}"
                             f"  {speed:4.1f}x  {clock(left)} left \x1b[K")
            sys.stderr.flush()
    p.wait()
    reader.join()
    if show:
        sys.stderr.write("\r\x1b[K")
    if p.returncode:
        raise subprocess.CalledProcessError(p.returncode, cmd, stderr="".join(err))


def jpeg_frames(data):
    """Where each JPEG in ffmpeg's mjpeg stream starts and ends: its
    header segments walked by their lengths, then the scan read up to the
    end marker, which the scan's own bytes can never imitate."""
    pos, n = 0, len(data)
    while pos + 4 <= n:
        if data[pos:pos + 2] != b"\xff\xd8":
            raise SystemExit("ffmpeg's JPEG stream is damaged")
        p = pos + 2
        while p + 4 <= n:
            marker = data[p + 1]
            p += 2 + int.from_bytes(data[p + 2:p + 4], "big")
            if marker == 0xDA:          # start of scan
                break
        end = data.find(b"\xff\xd9", p)
        if end < 0:
            return
        yield pos, end + 2
        pos = end + 2


def mapped(path):
    if not os.path.getsize(path):
        return b""
    with open(path, "rb") as f:
        return mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)


def write_clip(out_path, w, h, fps, rate, channels, slice_paths, audio_path):
    slices = [mapped(p) for p in slice_paths]
    frames = [list(jpeg_frames(s)) for s in slices]
    pcm = mapped(audio_path) if audio_path else b""
    count = min(len(f) for f in frames)
    per_frame = rate // fps * 2 * channels if audio_path else 0
    flags = (SOUND | (STEREO if channels == 2 else 0)) if audio_path else 0
    offsets = []
    with open(out_path, "wb") as f:
        f.write(struct.pack("<4sHHHHIIIHHI", MAGIC, w, h, fps, flags, count,
                            rate if audio_path else 0, per_frame, len(slices), 0, 0))
        for i in range(count):
            offsets.append(f.tell())
            for s, fr in zip(slices, frames):
                a, b = fr[i]
                f.write(struct.pack("<I", b - a))
                f.write(s[a:b])
            if per_frame:
                chunk = pcm[i * per_frame:(i + 1) * per_frame]
                f.write(chunk + b"\0" * (per_frame - len(chunk)))
        # The index, so that the player can jump: where each frame starts.
        # The header says where it is, in what was a spare word.
        index_at = f.tell()
        f.write(b"PTVI" + struct.pack(f"<I{count}I", count, *offsets))
        f.seek(28)
        f.write(struct.pack("<I", index_at))
    for m in slices + [pcm]:
        if isinstance(m, mmap.mmap):
            m.close()
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--fit", action="store_true",
                    help="keep the whole picture, with black bars, rather than filling the screen")
    ap.add_argument("--fill", action="store_true", help=argparse.SUPPRESS)  # the default now
    ap.add_argument("--fps", type=int, default=None,
                    help="frames a second; the source's up to 24 if not given")
    ap.add_argument("--quality", type=int, default=2,
                    help="JPEG quality, ffmpeg's -q:v: 2 (best, the default) to 31")
    ap.add_argument("--saturation", type=float, default=1.5,
                    help="colour, 1.0 as the source has it (default 1.5)")
    ap.add_argument("--lift", type=float, default=1,
                    help="raise the darkest shadows, black staying black: 0 for none, "
                         "2 for a very dark film (default 1)")
    ap.add_argument("--black", type=float, default=0,
                    help="lumas up to this (of 255) become black, the rest stretched. "
                         "Crushes shadows: a dim face loses its eyes")
    ap.add_argument("--sharpen", type=float, default=0.5,
                    help="unsharp mask after scaling down, 0 for none")
    ap.add_argument("--denoise", type=float, default=0,
                    help="smooth film grain before it is compressed (2 is light)")
    ap.add_argument("--loud", default="-14", metavar="LUFS",
                    help="bring the sound to this loudness, evening it out "
                         "(default -14, for a small speaker); off for as it is")
    ap.add_argument("--audio", default=None,
                    help="the sound track: a language (eng, ita) or a number from 0; "
                         "English if there is some, else the first")
    ap.add_argument("--mono", action="store_true",
                    help="mono sound: a sixth smaller, and the speaker is mono anyway")
    ap.add_argument("--rate", type=int, default=None,
                    help="sound samples a second; the source's own by default "
                         "(48000 from YouTube), so it is not resampled")
    ap.add_argument("--crop", default="auto",
                    help="auto finds the source's own black bars and cuts them; "
                         "none keeps the picture whole; or W:H:X:Y")
    ap.add_argument("--size", default=DEFAULT_SIZE,
                    help="WxH of the screen the picture has to fit")
    ap.add_argument("--slices", type=int, default=DEFAULT_SLICES,
                    help="horizontal strips per frame, decoded one per core")
    ap.add_argument("--start", default=None, help="where to begin, e.g. 1:30")
    ap.add_argument("--length", default=None, help="how much to take, e.g. 20 or 2:00")
    ap.add_argument("--cpu", action="store_true",
                    help="decode on the processor even where the graphics card could")
    ap.add_argument("-q", "--quiet", action="store_true", help="no progress line")
    args = ap.parse_args()

    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        raise SystemExit("mkvideo needs ffmpeg and ffprobe")
    out_path = args.output or os.path.splitext(args.input)[0] + ".ptv"
    m = re.fullmatch(r"(\d+)x(\d+)", args.size)
    if not m:
        raise SystemExit("--size wants WxH, like 320x240")
    sw, sh = int(m.group(1)), int(m.group(2))
    if sw % 8 or sh % 8:
        raise SystemExit("--size wants multiples of 8: the decoder works in blocks")
    if args.slices < 1 or sh % (8 * args.slices):
        raise SystemExit(f"--slices must divide the height into whole blocks of 8; "
                         f"{sh} does not divide by {args.slices}")
    args.loud = None if args.loud.lower() in ("off", "none") else float(args.loud)

    src = Source(args.input)
    track = src.track(args.audio)
    if args.fps is None:
        args.fps = pick_fps(src.fps())
    if not 1 <= args.fps <= 120:
        ap.error("--fps must be between 1 and 120")
    rate = sound_rate(args.fps, args.rate or src.sound_rate(track))
    channels = 1 if args.mono else 2
    if src.hdr():
        print("note: an HDR source; its colours will look flat", file=sys.stderr)

    crop = None
    if args.crop == "auto":
        crop = detect_crop(args.input, src, seconds(args.start), seconds(args.length))
    elif args.crop != "none":
        crop = tuple(map(int, args.crop.split(":")))
        if len(crop) != 4:
            raise SystemExit("--crop wants W:H:X:Y")
    cw, ch = (crop[0], crop[1]) if crop else (src.w, src.h)
    w, h = picture_size(cw * src.sar, ch, sw, sh, args.fit)

    total = seconds(args.length) or max(0.0, src.duration - (seconds(args.start) or 0))
    out_dir = os.path.dirname(os.path.abspath(out_path))
    # The strips go beside the clip, not in /tmp, which is memory on many
    # PCs: an episode's are more than a gigabyte.
    with tempfile.TemporaryDirectory(prefix=".mkvideo-", dir=out_dir) as tmp:
        names = [os.path.join(tmp, f"v{i}.mjpeg") for i in range(args.slices)]
        audio = os.path.join(tmp, "a.raw") if track is not None else None
        # Square pixels only: the card's scaler knows nothing of the rest.
        gpu = None if args.cpu or src.sar != 1.0 else vaapi_device()
        if gpu:
            # A two-second try: a card or a codec it cannot do falls back.
            try:
                run(picture_command(args, src, crop, w, h, names, gpu, probe=True))
            except subprocess.CalledProcessError:
                gpu = None
        started = time.monotonic()
        sound = None
        if audio:
            sound = subprocess.Popen(sound_command(args, track, rate, audio),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            convert(picture_command(args, src, crop, w, h, names, gpu), total, args.quiet)
        except subprocess.CalledProcessError as e:
            if sound:
                sound.kill()
            raise SystemExit(f"ffmpeg failed:\n{e.stderr.strip()}")
        if sound and sound.wait():
            raise SystemExit(f"ffmpeg failed on the sound:\n{sound.stderr.read().strip()}")
        count = write_clip(out_path, w, h, args.fps, rate, channels, names, audio)
        took = time.monotonic() - started

    size = os.path.getsize(out_path)
    secs = count / args.fps
    sound = f"{'mono' if args.mono else 'stereo'} sound" if audio else "silent"
    print(f"{out_path}: {clock(secs)}, {w}x{h} at {args.fps} fps, {sound}, "
          f"{size / 1048576:.0f} MB ({size / max(secs, 0.001) / 1024:.0f} KB/s); "
          f"made in {clock(took)} ({secs / max(took, 0.001):.0f}x, "
          f"{'graphics card' if gpu else 'processor'})")


if __name__ == "__main__":
    main()
