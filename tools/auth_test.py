#!/usr/bin/env python3
"""Check production shadow parsing with valid and damaged fields under sanitizers."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "kernel/auth.c"
parser = source.read_text().split("static int from_hex(", 1)[1].split("bool auth_is_set", 1)[0]
with tempfile.TemporaryDirectory(prefix="pico-auth-") as tmp:
    (Path(tmp) / "auth_parse_under_test.h").write_text("static int from_hex(" + parser)
    exe = str(Path(tmp) / "auth_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", tmp,
        str(ROOT / "tools/auth_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe, str(Path(tmp) / "shadow")], check=True, timeout=10)
