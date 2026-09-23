#!/usr/bin/env python3
"""Drive the PocketType shell and check the results.

    make test                    boot build-qemu/flash.bin in QEMU
    make hwtest PORT=/dev/ttyACM0   run the same checks on a real board

On a board the checks work in /tmp/work and clean up after themselves. Opening
the port does not reset the board.

Needs Espressif's QEMU: `python $IDF_PATH/tools/idf_tools.py install qemu-xtensa`
(and `sudo pacman -S libslirp` on Arch). Set QEMU=/path/to/qemu-system-xtensa
to use a specific binary.
"""
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(ROOT), "tools"))
from board import Board, clean, find_qemu, free_port  # noqa: E402,F401

TARGET = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-qemu", "flash.bin")


def main():
    b = Board(TARGET)
    try:
        run_tests(b)
    finally:
        b.close()


def run_tests(b):
    if b.ser:
        b.send(b"\x15\r")	# a board is already running: just get a fresh prompt
    boot = clean(b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 60))
    print("=== BOOT ===")
    print(boot)

    checks = []

    def step(cmd, expect=None, reject=None, timeout=8.0, any_of=None):
        """`any_of` is for hardware: a driver that is switched off, or a part
        that is not plugged in, has to answer sensibly too, so any one of
        those answers counts."""
        out = b.run(cmd, timeout)
        ok = True
        for e in ([expect] if isinstance(expect, str) else expect or []):
            ok &= e in out
        for r in ([reject] if isinstance(reject, str) else reject or []):
            ok &= r not in out
        if any_of:
            ok &= any(e in out for e in any_of)
        checks.append((cmd, ok))
        print(f"=== {'PASS' if ok else 'FAIL'}: {cmd!r}")
        print(out)
        return out

    scratch = " ".join("/tmp/work/" + f for f in
                       "test.txt a d s.sh notes.txt notes.txt.tmp big.txt x1.txt x2.txt".split())
    step("rm -rf " + scratch + "; cd")
    step("uname -a", "PocketType pockettype 0.1.0")
    step("help", ["ls", "edit", "ps", "Commands"])
    step("help ls", "list directory contents")
    step("ls /", ["bin/", "dev/", "etc/", "home/", "mnt/", "proc/", "tmp/"], reject="flash")
    step("ls /proc", ["meminfo", "version"])
    step("cat /proc/version", "PocketType version")
    step("cat /proc/meminfo", "MemTotal")
    step("pwd", "/home/andre")
    step("mkdir -p /tmp/work && cd /tmp/work && pwd", "/tmp/work")
    step("ls -la /", ["bin/", "etc/", "home/"])
    step("echo hello > /tmp/work/test.txt; cat /tmp/work/test.txt", "hello")
    step("echo more >> test.txt; wc test.txt", " 2  2 11 test.txt")
    step("mkdir -p /tmp/work/a/b/c && ls /tmp/work/a/b", "c/")
    step("cp test.txt a/ && mv a/test.txt a/t2.txt && ls a", ["b/", "t2.txt"])
    step("rm a; echo $?", ["is a directory", "1"])
    step("rm -r a && ls", reject="a/")
    step("cat test.txt | grep mor", "more", reject="hello")
    # /proc grows a file per driver that registers one, so check the names
    step("ls /proc", ["cpuinfo", "kmsg", "meminfo", "mounts", "uptime", "version"])
    step("cat /proc/mounts", [" / ", "/tmp tmpfs"])
    step("echo keep > $HOME/.persist && cat $HOME/.persist", "keep")
    step("cat < test.txt | wc -c", "11")
    step("echo $HOME $USER", "/home/andre andre")
    step("false; echo status=$?", "status=1")
    step("true && echo yes || echo no", "yes", reject="no")
    step("false && echo yes || echo no", "no", reject="yes\n")
    step("export GREETING='hi there' && echo \"$GREETING!\"", "hi there!")
    step("env | grep GREETING", "GREETING=hi there")
    step("nosuchcmd", "command not found")
    step("echo 'echo script says $1 $#' > s.sh && sh s.sh one && ./s.sh two", ["script says one 1", "script says two 1"])
    step("touch x1.txt x2.txt && ls *.txt", ["test.txt", "x1.txt", "x2.txt"])
    step("rm x*.txt && ls", reject="x1.txt")
    step("ps", ["PID", "sh", "ps"])
    step("ps -a", "KERNEL TASK")
    step("free", ["internal", "psram"])
    step("df", ["/", "/tmp"])
    step("dmesg | head -n 3", "PocketType")
    step("uptime", "processes")
    step("date -s '2026-09-16 18:30:00' && date +%Y-%m-%dT%H:%M", "2026-09-16T18:30")
    step("cp test.txt test.txt; mv test.txt ./test.txt; cat test.txt",
         ["are the same file", "hello", "more"])
    step("mkdir d && cp -r d d/inner; rm -r d", "into itself")
    step("rm -r /tmp; rm -rf /; ls /tmp/work", ["refusing to remove /tmp", "refusing to remove /", "test.txt"])
    step("cat /proc/kmsg /proc/kmsg /proc/kmsg /proc/kmsg /proc/kmsg > big.txt; wc -c big.txt")
    step("cat big.txt | head -n 1; echo pipe=$?", ["pipe=0"], reject="Broken pipe")
    step("rm big.txt")
    step("echo gone > /dev/null; ls /dev; hexdump /dev/zero | head -n 1",
         ["null", "urandom", "zero", "00000000  00 00 00"], reject="gone")
    step("ls /nope > /dev/null 2>&1; echo quiet=$?", "quiet=2", reject="No such")
    step("export PS1='[\\W]\\$ '", None)
    step("echo prompt")
    checks[-1] = ("PS1 prompt", b"[work]$ " in b.last_raw)
    step("unset PS1", None)
    step("hexdump test.txt", ["68 65 6c 6c 6f", "|hello"])
    step("head -n 1 test.txt; tail -n 1 test.txt", ["hello", "more"])
    step("ls /nope 2>&1 | grep -c such; echo after", ["1", "after"])

    # Ctrl-C stops a foreground program
    b.send(b"sleep 30\r")
    time.sleep(1.0)
    b.send(b"\x03")
    out = clean(b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 5))
    step("echo status=$?", "status=130")

    # background job, then kill it
    out = step("sleep 60 &", "[1] ")
    if re.search(r"\[1\] (\d+)", out):
        step("jobs", "[1]+ Running      sleep 60")
        step("kill %1; sleep 0.3; echo reaped", "[1]  Terminated   sleep 60")

    # Ctrl-Z stops the job in front; bg and fg bring it back
    b.send(b"sleep 30\r")
    time.sleep(1.0)
    b.send(b"\x1a")
    out = clean(b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 5))
    checks.append(("ctrl-z", "[1]  Stopped      sleep 30" in out))
    print(f"=== {'PASS' if checks[-1][1] else 'FAIL'}: ctrl-z")
    print(out)
    step("ps", " T ")
    step("bg", "[1] sleep 30 &")
    step("jobs", "Running")
    b.send(b"fg\r")
    time.sleep(0.5)
    b.send(b"\x03")
    out = clean(b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 5))
    step("echo fg=$?; jobs; echo nojobs", "fg=130\nnojobs")

    # tab completion and history
    b.send(b"ech\t")
    time.sleep(0.3)
    out = step(" completed")
    checks[-1] = ("tab completion", "completed" in out and "command not found" not in out)
    b.send(b"\x1b[A")
    time.sleep(0.3)
    out = step("", "completed")
    checks[-1] = ("history up-arrow", "completed" in out)

    # editor: type, save with Ctrl-S, quit with Ctrl-Q
    b.send(b"edit notes.txt\r")
    time.sleep(1.0)
    b.send(b"first line\rsecond")
    time.sleep(0.5)
    b.send(b"\x13")
    time.sleep(0.8)
    b.send(b"\x11")
    b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 5)
    step("cat notes.txt", ["first line", "second"])

    # Esc menu in the editor
    b.send(b"edit notes.txt\r")
    time.sleep(1.0)
    b.send(b"\x1b[B\x1b[F!")
    time.sleep(0.2)
    b.send(b"\x1b")
    time.sleep(0.3)
    b.send(b"x")
    b.read_until(rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$", 5)
    step("cat notes.txt", "second!")

    # Hardware: whatever the board has, the answer must make sense. A
    # driver compiled out means the command is gone; a part that is not
    # plugged in means it says so.
    missing = "command not found"
    step("power", any_of=["cpufreq"])
    step("battery", any_of=[missing, "no cell", " V  ", "no battery sensing"])
    step("i2cdetect", any_of=[missing, "devices", "device\n", "usage: i2cdetect"])
    step("volume", any_of=[missing, "volume ", "no audio codec"])
    # a beep that works says nothing at all, so ask the shell how it went
    step("beep 440 50; echo beep=$?", any_of=[missing, "no audio codec", "beep=0"])
    step("play /nope.wav", any_of=[missing, "no audio codec", "No such file"])
    step("rec -t 1 /tmp/work/r.wav", any_of=[missing, "no audio codec", "recording"],
         timeout=12)
    step("backlight", any_of=[missing, "backlight "])
    step("ls /dev", any_of=["null", "zero"])
    step("wifi", any_of=[missing, "wifi:"])
    step("wifi scan", any_of=[missing, "networks", "network", "radio is off"], timeout=20)
    step("ntp", any_of=[missing, "no network", ":"], timeout=15)
    step("modem", any_of=[missing, "no modem", "module"])
    step("sms", any_of=[missing, "no modem", "message"])
    step("cat /proc/net", any_of=[missing, "No such file", "interface wlan0", "wifi: off"])
    step("screenshot /tmp/work/shot.bmp", any_of=[missing, "no screen", ".bmp"], timeout=20)
    step("nes /nope.nes", any_of=[missing, "no screen", "No such file", "not a ROM"])

    # the greeting a restarted login shell prints (/etc/motd)
    step("exit", "type 'help'", timeout=10)
    step("echo back", "back")
    step("cat /proc/kmsg | grep init", "restarting")

    b.run("rm -rf /tmp/work $HOME/.persist; cd")
    print("\n=== SUMMARY ===")
    failed = [c for c, ok in checks if not ok]
    for c, ok in checks:
        print(("PASS " if ok else "FAIL ") + c)
    print(f"{len(checks) - len(failed)}/{len(checks)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
