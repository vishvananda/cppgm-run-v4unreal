# PA7 — semantic dump handoff

## Stage and design boundary

- **Stage base commit:** `f4b2cdd351002ab973bad62dc4b16046b4937458`
- **Last reviewed commit:** `f4b2cdd351002ab973bad62dc4b16046b4937458`
- PA7 consumes the PA5 syntax graph and PA6 canonical `SemanticModel`, annotates expressions/statements with source-language types and value categories, resolves supported calls/conversions, and emits the deterministic PA7 view. There is no IR serialization/reparse, reference/host compiler delegation, fixture dispatch, or executable generation. `--emit-ast` and `--emit-types` remain separate existing views. Later class/template/lowering owners remain canonical type/scope/entity IDs, not rendered strings.
- The PA7 contract also has class/anonymous-union and a focused function-template fixture beyond the README's introductory subset. The implementation handles the checked surface; general template instantiation and later full class semantics remain later-stage work.

## Behavior groups, ownership, and validation

| Owner / group | Data flow and complexity | Validation / state |
|---|---|---|
| `CallSemantics` — expressions, conversions, indirect/direct calls, overload selection | Structured AST + indexed PA6 lookup/binding/type IDs → typed expression results and selected overloads → output view. Expression work is proportional to visited nodes and candidates; candidate conversion work is proportional to arguments, with pairwise dominance comparisons over the viable overload set. Template candidates and anonymous-union injected members are indexed once by interned name per TU. | Complete `make test-pa7`: 186/186, including overload ranking, pointer/reference qualification, arrays/functions, nullptr, enums, casts, template target selection. Complete. |
| `CallSemantics` — declarations, initializers, scopes, control flow | Function/block scope identities from the semantic model + source-ordered AST traversal → declaration/condition/return/case checks and deterministic statement dump. Work is linear in visited statements/expressions plus required lookup/candidate work. | Required success dumps and rejection statuses; loop/switch/condition-declaration, constexpr and anonymous-union fixtures. Complete. |
| `SemanticTypes` / parser — PA7 facts needed by the view | Retain the single parsed source graph through PA7 analysis; expose syntax type formation and indexed same-name bindings; add member-pointer/function-cv and parameter-adjustment facts, array-bound deduction, condition bindings, and qualified special-member ownership. | PA6/PA5 through report pass (498/498); PA7 output/status comparisons. Complete. |
| Driver / `CallSemantics` — TU output and failure handling | One independently preprocessed/parser TU per source argument → semantic model → exact dump wrappers. Analysis errors return `EXIT_FAILURE`; successful output is deterministic. | `make test-pa7`, multi-stage through report, file audit. Complete. |

## Performance evidence

PA7 emits semantic text, not an executable; generated-program runtime/text size is **not applicable**. A scope-lookup index was added to avoid repeated unrelated-scope and same-scope binding scans. Its evidence follows the fixed A/B protocol rather than treating a smaller graph as proof:

- Fixed `student.tests/pa7-semantic-perf.cpp` (404,882 bytes, SHA-256 `bcda1e0c7bf40b9d795dc3eea1940bf120d92ab50549f5cd7f9506ba8497ea62`): 5,000 function definitions plus 5,000 calls. Frozen A restored linear scope-location and candidate-binding scans; frozen B uses TU-built scope-location/name indexes. Flags were `g++ -std=gnu++11 -Wall -O3`, CPU pinned to 0. Binary hashes: A `13cdfaad024cb8fefaaf964de8281281f8d4606a051e84758681a6d94b6ecc87` (1,234,248 bytes), B `892e5afc1a4c24234f63230d186a3c2ea46d2cda1c6bfcd8f987c566e70b3665` (1,238,344 bytes). All A/B dumps were byte-identical: 2,615,146 bytes, SHA-256 `5040c3cecda39c09e183b6b361c2b568e7e8adf802316678daef6c25c4e1faf6`.
- Three paired ABBA blocks and three A/A plus three B/B calibration pairs are preserved in `pa7-performance-abba.csv`. A/A wall times varied substantially (17.17–26.31 s in calibration pairs, one 9.04 s pair delta); every ABBA B run was 0.38–0.39 s versus paired A medians 18.42–23.77 s, a roughly 97.9–98.4% repeatable reduction well outside that A/A spread. Peak RSS was 31,668–32,004 KiB (A) and 32,764–33,024 KiB (B); B adds roughly 3.5% on this stress workload. `size` reports compiler `.text` 1,099,219 B (A) → 1,101,175 B (B), +1,956 B / 0.18%. The compiler-work benefit on this affected workload is substantial relative to index construction and memory/text growth. No executable is emitted, so no runtime/code-size claim is made.
- `pa7-performance-measurements.csv` retains small course-input observations (below timer resolution) and five pinned CPU-0 runs of the final B stress workload (0.39–0.41 s, RSS 32,664–32,880 KiB; output hash identical). Earlier unpinned measurements with their own binary hashes are kept as historical observations, not pooled into the A/B result.

No PA7 numeric latency/RSS acceptance limit or O-level optimization budget is mandated. No unrelated performance gate was added; whole-pipeline acceptance is the required suite and the measured index work/growth is disclosed above.
## Handoff and review ledger

- Turn start: 0/186 PA7 tests passed; all failed with `EXIT_NOT_IMPLEMENTED`. Earlier stages and file audit were reported passing; the stage-progress baseline was 186 failures. No fixtures/references/comparison rules were changed.
- Final required status: PA7 186/186; earlier PA stages 498/498; `perl scripts/cppgm_file_audit.pl --stage pa7 --paths dev/src` checks 34 files. No coverage reduction. Personal stress input was explicitly run; it is not discovered by course criteria.
- **Unfinished PA7 implementation:** none against the checked PA7 surface. Full general template semantics, class/constructor semantics, and native-code behavior remain outside PA7's assignment boundary and belong to later PAs.
- **Independent audit questions (not waived):** trace one declaration and a demanded template through semantic identity and ownership; verify source buffers/AST lifetimes and ensure retained syntax is only the PA7 source-facing adapter, not a cross-phase transport; review the overload-candidate/qualification ranking implementation against the C++11 rules beyond supplied cases. The compiler-side lookup-index benefit is measured above; no generated-program optimization/runtime claim is made. The stage's required evidence does not certify those whole-compiler audits.
