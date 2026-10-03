#!/usr/bin/env python3
"""Fault-test the actual vendor palette builder, retaining its allocation guard."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "third_party/nofrendo/nofrendo.c").read_text()
start = source.index("void *nofrendo_buildpalette(")
end = source.index("\nint nofrendo_start(", start)
with tempfile.TemporaryDirectory(prefix="pico-nes-palette-") as tmp:
    folder = Path(tmp)
    (folder / "nes_palette_under_test.c").write_text(source[start:end])
    exe = folder / "test"
    subprocess.run([
        "cc", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, str(ROOT / "tools/nes_palette_test.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
