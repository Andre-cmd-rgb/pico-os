#!/usr/bin/env python3
"""Hammer a running PocketType and watch for leaks, crashes and errors.

    make stress PORT=/dev/ttyACM0 [SECONDS=600]

Each round starts ~100 processes from a script, runs pipelines, churns files
on / and /tmp, starts and kills background jobs, interrupts a command with Ctrl-C,
and every few rounds kills a process stuck in a loop with kill -9. Memory,
process count and uptime are sampled as it goes; a reboot or a panic shows up
as uptime going backwards or the ROM banner appearing.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shell_test  # noqa: E402

PROMPT = rb"\$ (\x1b\[K)?(\r\x1b\[\d+C)?$"
TROUBLE = re.compile(r"Guru Meditation|abort\(\)|ESP-ROM|panic|Out of memory|Cannot allocate|"
                     r"Resource temporarily unavailable|Broken pipe|stack overflow", re.I)


def main():
    port = sys.argv[1]
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 600
    b = shell_test.Board(port)
    b.send(b"\x15\r")
    b.read_until(PROMPT, 30)

    problems = []
    samples = []

    def run(cmd, timeout=20.0):
        out = b.run(cmd, timeout)
        if TROUBLE.search(out):
            problems.append((cmd, out.strip()[-300:]))
        return out

    def sample(label):
        out = run("free; uptime; ps")
        internal = re.search(r"internal\s+\d+\s+\d+\s+(\d+)", out)
        psram = re.search(r"psram\s+\d+\s+\d+\s+(\d+)", out)
        up = re.search(r"up (\d+):(\d+)(?::(\d+))?", out)
        procs = re.search(r"(\d+) processes", out)
        if not (internal and psram and up and procs):
            problems.append(("sample " + label, out.strip()[-300:]))
            return None
        parts = [int(x) for x in up.groups() if x is not None]
        seconds = parts[0] * 60 + parts[1] if len(parts) == 2 else parts[0] * 3600 + parts[1] * 60 + parts[2]
        s = dict(t=time.time(), internal=int(internal.group(1)), psram=int(psram.group(1)),
                 up=seconds, procs=int(procs.group(1)), label=label)
        samples.append(s)
        return s

    run("rm -f /tmp/st.tmp /tmp/st2.tmp /tmp/storm.sh")
    # a script of 100 `true` lines: one line of shell, 100 process starts
    run("echo true > /tmp/storm.sh")
    for _ in range(6):
        run("cat /tmp/storm.sh /tmp/storm.sh > /tmp/storm2.sh; mv /tmp/storm2.sh /tmp/storm.sh")
    run("head -n 100 /tmp/storm.sh > /tmp/storm2.sh; mv /tmp/storm2.sh /tmp/storm.sh")
    base = sample("start")

    start = time.time()
    rounds = spawns = forced_kills = 0
    while time.time() - start < duration:
        rounds += 1
        run("sh /tmp/storm.sh", 60)
        spawns += 100
        run("cat /proc/kmsg | grep init | wc -l")
        run("echo stress round %d > /st.tmp && cat /st.tmp >> /tmp/st2.tmp && rm /st.tmp" % rounds)
        run("ls -la / /proc /dev /mnt > /dev/null; ls /nope 2> /dev/null; hexdump /dev/urandom | head -n 2 > /dev/null")
        spawns += 8

        out = run("sleep 30 &")
        pid = re.search(r"\[(\d+)\]", out)
        if pid:
            run("kill %s" % pid.group(1))

        if rounds % 5 == 0:
            b.send(b"sleep 5\r")
            time.sleep(0.5)
            b.send(b"\x03")
            b.read_until(PROMPT, 10)
            out = run("echo interrupted=$?")
            if "interrupted=130" not in out:
                problems.append(("ctrl-c", out.strip()))
            run("rm -f /tmp/st2.tmp")

        if rounds % 8 == 0:
            out = run("bench hog &")
            pid = re.search(r"\[(\d+)\]", out)
            if pid:
                time.sleep(1.0)
                run("kill -9 %s" % pid.group(1))
                time.sleep(1.0)
                out = run("ps")
                # still running is a failure; a zombie waiting for the shell
                # to reap it, or the kernel's "task deleted" line, is not
                if re.search(r"^\s*%s\s+\d+\s+R\s" % pid.group(1), out, re.M):
                    problems.append(("kill -9 hog", out.strip()))
                forced_kills += 1

        if rounds % 4 == 0:
            s = sample("round %d" % rounds)
            if s and samples and len(samples) > 1 and s["up"] < samples[-2]["up"]:
                problems.append(("reboot", "uptime went from %d to %d s" % (samples[-2]["up"], s["up"])))
            print("round %4d  %6d spawns  internal %3d KB  psram %4d KB  procs %d  up %ds" % (
                rounds, spawns, s["internal"], s["psram"], s["procs"], s["up"]) if s else "round %d: sample failed" % rounds,
                flush=True)

    run("rm -f /tmp/st.tmp /tmp/st2.tmp /tmp/storm.sh")
    time.sleep(2)
    end = sample("end")
    dmesg = run("dmesg | tail -n 8")
    b.close()

    print("\n=== STRESS SUMMARY ===")
    print("duration      %d s, %d rounds" % (time.time() - start, rounds))
    print("processes     about %d started (%.1f per second)" % (spawns, spawns / (time.time() - start)))
    print("kill -9       %d runaway processes removed" % forced_kills)
    if base and end:
        print("internal RAM  %d KB free at start, %d KB at end (min %d KB)" % (
            base["internal"], end["internal"], min(s["internal"] for s in samples)))
        print("PSRAM         %d KB free at start, %d KB at end (min %d KB)" % (
            base["psram"], end["psram"], min(s["psram"] for s in samples)))
        print("uptime        %d s at start, %d s at end: %s" % (
            base["up"], end["up"], "no reboot" if end["up"] >= base["up"] else "REBOOTED"))
    print("problems      %d" % len(problems))
    for cmd, out in problems[:10]:
        print("  - %s: %s" % (cmd, out.replace("\n", " | ")[:200]))
    print("dmesg tail:\n" + dmesg.strip())
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
