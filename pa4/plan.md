# PA4 implementation plan and handoff ledger

## Stage entry and design alignment

- Stage base commit: `4c7867d2d7d0e26237fcab01eb8d4ea0af777242`; Last reviewed commit: `4c7867d2d7d0e26237fcab01eb8d4ea0af777242` (preserve both while implementing).
- Current ownership path is `PPTokenizer` borrowed callbacks -> typed preprocessing tokens/directive state -> macro rescanner -> posttoken phase-5/7 adapter. The assignment has no produced C++ executable; compiler latency/RSS and preproc executable `.text` apply, generated runtime/text do not. No optimizer/IR budgets exist in PA4.
- Current failure owner: `dev/preproc.cpp` is wholly NotImplemented, so all 105 failures share the missing preprocessor end-to-end behavior. Planned groups: (1) tokenization, macro definitions/argument collection/stringize/paste/rescan/recursion (O(produced tokens), macro state); (2) conditionals/directive ordering/error semantics; (3) includes, source locations/predefineds/line control/once/pragma; (4) integration/output and phase-7 validation. Data flows from one source buffer through PPTokenizer to one current file token cursor, with translation-unit state shared by includes and reset for each primary input. Validate incrementally with focused fixtures, full `make test-pa4`, explicit `student.tests/`, and through-PA4.

## Remaining groups, evidence and ledger

- Macro engine: owner `dev/src/preprocess`; raw/expanded parameter views and token-local unavailable-name paint; complexity target linear in input plus produced expansion tokens, with a defensive nesting limit only for malformed resource exhaustion. Required macro fixture groups are validation.
- Directives/inclusion: same preprocessor state owner; conditional stack, include recursion, file identity, presumed file/line and builtins. Work proportional to directive/token/include work; validate all directive fixtures and multiple primary translation units.
- Phase 5-7 integration: reuse PA2's typed posttoken callback consumer rather than serialize preprocessor tokens back into source; output PA2 record format. Validate invalid tokens, literal concatenation and exact checked output.
- Performance evidence: pending implementation. Freeze compiler binary/flags/inputs, measure ABBA latency and peak RSS, and executable `.text`; no native generated program is produced. Preserve measurements and compare against stage-base equivalent behavior where meaningful. No self-imposed hard thresholds.
- Handoff boundary: implementation and independent-review questions will be recorded separately after validation; no known defect or requirement is waived.
