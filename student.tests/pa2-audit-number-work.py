#!/usr/bin/env python3
"""Reduced PA2 suffix-scan case; output is deterministic, input is generated."""
import subprocess
import sys

compiler = sys.argv[1] if len(sys.argv) > 1 else "dev/posttoken"
count = 50_000
number = "1.a" + "_" * count
result = subprocess.run(
    [compiler], input=(number + "\n").encode(), stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, check=False, timeout=30)
expected = ("invalid " + number + "\neof\n").encode()
if result.returncode != 0 or result.stdout != expected or result.stderr:
    raise SystemExit("posttoken did not classify the reduced long pp-number exactly")
print("PASS: 50,000-character malformed pp-number exact-output reducer")
