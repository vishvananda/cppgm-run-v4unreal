#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$root/dev/cppgm++" --emit-types -o "$tmp/good.types" "$root/student.tests/pa6-final-audit.cpp"
"${CXX:-g++}" -std=gnu++11 -Wall -I"$root/dev/src" \
  "$root/student.tests/pa6-semantic-model-api.cpp" \
  "$root/obj/dev/syntax/SemanticTypes.o" "$root/obj/dev/syntax/SyntaxArena.o" \
  -o "$tmp/semantic-model-api"
"$tmp/semantic-model-api"
grep -F 'variable pointer pointer to struct Hidden' "$tmp/good.types" >/dev/null
grep -F 'variable selected_value int' "$tmp/good.types" >/dev/null
for case in scoped-left scoped-right scoped-nested scoped-init scoped-target scoped-variable-init constexpr-invalid return-mismatch cv-void cv-void-alias void-array void-reference; do
  case "$case" in
    scoped-left) cat >"$tmp/$case.cpp" <<'SRC'
enum class E { zero = 0 };
static_assert(!(E::zero && (1 / 0)), "invalid scoped-enum bool conversion");
SRC
      ;;
    scoped-right) cat >"$tmp/$case.cpp" <<'SRC'
enum class E { one = 1 };
static_assert(E::one || (1 / 0), "invalid scoped-enum bool conversion");
SRC
      ;;
    scoped-nested) cat >"$tmp/$case.cpp" <<'SRC'
enum class E { one = 1 };
static_assert(1 || (E::one < 3), "invalid comparison in unselected operand");
SRC
      ;;
    scoped-init) cat >"$tmp/$case.cpp" <<'SRC'
enum class E { value = 1 };
int invalid = E::value;
SRC
      ;;
    scoped-target) cat >"$tmp/$case.cpp" <<'SRC'
enum class E { value = 1 };
E invalid = 1;
SRC
      ;;
    scoped-variable-init) cat >"$tmp/$case.cpp" <<'SRC'
enum class E { value = 1 };
E source;
int invalid = source;
SRC
      ;;
    constexpr-invalid) cat >"$tmp/$case.cpp" <<'SRC'
constexpr int invalid = 1 / 0;
SRC
      ;;
    return-mismatch) cat >"$tmp/$case.cpp" <<'SRC'
int incompatible(int);
double incompatible(int);
SRC
      ;;
    cv-void) cat >"$tmp/$case.cpp" <<'SRC'
const void object;
SRC
      ;;
    cv-void-alias) cat >"$tmp/$case.cpp" <<'SRC'
typedef const void CV;
CV object;
SRC
      ;;
    void-array) cat >"$tmp/$case.cpp" <<'SRC'
typedef void V;
V object[2];
SRC
      ;;
    void-reference) cat >"$tmp/$case.cpp" <<'SRC'
typedef void V;
V &object;
SRC
      ;;
  esac
  if "$root/dev/cppgm++" --emit-types -o "$tmp/$case.types" "$tmp/$case.cpp" >/dev/null 2>&1; then
    echo "expected PA6 rejection: $case" >&2
    exit 1
  fi
done
echo 'PA6 final-audit reducers passed'
