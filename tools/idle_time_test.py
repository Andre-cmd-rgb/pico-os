#!/usr/bin/env python3
"""Check actual idle timestamp helpers with concurrent callers and sanitizers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
idle = (ROOT / "drivers/power/idle.c").read_text()
helpers = idle[idle.index("/* Activity times cross cores"):idle.index("enum screen_state power_screen(")]
with tempfile.TemporaryDirectory(prefix="pico-idle-time-") as tmp:
    (Path(tmp) / "idle_time_under_test.h").write_text(helpers)
    exe = str(Path(tmp) / "idle_time_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
        "-I", tmp, str(ROOT / "tools/idle_time_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=20)
