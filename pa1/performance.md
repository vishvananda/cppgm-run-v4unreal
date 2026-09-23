# PA1 performance evidence

The stage produces preprocessing-token output, not generated programs, so generated-program runtime/text size are not applicable. The only measured change for performance was turning off synchronized C/C++ standard streams in the CLI; no tokenization semantics or phases changed.

## Frozen A/B setup

- Host-built C++11 tool flags: `g++ -std=gnu++11 -Wall -O3` (same for both binaries).
- A (before stream setting): SHA-256 `d5f0be6a9aa363ee0b513a498a521c380db62d8e54793a66951e607b45502ddf`.
- B (after stream setting): SHA-256 `0f453b969bfd8a47dfd7814d3060b2fedbbd556ff18715b5fe0d97fb44cbafa5`.
- Input: generated 24 MiB source, 786,432 repetitions of `alpha_123 + 42.5e-1; // comment\n`; SHA-256 `81a5933fe6c6bb7bd36075fcae88af617d5423d0dff6d4a9a78268996bd56f2b`.
- Output was sent to `/dev/null` for timing. `/usr/bin/time` measured wall/user/system seconds and maximum RSS (KiB). Runs were serial in the listed A/A then ABBA order; the input and binaries were frozen.

## Observations

A/A noise calibration:

| Run | Wall | User | System | Max RSS |
|---|---:|---:|---:|---:|
| A0 | 5.87 s | 5.78 s | 0.09 s | 34,268 KiB |
| A1 | 5.99 s | 5.91 s | 0.08 s | 34,320 KiB |

Three ABBA blocks. Each row lists one invocation in actual A/B/B/A order:

| Block / order | Binary | Wall | User | System | Max RSS |
|---|---|---:|---:|---:|---:|
| 1/A1 | A | 5.77 s | 5.68 s | 0.08 s | 34,352 KiB |
| 1/B1 | B | 4.90 s | 4.83 s | 0.07 s | 34,376 KiB |
| 1/B2 | B | 4.93 s | 4.85 s | 0.08 s | 34,376 KiB |
| 1/A2 | A | 5.91 s | 5.83 s | 0.08 s | 34,412 KiB |
| 2/A1 | A | 5.73 s | 5.64 s | 0.08 s | 34,352 KiB |
| 2/B1 | B | 4.69 s | 4.62 s | 0.07 s | 34,460 KiB |
| 2/B2 | B | 5.03 s | 4.96 s | 0.06 s | 34,376 KiB |
| 2/A2 | A | 5.66 s | 5.56 s | 0.09 s | 34,320 KiB |
| 3/A1 | A | 5.62 s | 5.54 s | 0.08 s | 34,324 KiB |
| 3/B1 | B | 4.66 s | 4.59 s | 0.06 s | 34,344 KiB |
| 3/B2 | B | 4.77 s | 4.70 s | 0.06 s | 34,380 KiB |
| 3/A2 | A | 5.58 s | 5.49 s | 0.09 s | 34,088 KiB |

Paired block means were: block 1 A=5.840 s/B=4.915 s (15.8% reduction); block 2 A=5.695 s/B=4.860 s (14.7%); block 3 A=5.600 s/B=4.715 s (15.8%). Across blocks the paired reduction ranges 14.7–15.8% (median 15.8%). The A/A elapsed range is 5.87–5.99 s (2.0%), below the measured paired effect.

A max-RSS samples have median 34,338 KiB and range 34,088–34,412 KiB (324 KiB spread). B samples have median 34,378 KiB and range 34,344–34,460 KiB (116 KiB spread). The 40 KiB median difference is within the combined 372 KiB observed range.

`size` reported A/B text sections of 116,651/116,835 bytes (+184 bytes, +0.16%); data sections 1,936/1,944 bytes; BSS 1,352/1,352 bytes. The checkpoint binaries were built separately from the standalone binaries in the final-audit rerun, so their absolute section sizes should not be compared. The former plan's 1% tool-text and 1% RSS thresholds were self-imposed and are not a PA1 handout or spec requirement; they are preserved here only as historical context, not exit gates. A and B produced identical output on the frozen 1,046,978-byte equivalence input (SHA-256 input `d92949c09e7150cbe8cf1c216024f6785a49fbc50d660da14d6f24d922923e4e`); output SHA-256 for each: `88f0570ee3faf89a56ca8ade8194518af55c71aba92c704310b993985615e3a2`). B also passed all 54 PA1 fixtures; `make test-pa1` completed in 0.34 s on that checkout.

No generated-code optimization or generated executable size/runtime claim is made.

## Independent final-audit rerun

This is a fresh, frozen A/B measurement after the independent architecture audit. It supersedes the prior section for the final performance judgment while preserving the prior checkpoint numbers above.

- Toolchain/flags: `g++ -std=gnu++11 -Wall -O3 -Idev/src` (same command and current tokenizer translation unit for both binaries).
- A (`pptoken-A`) SHA-256: `783c036985c8bed7146d5bdbaa7da9c9f0849aafabc09ca631996730b4d33129`; it uses the pre-stream-tweak `dev/pptoken.cpp` from implementation commit `6a3a8518c`.
- B (`pptoken-B`) SHA-256: `1e6d0b687d009f42922b5678aab8e24e54fd3ca4050909751256bbb3c53a2e26`; it uses the audited current CLI. `diff` shows the only A/B source differences are `sync_with_stdio(false)` and `cin.tie(NULL)`.
- Both are fixed Linux x86-64 binaries, rebuilt reproducibly after source/test fix commit `6961c35ff` against the exact same audited `PPTokenizer.cpp`; input was 786,432 repetitions of `alpha_123 + 42.5e-1; // comment\n`, 25,165,824 bytes (24 MiB), SHA-256 `81a5933fe6c6bb7bd36075fcae88af617d5423d0dff6d4a9a78268996bd56f2b`. Timing sends output to `/dev/null`; `/usr/bin/time` collects wall, user, system seconds and maximum RSS KiB.
- A/A calibration was serial before the first ABBA block (A0/A1), then expanded with four additional A runs after outliers appeared. All observations are reported; the expanded calibration range is 5.68–5.93 s (median 5.715 s, 4.4% of the median). The observed environment was noisy in some ABBA samples; no outlier is discarded.

A/A calibration:

| Run | Wall | User | System | Max RSS KiB |
|---|---:|---:|---:|---:|
| A0 | 5.68 | 5.62 | 0.06 | 34,420 |
| A1 | 5.74 | 5.66 | 0.07 | 34,368 |
| A2 | 5.68 | 5.60 | 0.07 | 34,444 |
| A3 | 5.93 | 5.84 | 0.08 | 34,448 |
| A4 | 5.69 | 5.59 | 0.08 | 34,456 |
| A5 | 5.79 | 5.72 | 0.07 | 34,304 |

Seven serial ABBA blocks (all individual observations retained):

| Block / order | Binary | Wall | User | System | Max RSS KiB |
|---|---|---:|---:|---:|---:|
| 1/A1 | A | 5.68 | 5.59 | 0.08 | 34,456 |
| 1/B1 | B | 4.74 | 4.67 | 0.07 | 34,476 |
| 1/B2 | B | 6.04 | 5.97 | 0.07 | 34,476 |
| 1/A2 | A | 5.73 | 5.63 | 0.09 | 34,452 |
| 2/A1 | A | 6.50 | 6.38 | 0.11 | 34,444 |
| 2/B1 | B | 4.74 | 4.67 | 0.06 | 34,480 |
| 2/B2 | B | 4.76 | 4.68 | 0.07 | 34,476 |
| 2/A2 | A | 5.68 | 5.59 | 0.08 | 34,452 |
| 3/A1 | A | 5.65 | 5.56 | 0.08 | 34,448 |
| 3/B1 | B | 4.85 | 4.79 | 0.05 | 34,468 |
| 3/B2 | B | 5.84 | 5.71 | 0.07 | 34,444 |
| 3/A2 | A | 9.23 | 8.61 | 0.14 | 34,020 |
| 4/A1 | A | 5.67 | 5.58 | 0.08 | 34,176 |
| 4/B1 | B | 4.75 | 4.67 | 0.07 | 34,476 |
| 4/B2 | B | 4.76 | 4.69 | 0.06 | 34,448 |
| 4/A2 | A | 5.68 | 5.61 | 0.07 | 34,424 |
| 5/A1 | A | 5.81 | 5.73 | 0.07 | 34,420 |
| 5/B1 | B | 4.78 | 4.72 | 0.06 | 34,444 |
| 5/B2 | B | 4.75 | 4.68 | 0.06 | 34,476 |
| 5/A2 | A | 5.66 | 5.57 | 0.08 | 33,912 |
| 6/A1 | A | 6.14 | 6.06 | 0.07 | 34,452 |
| 6/B1 | B | 4.73 | 4.66 | 0.06 | 34,444 |
| 6/B2 | B | 4.79 | 4.72 | 0.06 | 34,480 |
| 6/A2 | A | 5.68 | 5.61 | 0.08 | 34,396 |
| 7/A1 | A | 5.67 | 5.59 | 0.07 | 34,400 |
| 7/B1 | B | 4.77 | 4.70 | 0.06 | 34,480 |
| 7/B2 | B | 4.75 | 4.69 | 0.06 | 34,476 |
| 7/A2 | A | 5.68 | 5.59 | 0.09 | 34,420 |

Paired block wall means and reductions (no exclusions):

| Block | A mean | B mean | B faster |
|---|---:|---:|---:|
| 1 | 5.705 s | 5.390 s | 5.5% |
| 2 | 6.090 s | 4.750 s | 22.0% |
| 3 | 7.440 s | 5.345 s | 28.2% |
| 4 | 5.675 s | 4.755 s | 16.2% |
| 5 | 5.735 s | 4.765 s | 16.9% |
| 6 | 5.910 s | 4.760 s | 19.5% |
| 7 | 5.675 s | 4.760 s | 16.1% |

B mean wall time is lower in all seven blocks; paired improvement median is 16.9% (range 5.5–28.2%). Across all ABBA samples A's median wall is 5.68 s (range 5.65–9.23); B's median is 4.76 s (range 4.73–6.04). The isolated 9.23 s A and 6.04 s B observations are retained and make the environment-noise caveat material. The median paired effect exceeds the 4.4% expanded A/A calibration spread, and six blocks are 16.1–28.2%; the smallest block (5.5%) includes the 6.04 s B outlier. This supports the direction and typical size of the stream-I/O improvement, not a guaranteed per-run percentage.

Across ABBA runs, maximum RSS median is 34,422 KiB for A (20 samples, 33,912–34,456) and 34,476 KiB for B (14 samples, 34,444–34,480), a 54 KiB / 0.16% median increase with overlapping ranges. This is not a meaningful peak-memory regression at the observed resolution. `size` reports A/B text 57,323/57,475 bytes (+152, +0.27%), data 1,640/1,648, BSS 936/936. These compiler-binary section sizes are recorded, not confused with generated-code text. The obsolete 1% thresholds remain non-binding: the spec defines no 1% PA1 text/RSS limit, and this CLI change neither alters the phase algorithm nor creates generated code.

A and B emitted identical full output on the frozen benchmark input; both output SHA-256 values are `21868a9412bee47b5f1a5e959a991d10e94232c4cb8ee9863288d94de641ffcd`. The course suite checks token output separately. PA1 generates no executable program, so generated-program runtime and generated text size are not applicable; no claim is made for them. The benchmark is relevant to PA1's fixed tokenizer CLI (identifiers, pp-numbers, operators, comments, and newlines), not to templates, later semantic phases, or object code.
