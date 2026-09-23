#!/usr/bin/env python3
"""Focused C++11 phase-2/raw-string boundary regressions for PA1."""
import subprocess
import sys

exe = sys.argv[1] if len(sys.argv) > 1 else "dev/pptoken"
cases = [
    ("missing final newline after token", b"a",
     b"identifier 1 a\nnew-line 0 \neof\n"),
    ("final backslash splices appended newline", b"a\\",
     b"identifier 1 a\neof\n"),
    ("final backslash after existing line", b"a\n\\",
     b"identifier 1 a\nnew-line 0 \neof\n"),
    ("backslash-newline at physical EOF appends another newline",
     b"a\n\\\n",
     b"identifier 1 a\nnew-line 0 \nnew-line 0 \neof\n"),
    ("trigraph backslash at physical EOF",
     b"a??/",
     b"identifier 1 a\neof\n"),
    ("trigraph backslash-newline at physical EOF",
     b"a??/\n",
     b"identifier 1 a\nnew-line 0 \neof\n"),
    ("raw prefix is phase-2 translated; raw body is reverted",
     b'R\\\n"(??=body)"x\n',
     b'user-defined-string-literal 13 R"(??=body)"x\nnew-line 0 \neof\n'),
    ("u8 raw prefix is phase-2 translated",
     b'u8R\\\n"(??=body)"x\n',
     b'user-defined-string-literal 15 u8R"(??=body)"x\nnew-line 0 \neof\n'),
]
for name, source, expected in cases:
    result = subprocess.run([exe], input=source, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if result.returncode != 0 or result.stdout != expected:
        sys.stderr.write("FAIL: %s\n" % name)
        sys.stderr.write("return code: %d\n" % result.returncode)
        sys.stderr.write("stdout: %r\nexpected: %r\n" %
                         (result.stdout, expected))
        sys.stderr.write("stderr: %r\n" % result.stderr)
        sys.exit(1)
print("PA1 phase-boundary checks passed (%d cases)" % len(cases))
