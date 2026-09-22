#!/usr/bin/env python3
"""Exercise the shell's control flow and scripting: if/for/while/case,
functions, $((...)), $(...), test, break/continue and scripts.

    make scripttest PORT=/dev/ttyACM0
    python3 tools/script_test.py /dev/ttyACM0

Works on a board over the serial console; the same harness as shell_test.py,
so opening the port does not reset the board. Everything runs in /tmp/work
and is cleaned up.
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from shell_test import Board, clean  # noqa: E402

TARGET = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-qemu", "flash.bin")


def main():
    b = Board(TARGET)
    checks = []

    def step(cmd, expect=None, reject=None, timeout=8.0):
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

        step("rm -rf /tmp/work; mkdir -p /tmp/work; cd /tmp/work; echo ready", "ready")

        # if / elif / else
        step("if true; then echo yes; else echo no; fi", "yes", reject="no")
        step("if false; then echo yes; else echo no; fi", "no", reject="yes")
        step("if false; then echo a; elif true; then echo b; else echo c; fi", "b",
             reject=["a", "c"])

        # for
        step("for i in 1 2 3; do echo $i; done", ["1", "2", "3"])
        step("for x in a b; do for y in 1 2; do echo $x$y; done; done",
             ["a1", "a2", "b1", "b2"])

        # while / until
        step("i=0; while [ $i -lt 3 ]; do echo w$i; i=$((i+1)); done", ["w0", "w1", "w2"])
        step("i=0; until [ $i -ge 2 ]; do echo u$i; i=$((i+1)); done", ["u0", "u1"])

        # case
        step("case b in a) echo A;; b) echo B;; *) echo other;; esac", "B", reject="A")
        step("case zz in a*) echo A;; *) echo other;; esac", "other", reject="A")

        # arithmetic
        step("echo $((1+2*3))", "7")
        step("echo $((10/3)) $((10%3))", "3 1")
        step("i=2; echo $((i*i+i))", "6")

        # command substitution
        step("echo $(echo inner)", "inner")
        step("echo total=$(echo a b c | wc -w)", "total= 3")
        step("x=$(echo 42); echo got=$x", "got=42")

        # functions
        step("greet() { echo hi $1; }; greet world", "hi world")
        step("add() { echo $(($1+$2)); }; add 3 4", "7")
        step("f() { local x=hello; echo $x; }; x=world; f; echo $x", ["hello", "world"])

        # test / [
        step("[ -e /proc ] && echo exists", "exists")
        step("[ 5 -gt 3 ] && echo big", "big")
        step("[ 2 -gt 3 ] && echo big || echo small", "small", reject="big")
        step("[ -d /tmp ] && [ -f /proc/version ] && echo both", "both")

        # break / continue
        step("for i in 1 2 3 4 5; do [ $i -eq 3 ] && break; echo $i; done", ["1", "2"], reject="4")
        step("for i in 1 2 3; do [ $i -eq 2 ] && continue; echo $i; done", ["1", "3"], reject="2")

        # a multi-line script with functions, if, for and positional parameters
        SCRIPT = [
            "sum() { echo $(($1+$2)); }",
            "if [ $# -ge 2 ]; then",
            "    echo args=$#",
            "    echo sum=$(sum $1 $2)",
            "fi",
            "for f in $@; do echo arg:$f; done",
        ]
        for i, line in enumerate(SCRIPT):
            b.run(f"echo '{line}' {'>' if i == 0 else '>>'} ctl.sh")
        step("wc -l ctl.sh", "ctl.sh")
        step("sh ctl.sh 3 4 5", ["args=3", "sum=7", "arg:3", "arg:4", "arg:5"])
        step("./ctl.sh 3 4 5", ["args=3", "sum=7", "arg:3", "arg:4", "arg:5"])

        # a loop that would run forever is stopped by Ctrl-C
        b.send(b"i=0; while true; do i=$((i+1)); done\r")
        b.read_until(rb"while true", 5)
        time.sleep(1.5)
        b.send(b"\x03")
        print("=== ctrl-c:", clean(b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 20)))
        step("echo status=$?", "status=130")

        step("rm -f ctl.sh; ls ctl.sh", "No such file")
        step("cd")
    finally:
        b.close()

    failed = [c for c, ok in checks if not ok]
    print("\n=== SCRIPT SUMMARY ===")
    for c, ok in checks:
        print(("PASS " if ok else "FAIL ") + c)
    print(f"{len(checks) - len(failed)}/{len(checks)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
