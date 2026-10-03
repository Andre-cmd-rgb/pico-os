#!/usr/bin/env python3
"""Check actual AI file reads for bounded context and allocation failures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "bin/ai.c").read_text()


def between(start, end):
    return start + source.split(start, 1)[1].split(end, 1)[0]


functions = between("static char *slurp(", "static int spill(")
functions += between("static void tool_read(", "static void tool_write(")
functions += between("static void tool_edit(", "static void job_write(")

with tempfile.TemporaryDirectory(prefix="pico-ai-file-read-") as tmp:
    (Path(tmp) / "ai_file_read_under_test.h").write_text(functions)
    exe = str(Path(tmp) / "ai_file_read_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, "-I", str(ROOT / "bin"), "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/ai_file_read_test.c"), str(ROOT / "bin/json.c"),
        "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
