#!/usr/bin/env python3
"""Check output levels and mixer headroom without the audio hardware."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
play = (ROOT / "bin/sink.c").read_text()
half = play[play.index("#define HALF_TAPS"):play.index("int sink_open(")]
# The mixer's walk through a stream, from drivers/audio/audio.c: the
# stream itself, pull() a frame at a time and mix_stream() a block.
audio = (ROOT / "drivers/audio/audio.c").read_text()
mixer = "".join(line + "\n" for line in audio.splitlines()
                if line.startswith(("#define BLOCK", "#define ONE")))
mixer += audio[audio.index("struct stream {"):audio.index("static i2s_chan_handle_t")]
mixer += audio[audio.index("static size_t filled("):audio.index("static void drop_queued(")]
mixer += audio[audio.index("/*\n * The stream's next frame at the rate"):
               audio.index("/* " + "-" * 60 + " the mixer */")]

with tempfile.TemporaryDirectory(prefix="pico-audio-test-") as tmp:
    exe = str(Path(tmp) / "audio_test")
    (Path(tmp) / "half_under_test.h").write_text(half)
    (Path(tmp) / "mixer_under_test.h").write_text(mixer)
    subprocess.run([
        "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", tmp,
        str(Path(__file__).with_suffix(".c")), "-o", exe, "-lm",
    ], check=True)
    subprocess.run([exe], check=True)
