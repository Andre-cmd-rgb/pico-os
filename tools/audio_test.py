#!/usr/bin/env python3
"""Check output levels and mixer headroom without the audio hardware."""
from pathlib import Path
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix="pico-audio-test-") as tmp:
    exe = str(Path(tmp) / "audio_test")
    subprocess.run([
        "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(Path(__file__).with_suffix(".c")), "-o", exe, "-lm",
    ], check=True)
    subprocess.run([exe], check=True)
