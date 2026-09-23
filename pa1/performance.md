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

`size` reported A/B text sections of 116,651/116,835 bytes (+184 bytes, +0.16%); data sections 1,936/1,944 bytes; BSS 1,352/1,352 bytes. The explicit PA1 acceptance budget for this I/O-only tweak was at most 1% text growth and no reproducible >1% peak-RSS regression; both fit. The paired front-end latency reduction is well above the 2.0% A/A wall-time range. A and B produced identical output on the frozen 1,046,978-byte equivalence input (SHA-256 input `d92949c09e7150cbe8cf1c216024f6785a49fbc50d660da14d6f24d922923e4e`); output SHA-256 for each: `88f0570ee3faf89a56ca8ade8194518af55c71aba92c704310b993985615e3a2`). B also passes all 54 PA1 fixtures; `make test-pa1` completed in 0.34 s on this checkout.

No generated-code optimization or generated executable size/runtime claim is made.
