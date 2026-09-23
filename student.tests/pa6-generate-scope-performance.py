#!/usr/bin/env python3
"""Generate a fixed wide-scope/class-index PA6 semantic workload."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--count", type=int, default=3600)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if args.count < 1:
    parser.error("count must be positive")
lines = ["namespace scope_bench {"]
for i in range(args.count):
    name = "Record_%05d" % i
    lines.append("struct %s { int payload; };" % name)
    lines.append("%s object_%05d;" % (name, i))
lines.extend(["}", "using namespace scope_bench;", "Record_%05d final_object;" % (args.count - 1), ""])
args.output.write_text("\n".join(lines))
