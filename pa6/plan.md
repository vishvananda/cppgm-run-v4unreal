# PA6 implementation handoff

## Stage markers and alignment
- **Stage base commit:** `782b9b53712785fbd9ed1111f77c7b7e0b9da4b2` (turn-start HEAD).
- **Last reviewed commit:** `782b9b53712785fbd9ed1111f77c7b7e0b9da4b2` (PA5 parser/AST audit); preserve both markers.
- Direct PA6 consumption must use the PA5 structured AST and translation-unit-owned source; no AST dump reparse. Semantic state is TU-local and identity-based, with deterministic output as an adapter. Keep AST mode unchanged and leave PA7 expressions/lowering to later stages.
- Turn-start required test baseline: 0/105 PA6 tests, all 105 reporting `EXIT_NOT_IMPLEMENTED`; earlier stages pass. Stage progress log did not provide a numeric baseline.

## Behavior groups, ownership and handoff ledger
| Group / owner | Flow and complexity | Validation / state |
|---|---|---|
| Scope/declaration graph, lookup, aliases/using and namespace/class/enum scopes / PA6 semantic analyzer | structured `SyntaxNode` -> TU-local scopes/entities/bindings/type IDs -> deterministic dump; source-order declaration/lookup, indexed name tables, bounded tree walk plus relevant lookup edges | TODO |
| Declarator type construction, compatibility and constants / same analyzer | specifiers + recursive declarators -> canonical recursive types, signatures and small integral evaluator; linear AST traversal and bounded expression walk | TODO |
| Driver dispatch and output / `cppgm++` | `--emit-types` selects analyzer; preserve `--emit-ast`; one independent semantic graph per input TU | TODO |

## Performance evidence and remaining groups
- PA6 emits a semantic text dump, not an executable; generated-program runtime/text size are inapplicable. Measure compiler build latency/peak RSS and representative dump latency/peak RSS; no assignment numeric latency/RSS/text gate exists. Do not impose unsupported thresholds or claim optimization without frozen A/B evidence.
- Remaining groups after implementation: TODO. Independent architecture audit questions must be recorded separately from unfinished behavior.
