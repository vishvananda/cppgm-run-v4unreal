# PA2 implementation plan

Stage base commit: `a857f68d973bbb77bfaed770254340614fc36f50`
Last reviewed commit: `a857f68d973bbb77bfaed770254340614fc36f50`

## Design/spec alignment

Reuse PA1's immutable-buffer, streaming `PPTokenizer` cursor; post-tokenize each preprocessing-token in one callback without retaining token vectors. Buffer only a maximal adjacent string-literal sequence until its boundary, then decode, concatenate, and emit once. Literal grammar/type/range checks and UTF encoders are local typed helpers; output remains the explicit text adapter. Keep later cumulative frontend/tool architecture unchanged.

## Remaining behavior groups

1. Identifier/simple/operator classification, pp-number grammar, integer/floating candidate types and ABI bytes.
2. Character/UD-character decoding and Unicode/code-unit validation.
3. Nonraw/raw strings, escapes, UCNs, encoding prefixes and phase-6/UD concatenation.

Owner/data flow: `posttoken.cpp` owns PA2 conversion/output and consumes borrowed `IPPTokenStream` spellings from `PPTokenizer`; only pending adjacent string spellings are copied, then released at a boundary. Expected linear time in input bytes/tokens and output code units; arbitrary-precision integer overflow will be detected while scanning, not via exceptions or whole-token conversions.

## Performance and validation

Streaming tokenizer/converter expected O(source bytes + emitted code units), O(maximal string run + one token) temporary memory. Record build wall time/peak RSS and executable/text size after implementation; no runtime output executable is produced by this tool. Required: `make test-pa2`, through-PA2 report, file audit, and explicit `student.tests/` probes. No optimization claims.

## Handoff ledger

- Unfinished implementation: none intended; update based on any remaining required fixture failures.
- Independent audit: verify broad C++11 edge grammar beyond checked fixtures, maximal adjacent-string boundaries, and performance evidence/protocol; do not waive requirements.
- Stage handoff boundary: PA2 token semantics only; future cumulative language phases remain untouched.
