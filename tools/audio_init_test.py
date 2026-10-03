#!/usr/bin/env python3
"""Inject allocation, task and bus failures into production audio startup."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
audio = (ROOT / "drivers/audio/audio.c").read_text()
codec = (ROOT / "drivers/audio/es8311.c").read_text()
with tempfile.TemporaryDirectory(prefix="pico-audio-init-") as tmp:
    present = audio.split("bool audio_present(void)", 1)[1].split("/* Playing or recording", 1)[0]
    startup = audio.split("int audio_init(void)", 1)[1].split("enum audio_out audio_output", 1)[0]
    (Path(tmp) / "audio_init_under_test.h").write_text(
        "bool audio_present(void)" + present + "int audio_init(void)" + startup
    )
    (Path(tmp) / "codec_under_test.h").write_text(
        codec[codec.index("#if CONFIG_PT_AUDIO"):]
    )
    for name, flags in [("audio", []), ("audio-jack", ["-DCONFIG_PT_AUDIO_JACK=1"]),
                        ("codec", ["-DTEST_CODEC=1"])]:
        exe = str(Path(tmp) / name)
        subprocess.run([
            "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", tmp, "-I", str(ROOT / "drivers/audio"), *flags,
            str(ROOT / "tools/audio_init_test.c"), "-o", exe,
        ], check=True)
        subprocess.run([exe], check=True, timeout=10)
