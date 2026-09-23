#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp)
trap 'rm -f "$out"' EXIT
dev="$root/dev/cppgm++"
"$dev" --emit-ast -o "$out" "$root/student.tests/pa5-parser-smoke.cpp"
cmp "$out" "$root/student.tests/pa5-parser-smoke.ref"
"$dev" --emit-ast -o "$out" \
  "$root/student.tests/pa5-parser-smoke.cpp" \
  "$root/student.tests/pa5-parser-smoke2.cpp"
cmp "$out" "$root/student.tests/pa5-parser-smoke-multitu.ref"
if "$dev" --emit-ast -o "$out" "$root/student.tests/pa5-parser-smoke-bad.cpp" >/dev/null 2>&1; then
  echo 'malformed input unexpectedly parsed' >&2
  exit 1
fi
