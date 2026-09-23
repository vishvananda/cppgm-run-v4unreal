# PA1 implementation plan

- Stage base commit: `e50e87639188c5da678ab00d4d927c5d57396c65`
- Last reviewed commit: `e50e87639188c5da678ab00d4d927c5d57396c65`
- Alignment: N3485 phases 1–3; immutable UTF-8 source, bounded streaming phase/token lookahead, direct `IPPTokenStream` events; keep PA2 conversion and later preprocessing separate. Work is O(source bytes + emitted bytes), with source + current token + bounded lookahead storage. No generated-code optimization gate applies to PA1; CLI throughput evidence is a final validation item.
- Completed groups: (1) owner `PPTokenizer` phase cursor, bytes→UTF-8/trigraph/line-splice/UCN/newline→tokens; linear scanning, raw delimiter capped at 16; validated by PA1 translation/literal tests. (2) owner `Lexer`, comments, maximal tokens and line-context header names; linear scanning and bounded fixed operator/Unicode-table lookups; validated by 54/54 fixtures and personal reducers.
- Reference correction: reduced `/*\nbody\n*/x\n` shows bundle `pptoken` drops both block-comment new-lines. N3485 §2.2 ¶3 requires retaining them. Corrected only `100-extra-comments.ref` and `900-real-world.ref`; inputs, status expectations, coverage unchanged. Bundle source SHA: `c2f713cd70d06170632bfde3e75dd6fe1aa44d98`. Reducer: `student.tests/pa1-block-comment-newlines.t`.
- Remaining implementation: none currently identified. Independent audit (not waived): verify whole-stage C++11 edge conformance and later PA2–PA4 reuse/integration.
- Performance evidence: tokenize and memory measurements pending; PA1 emits no generated executable, so generated-program runtime/text size are N/A. Record the output path and compiler-latency/RSS measurements before handoff.
- Handoff ledger: phase cursor/tokenizer and the corrected newline behavior are implemented; 54/54 PA1 fixtures, personal reducers, and file audit pass. Performance evidence and final performance check remain for the next increment; no known unfinished PA1 behavior.
