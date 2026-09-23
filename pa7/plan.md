# PA7 implementation plan

## Design/spec alignment

Stage base commit: `f4b2cdd351002ab973bad62dc4b16046b4937458`
Last reviewed commit: `f4b2cdd351002ab973bad62dc4b16046b4937458`

PA7 consumes the existing PA5 AST and PA6 canonical `SemanticModel`, then performs
procedural expression/call/statement analysis and deterministic output. Keep the
semantic result identity-based and the dump a view; do not add text reparsing,
fixture dispatch, or host/reference compiler delegation. Preserve existing
`--emit-ast`/`--emit-types` paths and leave later class/template/lowering work
possible. Stage acceptance covers correctness and tests; no optimization exists
in this stage, so performance evidence will measure compile latency/RSS and the
semantic execution overhead/output size separately.

## Behavior groups and ledger

| Group / owner | Data flow and complexity | Validation | State |
|---|---|---|---|
| Expression typing, conversions, overloads / PA7 semantic analyzer | AST expression -> typed result with canonical type/category/declaration; lexical lookup and candidate-local standard conversion ranking | focused spec/general tests, then all `make test-pa7` | not started |
| Statement scopes, control flow, initializers / PA7 statement analyzer | PA5 statement AST + PA6 scopes -> sequential block/selection/loop/switch checking and structured output; work proportional to AST plus visited lookup candidates | scope/control tests and whole stage suite | not started |
| Deterministic PA7 output + driver / PA7 printer/dispatcher | typed semantic result -> exact reference-compatible dump, linear in emitted nodes | success-output and failure-status tests; preserve PA5/6 | not started |

## Performance evidence

No PA7 measurements yet. Record compiler build/test latency and peak RSS, and
representative semantic-analysis latency/output bytes after implementation;
correctness work is not justified by optimization claims. Stage-scoped limits
are the required assignment tests and no self-imposed performance gate.

## Handoff ledger

Turn-start: 0/186 tests pass; all 186 return `EXIT_NOT_IMPLEMENTED` because the
PA7 driver is a stub. Earlier stages pass; file audit passes. Baseline stage
failure count for progress is 186. Independent architecture review questions
remain distinct from any unfinished PA7 implementation.
