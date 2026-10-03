#!/usr/bin/env python3
"""Inject failures into the production LCD startup and both bus lifecycles."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


panel = (ROOT / "drivers/video/ili9341.c").read_text()
sequence = panel[panel.index("#if CONFIG_PT_LCD_INIT_ALT"):panel.index("static esp_lcd_panel_io_handle_t io;")]
startup = "".join(function(panel, sig) for sig in [
    "int lcd_init(void)", "void lcd_panel_power(bool on)", "bool lcd_panel_on(void)",
    "int lcd_read_reg(", "int lcd_set_rotation(", "int lcd_set_clock(",
])
with tempfile.TemporaryDirectory(prefix="pico-lcd-init-") as tmp:
    directory = Path(tmp)
    (directory / "lcd_init_under_test.h").write_text(sequence + startup)
    for bus in ["spi", "i80"]:
        source = (ROOT / f"drivers/video/io_{bus}.c").read_text()
        (directory / f"{bus}_under_test.h").write_text("".join(function(source, sig) for sig in [
            "int lcd_io_open(", "int lcd_io_close(",
        ]))
    for name, flags in [("spi", []), ("i80", ["-DTEST_I80=1"]),
                        ("i80-rd", ["-DTEST_I80=1", "-DCONFIG_PT_LCD_RD=13"]),
                        ("spi-standard", ["-DCONFIG_PT_LCD_INIT_ALT=0"]),
                        ("spi-no-miso", ["-DCONFIG_PT_LCD_MISO=-1"]),
                        ("spi-reset", ["-DCONFIG_PT_LCD_SPI_RST=41"])]:
        exe = str(directory / name)
        subprocess.run([
            "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-I", tmp, "-I", str(ROOT / "drivers/video"),
            *flags, str(ROOT / "tools/lcd_init_test.c"), "-o", exe,
        ], check=True)
        subprocess.run([exe], check=True, timeout=10)
    disabled = directory / "disabled.c"
    disabled.write_text(
        "#include <assert.h>\n#include <errno.h>\n#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n" +
        panel.split("#else /* !CONFIG_PT_LCD */", 1)[1].rsplit("#endif", 1)[0] +
        "int main(void) { assert(lcd_init() == -ENODEV); assert(!lcd_width() && !lcd_height()); "
        "assert(!lcd_panel_on()); lcd_panel_power(true); assert(lcd_set_rotation(0) == -ENODEV); return 0; }\n"
    )
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    str(disabled), "-o", str(directory / "disabled")], check=True)
    subprocess.run([str(directory / "disabled")], check=True)
