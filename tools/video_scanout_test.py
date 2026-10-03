#!/usr/bin/env python3
"""Exercise the production native scanout against a simulated panel and bus."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "drivers/video/ili9341.c").read_text()
# Everything from the panel's dimensions to the end of lcd_draw_native().
scanout = source.split("#define NATIVE_W", 1)[1].split("/* A register read back", 1)[0]
with tempfile.TemporaryDirectory(prefix="pico-scanout-") as tmp:
    (Path(tmp) / "scanout_under_test.h").write_text("#define NATIVE_W" + scanout)
    exe = str(Path(tmp) / "video_scanout_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, "-I", str(ROOT / "drivers/video"),
        str(ROOT / "tools/video_scanout_test.c"), "-o", exe, "-lm",
    ], check=True)
    subprocess.run([exe], check=True, timeout=120)
