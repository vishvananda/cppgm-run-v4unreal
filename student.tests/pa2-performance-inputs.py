#!/usr/bin/env python3
"""Generate deterministic, non-course PA2 performance inputs."""
from pathlib import Path
import sys

out = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/pa2-perf-inputs")
out.mkdir(parents=True, exist_ok=True)
(out / "many-identifiers.in").write_bytes((b"x " * 300_000) + b"\n")
(out / "malformed-number.in").write_bytes(b"1.a" + b"_" * 20_000 + b"\n")
print(out)
