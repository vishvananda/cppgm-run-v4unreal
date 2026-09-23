#!/usr/bin/env python3
"""Emit a fixed PA3 throughput workload; use python3 explicitly."""
import sys

for _ in range(200_000):
    sys.stdout.write("1 + 2 * 3 == 7 || 0\n")
