#!/usr/bin/env python3
"""Generate reproducible declaration/lookup-heavy PA6 frontend inputs."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument("--count", type=int, default=1800)
parser.add_argument("--output", type=Path, default=ROOT / "pa6-performance-input.cpp")
args = parser.parse_args()
if args.count < 1:
    parser.error("--count must be positive")
lines = ["namespace bench {", "typedef const unsigned long Extent;"]
for i in range(args.count):
    lines.append("constexpr unsigned long bound_%05d = %d;" % (i, i % 31 + 1))
    lines.append("typedef Extent Row_%05d[%d];" % (i, i % 7 + 1))
    lines.append("Row_%05d row_%05d[bound_%05d];" % (i, i, i))
lines.extend(["}", "using namespace bench;", "Row_%05d final_row;" % (args.count - 1), ""])
args.output.write_text("\n".join(lines))
