#!/usr/bin/env python3
"""Exercise production idle screen transitions with rejected LCD commands."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


source = (ROOT / "drivers/power/idle.c").read_text()
tested = "".join(function(source, signature) for signature in [
    "static int dim_level(void)", "static bool set_screen(",
    "static TickType_t idle_wait_ticks(", "void power_activity(void)",
])
with tempfile.TemporaryDirectory(prefix="pico-idle-screen-") as tmp:
    directory = Path(tmp)
    (directory / "idle_screen_under_test.h").write_text(tested)
    exe = str(directory / "idle-screen")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, str(ROOT / "tools/idle_screen_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
