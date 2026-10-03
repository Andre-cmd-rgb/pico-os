#!/usr/bin/env python3
"""Exercise the AI screen on the host under ASan and UBSan."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="pico-ai-test-") as tmp:
    exe = str(Path(tmp) / "ai_ui_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/ai_ui_test.c"), str(ROOT / "bin/json.c"),
        str(ROOT / "bin/util.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True)
