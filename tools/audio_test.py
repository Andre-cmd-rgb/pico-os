#!/usr/bin/env python3
"""Check output levels and mixer headroom without the audio hardware."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
play = (ROOT / "bin/audio.c").read_text()
half = play[play.index("#define HALF_TAPS"):play.index("/*\n * Everything that plays goes")]

with tempfile.TemporaryDirectory(prefix="pico-audio-test-") as tmp:
    exe = str(Path(tmp) / "audio_test")
    (Path(tmp) / "half_under_test.h").write_text(half)
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", tmp,
        str(Path(__file__).with_suffix(".c")), "-o", exe, "-lm",
    ], check=True)
    subprocess.run([exe], check=True)
