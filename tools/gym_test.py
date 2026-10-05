#!/usr/bin/env python3
"""gym's files and what it suggests, on the host (tools/host_pt.c), ASan/UBSan.

`gym plan` prints each exercise of a scheda with the aim for today, which
is where double progression is decided: every set made at the working
weight is a step up; most of them short of the range a step down; else
the same weight and a rep more. Each of those is set up here in a log
and checked, with reps-only, timed and "max" exercises, drop sets, the
most-used weight among mixed ones, files with CRLF and no last newline,
and `gym last`.
"""
import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PLAN = """# a comment, and a blank line after it

[A] push
Bench press      4x10 +2.5
Shoulder press   4x10 +5
Lateral raise    3x12 +1
Push-ups         3x15
Plank            3x45s
Dips             3xmax
Fly              3x12 +5
Squat            4x8 +10
Cable row        4x12 +5

[B] pull
Lat pulldown     4x10 +5"""

LOG = """2026-09-28 A
Bench press: 37.5x10 37.5x9 37.5x9 37.5x8
Fly: 20x12 20x12 20x12

2026-10-01 A
Bench press: 40x10 40x10 40x10 40x10
Shoulder press: 30x10 30x9 30x8 30x8
Lateral raise: 8x7 8x6 8x8
Push-ups: x15 x14 x12
Plank: 45s 40s 50s
Dips: x9 x7 x6
Squat: 60x8 70x8 70x8 70x7+60x4"""

AIMS = {
    "Bench press": "up: 42.5 kg, 8-10 reps (all 10 made at 40)",
    "Shoulder press": "30 kg again: a rep more, up to 10 each set",
    "Lateral raise": "down: 7 kg, build back to 12 reps",
    "Push-ups": "aim 16: one more than last time's best",
    "Plank": "aim 55 s, 5 more than last time",
    "Dips": "aim 10: one more than last time's best",
    "Fly": "up: 25 kg, 10-12 reps (all 12 made at 20)",
    "Squat": "70 kg again: a rep more, up to 8 each set",
    "Cable row": "first time: a weight you lift 12 times, 1-2 left",
}


def build(tmp):
    exe = os.path.join(tmp, "host_pt")
    sources = [os.path.join(HERE, "host_pt.c")] + [os.path.join(ROOT, p) for p in (
        "bin/util.c", "kernel/match.c", "bin/dates.c", "bin/pick.c", "bin/gym.c")]
    subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-sign-compare",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", os.path.join(ROOT, "kernel", "include"), "-I", os.path.join(ROOT, "bin"),
                    "-o", exe, *sources, "-lm"], check=True)
    gym = os.path.join(tmp, "gym")
    os.symlink(exe, gym)
    return gym


def run(gym, home, *args):
    return subprocess.run([gym, *args], capture_output=True, text=True, timeout=20,
                          env={**os.environ, "HOME": home})


def main():
    with tempfile.TemporaryDirectory(prefix="pico-gym-test-") as tmp:
        gym = build(tmp)
        home = os.path.join(tmp, "home")
        os.makedirs(home)

        # a first start writes a program to begin from, which reads back
        out = run(gym, home, "plan")
        assert out.returncode == 0 and out.stdout.startswith("[A] chest, shoulders, triceps\n"), out
        assert "Bench press  4x10\n  first time" in out.stdout, out.stdout
        print("PASS a first start: the example program, read back")

        for name, newline in (("LF", "\n"), ("CRLF, no last newline", "\r\n")):
            with open(os.path.join(home, "gym", "schede.txt"), "w", newline="") as f:
                f.write(PLAN.replace("\n", newline) + ("" if newline == "\r\n" else "\n"))
            with open(os.path.join(home, "gym", "log.txt"), "w", newline="") as f:
                f.write(LOG.replace("\n", newline))
            out = run(gym, home, "plan", "A")
            assert out.returncode == 0, out
            lines = out.stdout.splitlines()
            assert lines[0] == "[A] push", lines[0]
            got = {}
            for i, line in enumerate(lines[1:], 1):
                if not line.startswith(" "):
                    exercise = line.rsplit("  ", 1)[0]
                elif not line.startswith("  last,"):
                    got[exercise] = line.strip()
            assert got == AIMS, "\n".join(f"{k}: {got.get(k)!r} != {v!r}"
                                          for k, v in AIMS.items() if got.get(k) != v)
            assert "  last, " in out.stdout and "40x10 40x10 40x10 40x10" in out.stdout
            print(f"PASS the aims, {len(AIMS)} exercises ({name})")

        # with no scheda named, the one after the last done: B after A
        out = run(gym, home, "plan")
        assert out.stdout.startswith("[B] pull\n"), out.stdout
        out = run(gym, home, "plan", "Z")
        assert out.returncode == 1 and "no such scheda" in out.stderr, out
        print("PASS the next scheda in turn, and one that is not there")

        out = run(gym, home, "last", "1")
        assert out.stdout == ("2026-10-01 A\n  Bench press: 40x10 40x10 40x10 40x10\n"
                              "  Shoulder press: 30x10 30x9 30x8 30x8\n  Lateral raise: 8x7 8x6 8x8\n"
                              "  Push-ups: x15 x14 x12\n  Plank: 45s 40s 50s\n  Dips: x9 x7 x6\n"
                              "  Squat: 60x8 70x8 70x8 70x7+60x4\n"), out.stdout
        print("PASS gym last")
    print("gym: double progression decided as it should be, files read either way (ASan/UBSan)")


if __name__ == "__main__":
    main()
