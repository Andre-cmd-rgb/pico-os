#!/usr/bin/env python3
"""Check actual AI tool argument dispatch before any file/command operation."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "bin/ai.c").read_text()
integer = "static int integer(" + source.split("static int integer(", 1)[1].split(
    "/* A piece of a tool call", 1)[0]
dispatch = "static int tool_string(" + source.split("static int tool_string(", 1)[1].split(
    "/* ------------------------------------------------------------ a question */", 1)[0]
with tempfile.TemporaryDirectory(prefix="pico-ai-dispatch-") as tmp:
    (Path(tmp) / "ai_dispatch_under_test.h").write_text(integer + dispatch)
    exe = str(Path(tmp) / "ai_dispatch_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, "-I", str(ROOT / "bin"), "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/ai_dispatch_test.c"), str(ROOT / "bin/json.c"),
        "-lm", "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
