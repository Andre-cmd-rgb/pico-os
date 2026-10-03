#!/usr/bin/env python3
"""Check the real USB output keeps writer ownership past the old timeout."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "drivers/input/serial.c").read_text()
lock = "static void out_lock(" + source.split("static void out_lock(", 1)[1].split(
    "#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG", 1)[0]
write = "static void serial_out(" + source.split("static void serial_out(", 1)[1].split(
    "static int serial_read(", 1)[0]
init = "int serial_console_init(" + source.split("int serial_console_init(", 1)[1]
with tempfile.TemporaryDirectory(prefix="pico-serial-") as tmp:
    (Path(tmp) / "serial_output_under_test.h").write_text(lock + write + init)
    exe = str(Path(tmp) / "serial_output_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
        "-I", tmp, str(ROOT / "tools/serial_output_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
