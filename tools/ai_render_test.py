#!/usr/bin/env python3
"""Run ai.c's streaming renderer under sanitizers without the ESP network SDK."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "bin/ai.c").read_text()
renderer = source.split("enum link_state", 1)[1].split(
    "/* ------------------------------------------------------------ the conversation */", 1
)[0]
with tempfile.TemporaryDirectory(prefix="pico-ai-render-") as tmp:
    (Path(tmp) / "ai_renderer_under_test.h").write_text("enum link_state" + renderer)
    exe = str(Path(tmp) / "ai_render_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-I", tmp, "-I", str(ROOT / "bin"), "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/ai_render_test.c"), str(ROOT / "bin/json.c"),
        str(ROOT / "bin/util.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=20)
