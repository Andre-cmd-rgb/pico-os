#!/usr/bin/env python3
"""Exercise production network reads against POSIX sockets under sanitizers."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "drivers/net/netconsole.c"
read = source.read_text().split("static size_t strip_telnet(", 1)[1].split("static ssize_t net_write", 1)[0]
with tempfile.TemporaryDirectory(prefix="pico-netconsole-") as tmp:
    (Path(tmp) / "net_read_under_test.h").write_text("static size_t strip_telnet(" + read)
    exe = str(Path(tmp) / "netconsole_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-I", tmp,
        str(ROOT / "tools/netconsole_test.c"), "-pthread", "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
