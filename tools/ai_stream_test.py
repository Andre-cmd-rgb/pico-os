#!/usr/bin/env python3
"""Exercise ai.c's request and tool lifecycle with mocked network input."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "bin/ai.c").read_text()


def between(start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


types = "struct source {" + between("struct source {", "static void save_session(")
types += "struct call {" + between("struct call {", "static void msg_add(")
functions = "static int conn_all(" + between("static int conn_all(", "static void say_error(")
functions += "static void msg_add(" + between(
    "static void msg_add(", "/*\n * Too long a conversation"
)
functions += "static int integer(" + between(
    "static int integer(", "/* ------------------------------------------------------------ the tools */"
)
functions += "static void tool_run(" + between("static void tool_run(", "static void tool_job(")
functions += "static int question(" + between(
    "static int question(", "/* ------------------------------------------------------------ the voice */"
)
functions += "static int read_question(" + between("static int read_question(", "PT_COMPLETE(ai,")

with tempfile.TemporaryDirectory(prefix="pico-ai-stream-") as tmp:
    (Path(tmp) / "ai_stream_types.h").write_text(types)
    (Path(tmp) / "ai_stream_under_test.h").write_text(functions)
    exe = str(Path(tmp) / "ai_stream_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, "-I", str(ROOT / "bin"), "-I", str(ROOT / "kernel/include"),
        str(ROOT / "tools/ai_stream_test.c"), str(ROOT / "bin/json.c"),
        str(ROOT / "bin/ai_stats.c"), "-lm", "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=20)
