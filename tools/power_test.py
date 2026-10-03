#!/usr/bin/env python3
"""Check CPU policy and shared screen holds with concurrent callers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
cpu = (ROOT / "drivers/power/cpufreq.c").read_text().split("static int  cur_min", 1)[1]
idle = (ROOT / "drivers/power/idle.c").read_text().split("void power_keep_screen(", 1)[1].split(
    "void idle_get(", 1)[0]
with tempfile.TemporaryDirectory(prefix="pico-power-") as tmp:
    (Path(tmp) / "power_under_test.h").write_text(
        "static int  cur_min" + cpu + "\nvoid power_keep_screen(" + idle)
    exe = str(Path(tmp) / "power_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
        "-I", tmp, str(ROOT / "tools/power_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=20)
