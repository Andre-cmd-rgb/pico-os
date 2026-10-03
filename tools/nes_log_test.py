#!/usr/bin/env python3
"""Check the actual NES logging hook under ASan/UBSan without hardware."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "emu/nes_port.c").read_text()
start = source.index("void rg_system_log(")
end = source.index("\nuint32_t rg_crc32(", start)

with tempfile.TemporaryDirectory(prefix="pico-nes-log-") as tmp:
    folder = Path(tmp)
    (folder / "nes_log_under_test.c").write_text(source[start:end])
    exe = str(folder / "nes_log_test")
    subprocess.run([
        "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-I", tmp,
        str(ROOT / "tools/nes_log_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
