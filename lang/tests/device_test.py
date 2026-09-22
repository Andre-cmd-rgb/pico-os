#!/usr/bin/env python3
"""Check the a language on the device (QEMU, or a board over serial).

    python3 lang/tests/device_test.py [build-qemu/flash.bin | /dev/ttyACM0]

It types a small program into the shell, compiles and runs it both ways
(`a program.al` and `ac` then `./program`), and checks a runtime error and
the built-in help. Everything happens under $HOME and is cleaned up.
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from shell_test import Board, clean  # noqa: E402

TARGET = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-qemu", "flash.bin")

# No single quotes in the program: the shell writes it with echo '...'.
PROGRAM = [
    "struct Point { int x; int y; }",
    "int dist2(Point p) { return p.x * p.x + p.y * p.y; }",
    "int main(str[] args) {",
    "    str name = len(args) > 1 ? args[1] : \"device\";",
    "    Point p = Point{x: 3, y: 4};",
    "    printf(\"hello %s, dist2 = %d\\n\", name, dist2(p));",
    "    int[] squares = [];",
    "    for (int i = 1; i <= 5; i++) { push(squares, i * i); }",
    "    println(\"squares: \", squares);",
    "    File f = open(\"lang.txt\", \"w\");",
    "    write(f, \"written by a\\n\");",
    "    close(f);",
    "    println(\"read back: \", trim(read(open(\"lang.txt\", \"r\"))));",
    "    return 0;",
    "}",
]

BAD = [
    "int main() {",
    "    int[] a = [1, 2, 3];",
    "    println(\"before\");",
    "    println(a[9]);",
    "    return 0;",
    "}",
]


def main():
    b = Board(TARGET)
    checks = []

    def step(cmd, expect=None, reject=None, timeout=20.0):
        out = b.run(cmd, timeout)
        ok = all(e in out for e in ([expect] if isinstance(expect, str) else expect or []))
        ok &= all(r not in out for r in ([reject] if isinstance(reject, str) else reject or []))
        checks.append((cmd, ok))
        print(f"=== {'PASS' if ok else 'FAIL'}: {cmd!r}")
        print(out)
        return out

    try:
        if b.ser:
            b.send(b"\x15\r")
        print(clean(b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 60)))

        step("cd; rm -f hello.al hello bad.al lang.txt; echo ready", "ready")
        for i, line in enumerate(PROGRAM):
            b.run(f"echo '{line}' {'>' if i == 0 else '>>'} hello.al")
        for i, line in enumerate(BAD):
            b.run(f"echo '{line}' {'>' if i == 0 else '>>'} bad.al")

        step("wc -l hello.al", "hello.al")
        step("help a", "run an a program")
        step("help ac", "compile an a program")
        # compile in memory and run
        step("a hello.al andre", ["hello andre, dist2 = 25", "squares: [1, 4, 9, 16, 25]",
                                  "read back: written by a"])
        # compile to a file, then run it through the loader
        step("ac hello.al && ls -l hello", "hello")
        step("./hello board", ["hello board, dist2 = 25", "squares: [1, 4, 9, 16, 25]"])
        step("./hello; echo status=$?", "status=0")
        # runtime errors name the line and stop with a non-zero status
        step("a bad.al; echo status=$?", ["before", "bad.al:4: runtime error: index 9 out of range (length 3)",
                                          "status=1"])
        # compile errors
        step("echo 'int main() { return \"x\"; }' > bad2.al; a bad2.al; echo status=$?",
             ["bad2.al:1:21: error: expected int, got str", "status=1"])
        # spawn a command from a program and read its status back
        step("echo 'int main() { println(\"status \", run(\"echo\", \"from a\")); return 0; }' > spawn.al",
             reject="error")
        step("a spawn.al", ["from a", "status 0"])

        # Ctrl-C stops a running program
        b.send(b"echo 'int main() { int i = 0; while (true) { i++; } return 0; }' > loop.al\r")
        b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 20)
        b.send(b"a loop.al\r")
        b.read_until(rb"a loop.al", 10)
        time.sleep(2)
        b.send(b"\x03")
        print("=== ctrl-c:", clean(b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 20)))
        step("echo status=$?", "status=130")   # 128 + SIGINT: the loop stopped

        step("rm -f hello.al hello bad.al bad2.al lang.txt spawn.al loop.al; ls hello.al", "No such file")
    finally:
        b.close()

    bad = [c for c, ok in checks if not ok]
    print(f"\n{len(checks) - len(bad)}/{len(checks)} device checks passed")
    for c in bad:
        print("  failed:", c)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
