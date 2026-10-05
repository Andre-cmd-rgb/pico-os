#!/usr/bin/env python3
"""Exercise production VT allocation, rendering and the renderer's passes under sanitizers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "drivers/tty/vt.c").read_text()


def section(start, end):
    at = source.index(start)
    return source[at:source.index(end, at)]


parts = {
    "screen": section("struct screen {", "static struct screen\t screens"),
    "grid": section("static void mark_on(", "\n/*\n * One cell into a row of pixels"),
    # from the emoji painter's declaration: draw_cell() calls it
    "cell": section("static void draw_emoji(uint8_t *px", "\n/*\n * A program"),
    "holders": section("/*\n * A program that draws on the panel itself holds", "#if CONFIG_PT_STATUS_LINE"),
    "status": section("#if CONFIG_PT_STATUS_LINE", "/* The height of a line of text"),
    "render": section("static struct screen *onscreen(void)", "static void place_bar(void)"),
    "looks": section("static void place_bar(void)", "void vt_init(void)"),
    "allocation": section("static void screen_free_all(void)\n{", "/*\n * Writing to a terminal"),
    "terminals": section("/*\n * Writing to a terminal", "void vt_start_display(void)"),
}
with tempfile.TemporaryDirectory(prefix="pico-vt-render-") as tmp:
    for name, text in parts.items():
        (Path(tmp) / f"vt_{name}_under_test.h").write_text(text)
    exe = str(Path(tmp) / "vt_render_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-variable", "-Wno-unused-parameter", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-I", tmp,
        str(ROOT / "tools/vt_render_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
