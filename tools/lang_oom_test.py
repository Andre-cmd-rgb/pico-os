#!/usr/bin/env python3
"""Fail each executable-table allocation and run the real VM cleanup."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FLAGS = ["-std=gnu2x", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-sign-compare", "-DAL_NO_POOL",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-I", str(ROOT / "lang")]
with tempfile.TemporaryDirectory(prefix="pico-lang-oom-") as tmp:
    host = str(Path(tmp) / "port_host.o")
    subprocess.run(["cc", *FLAGS, "-Dmain=pico_host_main", "-c",
                    str(ROOT / "lang/port_host.c"), "-o", host], check=True)
    exe = str(Path(tmp) / "lang_oom")
    sources = ["builtins", "compile", "driver", "image", "lex", "quicken", "runtime", "vm"]
    subprocess.run(["cc", *FLAGS, str(ROOT / "tools/lang_oom_test.c"), host,
                    *(str(ROOT / "lang" / (s + ".c")) for s in sources),
                    "-Wl,--wrap=port_alloc", "-lm", "-pthread", "-o", exe], check=True)
    subprocess.run([exe], check=True, timeout=20)
