#!/usr/bin/env python3
"""Check saved chats and usage reporting with ASan and UBSan."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="pico-ai-sessions-") as tmp:
    exe = str(Path(tmp) / "ai_session_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/ai_session_test.c"),
        *(str(ROOT / "bin" / f) for f in ("ai_session.c", "ai_stats.c", "json.c")),
        "-lm", "-o", exe,
    ], check=True)
    subprocess.run([exe, str(Path(tmp) / "chats")], check=True, timeout=30)
