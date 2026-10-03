#!/usr/bin/env python3
"""Exercise production process and video cleanup ordering under sanitizers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
proc = (ROOT / "kernel/proc.c").read_text()
video = (ROOT / "bin/video.c").read_text()
keys = (ROOT / "kernel/keys.c").read_text()
syscalls = (ROOT / "kernel/sys.c").read_text()
with tempfile.TemporaryDirectory(prefix="pico-cleanup-") as tmp:
    kernel = proc[proc.index("int proc_set_cleanup("):proc.index("static void teardown(")]
    kernel += proc[proc.index("void pt_exit("):proc.index("int pt_wait(")]
    kernel += proc[proc.index("static void force_kill("):proc.index("static volatile bool quiet;")]
    (Path(tmp) / "proc_under_test.h").write_text(kernel)
    tasks = video[video.index("static int task_start("):video.index("/* Where the picture goes")]
    (Path(tmp) / "video_cleanup_under_test.h").write_text(tasks)
    key_exit = syscalls[syscalls.index("ssize_t pt_read("):syscalls.index("ssize_t pt_write(")]
    key_exit += syscalls[syscalls.index("int pt_ioctl("):syscalls.index("bool pt_isatty(")]
    key_exit += keys[keys.index("static int read_byte("):keys.index("static int tilde_key(")]
    (Path(tmp) / "keys_exit_under_test.h").write_text(key_exit)
    for name in ("proc_cleanup_test", "video_cleanup_test"):
        exe = str(Path(tmp) / name)
        subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-I", tmp,
                        str(ROOT / "tools" / (name + ".c")), "-o", exe], check=True)
        subprocess.run([exe], check=True, timeout=10)
