#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp)
bin=$(mktemp)
trap 'rm -f "$out" "$bin"' EXIT
dev="$root/dev/cppgm++"
"$dev" --emit-ast -o "$out" "$root/student.tests/pa5-audit-parameter-scope.cpp"
cmp "$out" "$root/student.tests/pa5-audit-parameter-scope.expected"
"$root/student.tests/run-pa5-parser-smoke.sh"
g++ -std=gnu++11 -Wall -O2 -I"$root/dev/src" \
  "$root/student.tests/pa5-audit-location.cpp" \
  "$root/obj/dev/syntax/Parser.o" \
  "$root/obj/dev/syntax/SyntaxArena.o" \
  "$root/obj/dev/preprocess/Preprocessor.o" \
  "$root/obj/dev/preprocess/tokens/PPTokenizer.o" \
  "$root/obj/dev/preprocess/tokens/PostTokenPipeline.o" \
  "$root/obj/dev/preprocess/expressions/ControlExpression.o" \
  -o "$bin"
"$bin" "$root/student.tests/pa5-audit-location-source.cpp"
