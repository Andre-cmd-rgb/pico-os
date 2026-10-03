#!/usr/bin/env python3
"""Check file scope, model records, command control and DMA alignment under sanitizers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="pico-ai-tools-") as tmp:
    exe = str(Path(tmp) / "ai_tools_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/ai_tools_test.c"),
        *(str(ROOT / "bin" / f) for f in ("ai_jobs.c", "ai_models.c", "ai_path.c", "json.c")),
        "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=20)
