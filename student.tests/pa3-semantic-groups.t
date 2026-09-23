
// alternative tokens stay identifier-like to defined, operators elsewhere
defined and
defined (or)
defined not_eq
defined new
defined (delete)
new
delete + true
not 0
2 bitand 3
1 bitor 2
1 xor 3

// literal typing and usual arithmetic conversions
-5 < 5u
false ? 5u : -5
false ? 5/0u : -5
'π'
u'π'
U'𝄞'

// lazy evaluation still computes the conditional's static result type
true ? 5 : 5/0
false ? 5/0 : 5
0 && (1 << 64)
1 && (1 << 64)
1 || (1/0)
0 || (1/0)
false ? 1 : true ? 7u : 1/0

// invalid post-tokens are line-local failures
"not an integer"
0b10
1 + == 4
42
