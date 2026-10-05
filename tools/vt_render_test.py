#!/usr/bin/env python3
"""Exercise production VT allocation and row rendering under sanitizers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "drivers/tty/vt.c").read_text()
# from the emoji painter's declaration: draw_cell() calls it
cell_start = source.index("static void draw_emoji(uint8_t *px")
cell = source[cell_start:source.index("\n/*\n * A program", cell_start)]
screen = source[source.index("struct screen {"):source.index("static struct screen\t screens")]
# the pictures programs set over their text, which the renderer paints after it
insets_start = source.index("static struct {\n\tuint8_t\t\t*px;")
screen += source[insets_start:source.index("\n", source.index("} insets[", insets_start)) + 1]
allocation = source[source.index("static void screen_free_all(void)\n{"):source.index(
    "/*\n * Writing to a terminal")]
render = source[source.index("static const struct cell *shown_row("):source.index(
    "static void render_task(")]
switch_start = source.index("int vt_switch(")
switch = source[switch_start:source.index(
    "/*\n * A program that draws on the panel itself", switch_start)]
start = source.index("static void render_task(")
alloc_check = source[start:source.index("\n\tfor (;;) {", start)] + "\n}\n"
with tempfile.TemporaryDirectory(prefix="pico-vt-render-") as tmp:
    (Path(tmp) / "vt_draw_cell_under_test.h").write_text(cell)
    (Path(tmp) / "vt_screen_under_test.h").write_text(screen)
    (Path(tmp) / "vt_render_under_test.h").write_text(allocation + render + switch)
    (Path(tmp) / "vt_render_alloc_under_test.h").write_text(alloc_check)
    exe = str(Path(tmp) / "vt_render_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-unused-function",
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-I", tmp,
        str(ROOT / "tools/vt_render_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
