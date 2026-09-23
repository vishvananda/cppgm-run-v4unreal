#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CXX="$ROOT/dev/cppgm++"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
case_no=0
run_case() {
  name=$1 expected=$2 fragment=${3-}
  src="$TMP/$name.cpp" out="$TMP/$name.out"
  cat >"$src"
  set +e
  "$CXX" --emit-semantics -o "$out" "$src" >"$TMP/$name.stdout" 2>"$TMP/$name.stderr"
  status=$?
  set -e
  if [ "$status" -ne "$expected" ]; then
    echo "$name: expected status $expected, got $status" >&2
    cat "$TMP/$name.stderr" >&2
    exit 1
  fi
  if [ -n "$fragment" ] && ! grep -Fq "$fragment" "$out"; then
    echo "$name: expected output fragment '$fragment'" >&2
    cat "$out" >&2
    exit 1
  fi
  case_no=$((case_no+1))
}

# [stmt.for], [stmt.select]: declarations belong to the statement scope only.
run_case for_escape 1 <<'CPP'
int f() { for (int i=0; i<1; ++i) {} return i; }
CPP
run_case condition_escape 1 <<'CPP'
int f() { if (int x=1) {} return x; }
CPP
run_case loop_scope_reuse 0 <<'CPP'
int f() { for (int i=0; i<1; ++i) {} int i=2; return i; }
CPP
run_case condition_visible_in_arms 0 <<'CPP'
int f(int p) { if (int x=p) return x; else return x; }
CPP
run_case unbraced_sibling_declarations 0 <<'CPP'
void f(bool condition) { if (condition) int x=1; else int x=2; }
CPP
run_case unbraced_declaration_escape 1 <<'CPP'
int f(bool condition) { if (condition) int x=1; return x; }
CPP
run_case unbraced_do_declaration_escape 1 <<'CPP'
int f() { do int x=1; while(false); return x; }
CPP

# C++ ordinary-name hiding must not resurrect an unrelated TU template.
run_case template_hidden_by_object 1 <<'CPP'
template<class T> void f(T);
void g() { int f=0; f(1); }
CPP
run_case unqualified_template_demand 0 'callee id function of (int) returning int' <<'CPP'
template<class T> T id(T value) { return value; }
int g() { return id(3); }
CPP
run_case qualified_template_demand 0 'callee n::id function of (int) returning int' <<'CPP'
namespace n { template<class T> T id(T value) { return value; } }
int g() { return n::id(3); }
CPP

# [over.ics.rank]/[over.ics.ref]: reference binding, qualification, pointer,
# promotion, conversion, and ellipsis ordering.
run_case reference_value_ambiguity 1 <<'CPP'
void f(int); void f(const int&); void g(int value) { f(value); }
CPP
run_case pointer_qualification_precedes_void 0 'callee f function of (pointer to const int) returning void' <<'CPP'
void f(const int*); void f(void*); void g(int* p) { f(p); }
CPP
run_case reference_conversion_precedes_ellipsis 0 'callee f function of (lvalue-reference to const long int) returning void' <<'CPP'
void f(const long&); void f(...); void g() { f(1); }
CPP
run_case rvalue_reference_tie 0 'callee f function of (rvalue-reference to int) returning void' <<'CPP'
void f(int&&); void f(const int&); void g() { f(1); }
CPP

# [lex.ccon]/[lex.icon] and literal array code-unit counts.
run_case character_literal_type 0 'callee f function of (char) returning void' <<'CPP'
void f(char); void f(int); void g() { f('x'); }
CPP
run_case integer_literal_boundary 0 'callee f function of (long int) returning void' <<'CPP'
void f(int); void f(long); void g() { f(2147483648); }
CPP
run_case leading_dot_floating_literal 0 'literal prvalue double .5' <<'CPP'
double f() { return .5; }
CPP
run_case fixed_enum_underlying_promotion 0 'binary-expression prvalue unsigned long int OP_PLUS:+' <<'CPP'
enum E : unsigned long { value = 1 };
unsigned long f() { return value + 1; }
CPP
run_case escaped_string_bound 0 <<'CPP'
void f(const char (&)[3]); void g() { f("a\n"); }
CPP

# [expr.sizeof] and [stmt.switch] semantic constraints.
run_case sizeof_void_rejected 1 <<'CPP'
int f() { return sizeof(void); }
CPP
run_case duplicate_case_rejected 1 <<'CPP'
int f(int x) { switch(x) { case 1: return 1; case 1: return 2; } }
CPP
run_case duplicate_default_rejected 1 <<'CPP'
int f(int x) { switch(x) { default: return 1; default: return 2; } }
CPP
run_case sizeof_reference_case_collision 1 <<'CPP'
int f(int x) { switch(x) { case sizeof(int&): return 1; case 4: return 2; } }
CPP
run_case nested_switch_labels 0 <<'CPP'
int f(int x) { switch(x) { case 1: switch(x) { case 1: return 2; } return 0; } return 0; }
CPP
run_case case_labels_convert_to_switch_type 1 <<'CPP'
int f(unsigned int x) { switch(x) { case -1: return 1; case 4294967295U: return 2; } }
CPP
run_case switch_condition_declaration_scope 0 <<'CPP'
int f(int x) { switch(int y=x) { case 1: return y; } return 0; }
CPP

# Class/ABI facts are identity-based even when the dump's spelling is equal.
run_case unrelated_member_pointer_cast 1 <<'CPP'
struct A {}; struct B {};
typedef int A::* PA; typedef int B::* PB;
PA cast(PB value) { return static_cast<PA>(value); }
CPP
run_case global_is_not_member 1 <<'CPP'
int global_value;
struct C {};
int f(C& object) { return object.global_value; }
CPP

echo "PA7 independent reducers: $case_no passed"
