#!/usr/bin/env python3
"""Check rendered page bounds, preserving browser failures as failures."""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--chromium", default="chromium")
p.add_argument("booklets", nargs="+")
args = p.parse_args()
failed = False
for name in args.booklets:
    url = Path(name + ".html").resolve().as_uri() + "#check"
    result = subprocess.run([args.chromium, "--headless", "--disable-gpu",
                             "--virtual-time-budget=3000", "--dump-dom", url],
                            capture_output=True, text=True, timeout=60)
    checks = re.findall(r'data-check="([^"]+)"', result.stdout)
    if result.returncode or not checks:
        print(f"{name}: browser failed or produced no page checks ({result.returncode})")
        print(result.stderr[-1500:])
        failed = True
    for check in checks:
        if check.startswith("OVER"):
            print(f"{name}: {check}")
            failed = True
if failed:
    raise SystemExit(1)
print("every page fits")
