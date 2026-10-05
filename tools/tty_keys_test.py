#!/usr/bin/env python3
"""Check terminal/app scroll ownership and production key decoding."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "drivers/tty/tty.c").read_text()


def section(start, end):
    return start + source.split(start, 1)[1].split(end, 1)[0]


types = section("struct tty {", "EXT_RAM_BSS_ATTR static struct tty")
functions = section("static bool switch_key(", "int tty_switch(")
functions += section("static int tty_ioctl(", "static const struct pt_file_ops")

with tempfile.TemporaryDirectory(prefix="pico-tty-keys-") as tmp:
    (Path(tmp) / "tty_keys_types.h").write_text(types)
    (Path(tmp) / "tty_keys_under_test.h").write_text(functions)
    exe = str(Path(tmp) / "tty_keys_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/tty_keys_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
