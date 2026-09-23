# PA5 implementation handoff

Stage base commit: `4c77c279c09fb196ee905efad07594c8524b28a4`
Last reviewed commit: `4c77c279c09fb196ee905efad07594c8524b28a4`

## Design/spec alignment

`cppgm++ --emit-ast` now uses PA4 preprocessing and the PA2 post-token pipeline through a typed token collector (no textual token roundtrip). It parses one translation unit at a time into an exported structured `SyntaxNode` tree, then renders the deterministic PA5 dump. The parser covers declarations/declarators, limited PA5 name categories, templates/ambiguities, statements, expressions and required rejection cases; the driver preserves input order and reports preprocessing/parse failures.

## Behavior groups and handoff ledger

- **Frontend/driver (complete):** source files -> post-token records -> per-TU parser -> AST output. Storage/work are linear in the active TU's records and tree; token views avoid a second owning token vector, and TU records are released before the next input. Validated by PA5 188/188, personal single-/multi-TU smoke, and prior-PAs report.
- **Syntax parser/name categories (complete for PA5 contract):** recursive grammar parsing and scoped type/value/template facts, with delimiter-aware template spelling and precedence/associativity. Expected parsing is linear in consumed tokens plus AST output, with bounded lookahead in ambiguous regions. All exact success dumps and required failures pass.
- **AST rendering (complete):** deterministic rendering is an adapter over structured child nodes; exact checked-in outputs pass. No executable output is produced in PA5, so runtime/text-size acceptance is not applicable.
- **Unfinished implementation for later pipeline:** source-file/offset locations are not yet carried into every AST node, and syntax names are not yet canonical interned identities. The PA5 AST API is structured and reusable, but those PA6+ identity/location facts remain required follow-on work; this ledger does not waive them.
- **Independent audit questions:** audit future consumers for direct use of the structured tree (no dump reparsing), precise source-location propagation, and scope-category shadowing as semantic lookup replaces PA5 lexical fallback. No PA5 coverage or comparison rules were reduced.

## Performance evidence

Frozen release binaries A/B are byte-identical (`sha256 9e604dc010751a22ab2768813472657a899ab3c980d8908aaea0a97193321440`); flags and input were fixed. Input `/tmp/pa5-perf.cpp` is 81,586 bytes (1,800 generated distinct function definitions); AST is 1,041,995 bytes with one identical SHA-256 across all runs. Six A/A pairs calibrated noise; six wall-time ABBA blocks were recorded in `pa5-performance-measurements.csv`. A/A median 64.368 ms (63.430–66.028), max A/A peak RSS 9,332 KiB; the maximum across all A/A/ABBA runs was 9,336 KiB. ABBA group medians: A-before 64.308 ms, B-first 64.590 ms, B-second 65.102 ms, A-after 63.885 ms; paired spread overlaps A/A noise, and A/B are identical binaries, so no optimization benefit is claimed. `cppgm++` `.text` is 556,550 bytes. The AST dump is not executable, so no runtime or generated-program text-size result exists. PA5 adds no optimization pass or code-growth claim.
