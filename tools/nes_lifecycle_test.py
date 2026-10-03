#!/usr/bin/env python3
"""Exercise the actual NES port's lifecycle and faults without audio/hardware."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "emu/nes_port.c").read_text()
source = source[source.index("#define SAMPLE_RATE"):]

with tempfile.TemporaryDirectory(prefix="pico-nes-lifecycle-") as tmp:
    folder = Path(tmp)
    (folder / "nes_port_under_test.c").write_text(source)
    exe = str(folder / "nes_lifecycle_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-I", tmp,
        "-I", str(ROOT / "third_party/nofrendo"),
        "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/nes_lifecycle_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
