#!/usr/bin/env python3
"""Check the production SPI read with the ESP-IDF post-DMA GPIO state."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "drivers/video/io_spi.c"
read = source.read_text().split("int lcd_io_read(", 1)[1].split("/* Chip select by hand", 1)[0]
with tempfile.TemporaryDirectory(prefix="pico-lcd-io-") as tmp:
    (Path(tmp) / "lcd_read_under_test.h").write_text("int lcd_io_read(" + read)
    exe = str(Path(tmp) / "lcd_io_test")
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", tmp, str(ROOT / "tools/lcd_io_test.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True, timeout=10)
