#!/usr/bin/env python3
"""Exercise the production native scanout against a simulated panel clock."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "drivers/video/ili9341.c").read_text()
constants = source.split("#define NATIVE_W", 1)[1].split("static bool scan_down", 1)[0]
scanout = source.split("int lcd_draw_native(", 1)[1].split("/* A register read back", 1)[0]
with tempfile.TemporaryDirectory(prefix="pico-scanout-") as tmp:
    (Path(tmp) / "scanout_under_test.h").write_text("#define NATIVE_W" + constants)
    (Path(tmp) / "scanout_function.h").write_text("int lcd_draw_native(" + scanout)
    exe = str(Path(tmp) / "video_scanout_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, "-I", str(ROOT / "drivers/video"),
        str(ROOT / "tools/video_scanout_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=20)
