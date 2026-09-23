#!/usr/bin/env python3
"""Generate a deterministic PP-token throughput input for PA4 measurements."""
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
count = int(sys.argv[2]) if len(sys.argv) > 2 else 100000
with path.open("wb") as output:
    chunk = b"throughput_identifier\n" * 4096
    while count >= 4096:
        output.write(chunk)
        count -= 4096
    output.write(b"throughput_identifier\n" * count)
