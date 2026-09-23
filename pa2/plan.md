# PA2 implementation plan

Stage base commit: `a857f68d973bbb77bfaed770254340614fc36f50`
Last reviewed commit: `ebbd28aa090f7cee8a75b6726919c06a55204b46`

## Design/spec alignment

`posttoken` reuses PA1's immutable-source, streaming `PPTokenizer`. The event consumer classifies identifiers/operators and validates/decodes pp-numbers and character literals immediately. It buffers only one maximal adjacent string run for phase-6 encoding/suffix checks, then emits directly; rendered token text is only the required output adapter. UTF encoders, ABI bytes and literal candidate/range selection are explicit. Empty character tokens reach PA2 conversion and become `invalid`; PA1 behavior remains passing. No later-PA design changed.

Owners/data flow: `PPTokenizer` owns phases 1–3 and lends spellings through `IPPTokenStream`; `PostTokenStream` owns PA2 semantics/output and copies only pending string spellings. Ordinary processing is O(source bytes + tokens + emitted code units); temporary storage is O(maximal string run + one token), with no token-stream copies or text roundtrip.

## Behavior groups / status

- Complete: simple/keyword/operator and invalid classification; decimal/octal/hex integer grammar, suffixes, ABI type/range/bytes and numeric UDLs; floating grammar/decode; character/UD-character escape, Unicode and code-unit validation; raw/nonraw strings, numeric escapes, UTF-8/16/32, prefix/suffix constraints and maximal concatenation.
- No known unfinished implementation in PA2. All required fixture groups were exercised; personal edge test is under `student.tests/` and explicitly diff-checked.

## Performance evidence

Default g++ C++11 `-O3` rebuild of `posttoken` plus changed tokenizer object/link: 3.93 s, peak RSS 185,624 KiB. Tool processing the 429,984-byte `700-hard-string-concat.t`: 0.16 s, peak RSS 4,268 KiB. ELF `.text`: 152,024 bytes. PA2 emits tokens, not generated executables, so no generated-program runtime/text-size result exists. These are baseline measurements, not optimization claims; no optimization transformations or growth budget were added. Streaming work remains linear in consumed source/tokens/code units.

## Validation and handoff ledger

- `make test-pa2`: 26/26 pass; `make test-report-through-pa2`: 80/80 pass.
- `perl scripts/cppgm_file_audit.pl --stage pa2 --paths dev/src`: pass.
- `student.tests/pa2-literals-edge-cases.t`: explicitly run through `dev/posttoken`, exact diff pass.
- Unfinished implementation: none identified. Independent audit: review broader C++11 lexical corner cases and raw/string suffix boundaries; this review question does not waive required behavior.
- Handoff boundary: PA2 token stream/output is complete; subsequent preprocessing, parsing and code generation stages remain outside this stage. All intended changes are committed.
