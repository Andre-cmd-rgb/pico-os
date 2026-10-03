#!/usr/bin/env python3
"""Check normal MP3 tracks and unsupported format changes without playback.

The actual wrapper and minimp3 run under ASan/UBSan. Requires cc and ffmpeg
with the libmp3lame encoder; all generated clips stay in a temporary folder.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    with tempfile.TemporaryDirectory(prefix="pico-mp3-test-") as tmp:
        folder = Path(tmp)
        decoder = folder / "minimp3.o"
        exe = str(folder / "mp3_test")
        flags = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-I", str(ROOT / "third_party/minimp3")]
        subprocess.run(["cc", *flags, "-w", "-c",
                        str(ROOT / "third_party/minimp3/decoders.c"),
                        "-o", str(decoder)], check=True)
        subprocess.run(["cc", *flags, "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "kernel/include"),
                        str(ROOT / "tools/mp3_test.c"), str(decoder),
                        "-lm", "-o", exe], check=True)
        tracks = {}
        for name, channels, rate in (("mono", 1, 44100), ("stereo", 2, 44100),
                                     ("rate32", 1, 32000)):
            path = folder / (name + ".mp3")
            subprocess.run([
                "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                f"sine=frequency=440:sample_rate={rate}:duration=1",
                "-ac", str(channels), "-codec:a", "libmp3lame", "-b:a", "64k",
                "-reservoir", "0", "-write_xing", "0", "-id3v2_version", "0",
                str(path),
            ], check=True)
            tracks[name] = path
            subprocess.run([exe, str(path), str(channels), str(rate), "1024", "ok"],
                           check=True, timeout=10)
            print("PASS", name)
        same = folder / "same-format.mp3"
        same.write_bytes(tracks["mono"].read_bytes() * 2)
        subprocess.run([exe, str(same), "1", "44100", "1024", "ok"],
                       check=True, timeout=10)
        print("PASS same-format concatenation")
        for first, second, channels in (("mono", "stereo", 1), ("stereo", "mono", 2),
                                         ("mono", "rate32", 1)):
            path = folder / (first + "-to-" + second + ".mp3")
            path.write_bytes(tracks[first].read_bytes() + tracks[second].read_bytes())
            for chunk, result in ((1152, "aligned-error"), (1001, "partial-error")):
                subprocess.run([exe, str(path), str(channels), "44100", str(chunk), result],
                               check=True, timeout=10)
                print(f"PASS {first} to {second}, {result}")
    print("mp3: 10 track/format checks passed (ASan/UBSan, no playback)")


if __name__ == "__main__":
    main()
