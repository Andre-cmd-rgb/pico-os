#!/usr/bin/env python3
"""Exercise actual LCD capture and drawing with concurrent cleanup attempts."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "drivers/video/ili9341.c").read_text()
start = source.index("static uint8_t\t*capture;")
rect_start = source.index("static void capture_rect(", start)
draw_start = source.index("/* A rectangle's commands", rect_start)
state = source[start:rect_start]
rect = source[rect_start:draw_start]
draw = source[draw_start:source.index("/*\n * A rectangle of one colour", draw_start)]
with tempfile.TemporaryDirectory(prefix="pico-lcd-capture-") as tmp:
    (Path(tmp) / "lcd_capture_state_under_test.h").write_text(state)
    (Path(tmp) / "lcd_capture_rect_under_test.h").write_text(rect)
    (Path(tmp) / "lcd_draw_under_test.h").write_text(draw)
    exe = str(Path(tmp) / "lcd_capture_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
        "-I", tmp, str(ROOT / "tools/lcd_capture_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
