#!/usr/bin/env python3
"""Fetch a video from the web and make it playable on PocketType.

    python3 tools/ytgrab.py https://youtu.be/...            (or: make yt URL=...)
    python3 tools/ytgrab.py URL --length 60 --fps 24
    python3 tools/ytgrab.py URL --push                      send it to the board

The screen is 320x240, but the stream fetched is 480p: the 240p ones are
compressed to mush before they get here, and a film posted with black
bars round it (a 4:3 video on a 16:9 page) has only its middle to give
once mkvideo.py cuts the bars. --small takes the 240p one when every
megabyte counts (on mobile data). What it costs is printed before
anything is downloaded.

The conversion itself is tools/mkvideo.py. Anything it understands can be
passed through here.

Needs yt-dlp and ffmpeg on PATH. yt-dlp handles a great many sites, not
only YouTube, and the URL is passed to it unchanged.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(os.path.dirname(HERE), "clips")

# The stream nearest 480 lines, or with --small nearest 240; `+size`
# breaks ties towards the smaller file.
FORMAT = ["-S", "res:480,+size,+br", "-f", "bv*+ba/b"]
SMALL = ["-S", "res:240,+size,+br", "-f", "bv*+ba/b"]


def need(tool):
    from shutil import which

    if not which(tool):
        raise SystemExit(f"{tool} is not installed; `sudo pacman -S {tool}` on Arch")


def human(n):
    return "unknown size" if not n else f"{n / 1e6:.1f} MB"


def peek(url):
    """Title, duration and rough size, without downloading the video."""
    out = subprocess.run(["yt-dlp", "-J", "--no-warnings", *FORMAT, url],
                         capture_output=True, text=True)
    if out.returncode:
        raise SystemExit(out.stderr.strip().split("\n")[-1] if out.stderr else
                         "yt-dlp could not read that link")
    info = json.loads(out.stdout)
    size = sum(f.get("filesize") or f.get("filesize_approx") or 0
               for f in info.get("requested_formats", [info]))
    return info.get("title", "video"), info.get("duration") or 0, size


def seconds(t):
    """0:30, 1:02:03 or 45 as a number of seconds."""
    parts = str(t).split(":")
    total = 0.0
    for p in parts:
        total = total * 60 + float(p or 0)
    return total


def safe_name(title):
    keep = [c if c.isalnum() or c in " -_" else "-" for c in title]
    return "".join(keep).strip().replace(" ", "-").lower()[:48] or "clip"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url")
    ap.add_argument("--out", default=DEFAULT_OUT, help="where the .ptv goes")
    ap.add_argument("--name", default=None, help="what to call it")
    ap.add_argument("--start", default=None, help="skip to here first, e.g. 1:30")
    ap.add_argument("--length", default=None, help="how many seconds to keep")
    ap.add_argument("--fps", default=None)
    ap.add_argument("--size", default=None)
    ap.add_argument("--quality", default=None)
    ap.add_argument("--push", action="store_true", help="send it to the board when done")
    ap.add_argument("--port", default=None, help="the board's serial port")
    ap.add_argument("--keep", action="store_true", help="keep the downloaded original")
    ap.add_argument("--small", action="store_true",
                    help="fetch the 240p stream: less to download, a softer picture")
    ap.add_argument("--fill", action="store_true",
                    help="fill the screen, cutting what does not fit, not letterbox")
    args = ap.parse_args()
    if args.small:
        FORMAT[:] = SMALL

    need("yt-dlp")
    need("ffmpeg")

    title, duration, size = peek(args.url)
    mins = f"{int(duration) // 60}:{int(duration) % 60:02d}" if duration else "unknown length"
    print(f"{title}  ({mins}, about {human(size)} to download)")

    os.makedirs(args.out, exist_ok=True)
    name = args.name or safe_name(title)
    ptv = os.path.join(args.out, name + ".ptv")

    with tempfile.TemporaryDirectory() as tmp:
        target = os.path.join(args.keep and args.out or tmp, name + ".%(ext)s")
        cmd = ["yt-dlp", "--no-warnings", "--no-playlist", *FORMAT, "-o", target]
        if args.start or args.length:
            # A section rather than the whole thing, so a long video costs little.
            start = seconds(args.start or 0)
            end = start + seconds(args.length) if args.length else duration or start + 60
            cmd += ["--download-sections", f"*{start:.0f}-{end:.0f}",
                    "--force-keyframes-at-cuts"]
        cmd.append(args.url)
        print("downloading...")
        if subprocess.run(cmd).returncode:
            raise SystemExit("yt-dlp failed")

        got = [os.path.join(d, f) for d in (args.out if args.keep else tmp,)
               for f in os.listdir(d) if f.startswith(name + ".") and not f.endswith(".ptv")]
        if not got:
            raise SystemExit("nothing was downloaded")
        source = max(got, key=os.path.getsize)

        convert = [sys.executable, os.path.join(HERE, "mkvideo.py"), source, ptv]
        # The section is already cut, so start and length are not passed on.
        for flag in ("fps", "size", "quality"):
            if getattr(args, flag) is not None:
                convert += [f"--{flag}", str(getattr(args, flag))]
        if args.fill:
            convert.append("--fill")
        if subprocess.run(convert).returncode:
            raise SystemExit("converting failed")

    if args.push:
        port = args.port or first_port()
        print(f"sending to {port}...")
        # "~/" is the board's home, whatever its user is called: xfer.py
        # hands it to the board's shell as $HOME.
        if subprocess.run([sys.executable, os.path.join(HERE, "xfer.py"), "push", port,
                           ptv, "~/video/" + os.path.basename(ptv)]).returncode:
            raise SystemExit(f"sending failed; the clip is in {ptv}")
    else:
        print("copy it to the card's video/ directory, or pass --push")


def first_port():
    import glob

    ports = sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/ttyUSB*"))
    if not ports:
        raise SystemExit("no board found: plug it in, or pass --port")
    return ports[0]


if __name__ == "__main__":
    main()
