#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$root/dev/cppgm++" --emit-types -o "$tmp/types" "$root/student.tests/pa6-semantic-smoke.cpp"
grep -F 'type-alias Value const int' "$tmp/types" >/dev/null
grep -F 'type-alias Grid array of 3 const int' "$tmp/types" >/dev/null
grep -F 'variable values array of 3 const int' "$tmp/types" >/dev/null
grep -F 'variable size_values array of 16 int' "$tmp/types" >/dev/null
grep -F 'variable align_values array of 8 int' "$tmp/types" >/dev/null
grep -F 'variable ref_size array of 4 int' "$tmp/types" >/dev/null
grep -F 'variable callback pointer to function of (array of 4 int) returning int' "$tmp/types" >/dev/null
grep -F 'scope block' "$tmp/types" >/dev/null
grep -F 'variable deep_value int' "$tmp/types" >/dev/null
if "$root/dev/cppgm++" --emit-types -o "$tmp/bad" "$root/student.tests/pa6-semantic-smoke-bad.cpp" >/dev/null 2>&1; then
  echo 'expected static_assert rejection' >&2
  exit 1
fi
echo 'PA6 semantic smoke tests passed'
