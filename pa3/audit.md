# PA3 Final Architecture Audit — ctrlexpr

## Scope and method

Traced representative facts through their full ownership paths: source line →
pp-token events (`CtrlExprLineSplitter`) → per-line PA2 replay
(`PostTokenStream` → `CtrlExprTokenCollector`) → typed `CtrlExprToken` →
parse/evaluate (`EvaluateControllingExpression`) → printed result. Measured
scaling on 100k/200k-line and 100k/200k-token single-line inputs, probed the
recursion envelope against the reference, and ran differential conformance:
fixtures plus a 60-seed × 400-line randomized grammar fuzzer (24,000 lines)
against `ctrlexpr-ref`, comparing stdout bytes and exit status.

## Ownership trace (clean)

- **Line splitting**: `CtrlExprLineSplitter : IPPTokenStream` is the single
  owner of logical-line boundaries. It buffers raw pp-token events (kind +
  spelling) and discards whitespace-sequences at this boundary, exactly as
  pa3/README.md Features specifies; `PostTokenStream::emit_whitespace_sequence`
  is a no-op, so nothing is lost relative to whole-file classification. The
  event buffer is cleared per line with retained capacity — no cross-line
  growth. Per-line replay is what keeps PA2's cross-line string concatenation
  correctly out of PA3 (two adjacent string-literal lines stay two errors).
  The pp-token spelling here is not a textual downgrade: pp-tokens are still
  text at this stage; classification happens exactly once, during replay.
- **Token conversion**: `CtrlExprTokenCollector : IPostTokenOutputStream` is
  the one mapping from PA2 emissions to the four-kind `CtrlExprToken`
  vocabulary. `IsIntegralType` is the sole owner of the course
  integral/signedness split (no equivalent authority exists in PA2 to reuse —
  PA2's signedness lives implicitly in its candidate lists). `ReadScalar`
  decodes the emitted ABI bytes back to a value at the fixed
  `IPostTokenOutputStream` boundary; the interface carries `(data, nbytes)`,
  so this is the boundary contract, not a downgrade.
- **Evaluation**: `EvaluateControllingExpression` owns the README's ordered
  semantics: invalid-token check first (a line holding a Bad token errors
  without parsing), then a precedence-climbing parse-and-evaluate with no AST.
  Binary precedence lives in one table-shaped switch mirroring the grammar
  ladder; associativity is the climb loop. All arithmetic wraps in `uint64_t`
  with flagged errors — no UB path exists. `defined` resolution is injected
  (`CtrlExprIsDefined`), keeping the PA3 mock in the tool so the evaluator can
  take a real macro table at pa6+.
- **Presentation**: result formatting (`PrintResult`) writes `"\n"`-terminated
  lines to the injected ostream; no per-line flush (the pa1/pa2 flush lesson
  holds). The tool adapter owns iostreams and the exit-status envelope.

## Error and type model (reference-pinned)

The evaluator's semantics were differentially validated against
`ctrlexpr-ref`, which pins three rules that a strict ISO reading would not
give. All three are deliberate, fixture-pinned conformance choices — later
preprocessor stages (`#if`) gate on ref-generated fixtures that embed the
same evaluator semantics, and the differences are value-observable there
(e.g. `!0u - 1 > 0`).

1. **`!` preserves operand signedness**: `!0u` → `1u` (ISO: `!` yields
   signed int). Comparisons and `&&`/`||` yield signed 0/1 as usual.
2. **`?:` discards its condition's evaluation error**: the condition's
   carried value still selects the arm, and only the selected arm's error
   survives (`1/0 ? 1 : 2` → `1`, `1/0 ? 1/0 : 2` → `error`). The result
   type is the OR of both arm types regardless of selection.
3. **A failed div/mod/shift carries its left operand's value**: `2/0` flags
   an error but carries 2, observable exactly when a `?:` condition discards
   that error (`0%0 ? 5 : 6` → `6`, `2/0 ? 5 : 6` → `5`). Consequently
   value computation is not gated on operand error flags
   (`(2/0)/1 ? 5 : 6` → `5`).

`&&`/`||` propagate an *evaluated* operand's error (`(1/0) && 0` → error)
and suppress only short-circuited operands (`0 && (1/0)` → 0). Syntax errors
and invalid tokens always poison the line.

## Findings and changes (this cleanup)

1. **`?:` condition-error propagation diverged from the reference (material,
   fixed)**: evaluation errors in the condition poisoned the result and
   gated arm evaluation (`1/0 ? 1 : 2` → error vs ref `1`; found by fuzz
   round 1 in ~2% of generated lines). Rules 2–3 above are now implemented;
   fixture `255-cond-error-discard` (26 lines) pins the discard, the
   carried-value selection, arm-error propagation, and the `&&`/`||`
   contrast cases.
2. **`!` yielded signed for unsigned operands (material, fixed)**: `!0u`
   printed `1` vs ref `1u`. Fixed to preserve operand signedness; fixture
   `265-lnot-unsigned-type` pins it, including `!` through `?:` static arm
   typing and char16_t operands. The ISO conflict is documented at the fix
   site and above.
3. **Crash on invalid-token lines with deep nesting (material, fixed)**: the
   parser recursed before discovering a Bad token, so `"(" × 100000` followed
   by `"x"` segfaulted where the reference prints `error`. The
   invalid-token check now precedes parsing (matching the README's Features
   order), so unparseable lines never recurse; the explicit KIND_BAD branch
   in `ParsePrimary` became unreachable and was removed. Fixture
   `310-invalid-token-prescan` pins a 60k-deep bad-token line (beyond the
   ~35k recursion capacity, so a regression fails by crash, not by output).
4. **Whitespace events buffered then replayed as no-ops (fixed)**: the
   splitter stored a `RawEvent` per whitespace-sequence and scanned each line
   to detect emptiness. Whitespace is now discarded at emission (per the
   README) and emptiness is `line_.empty()` — one less event kind, no scan,
   byte-identical output.
5. **Minor**: stray member indentation, two `ParseDefined` string copies →
   const refs, and a comment stating the `token_type <= KW_WHILE`
   enum-order dependency in `emit_simple`.

## Performance evidence

Release build, output to /dev/null, medians of five interleaved runs:

- 100k lines of `((((5))))*3+4`: 0.91s / 6.5 MB RSS; 200k lines: 1.79s /
  9.3 MB — 1.97× for 2× input, well under the 10s harness timeout
  (unloaded-host best case 0.44s/0.89s).
- One 100k-term line `1+1+…`: 0.11s / 33 MB RSS; 200k terms: 0.22s / 63 MB —
  linear time; RSS is the per-line event+token buffers, proportional to the
  line itself (inherent: a line must be complete before evaluation).
- 60k-deep bad-token line: 0.03s (pre-scan, no recursion).
- Recursion envelope: valid nesting parses to ~35k depth (4 frames/level);
  ours and the reference both segfault at 50k and both succeed at 20k —
  identical envelope class, inherent to recursive descent. Every input the
  reference answers, we now answer.
- Per line: one `PostTokenStream` (O(1) state) and one token vector; the
  event buffer reuses capacity across lines. No quadratic path exists:
  precedence climbing is one pass, each subexpression evaluates exactly once.

## Conformance validation

- `make test-report-through-pa3`: 104/104 (53 pa1 + 28 pa2 + 11 pa3 local +
  12 pa3 course), including the three new boundary fixtures; file audit: 20
  files pass.
- Randomized differential: 60 seeds × 400 lines (24,000 lines) over integer/
  char literals of every base and suffix, all operators incl. alternative
  tokens, `defined` forms, keywords, unicode identifiers, injected bad
  tokens, comment/whitespace variants, and token soup — 0 mismatches in
  stdout bytes or exit status (before fixes: 27/30 seeds mismatched).
  Whole-file lex-error partial output also matches byte-for-byte.
- Directed probes: fixture-pinned semantics re-verified (`false?5/0u:-5`,
  `(1u<<63)%-1`, shift typing, signed/unsigned comparison, `defined`
  malformed operands) plus the new error-model edges enumerated above.
- Divergence envelope: rules 1–3 (Error and type model) conflict with a
  strict ISO reading and are carried deliberately as reference conformance;
  they are pinned by fixtures 255/265 so any future reference change is
  caught at regeneration time. No other reference quirks found in 24,000
  fuzzed lines.

## Checkpoint ledger

| id  | scope                                            | proof | status |
|-----|--------------------------------------------------|-------|--------|
| CP1 | line pipeline, primary/unary/defined evaluation  | 9/20 pa3; 81/81 through-pa2; file audit pass | DONE |
| CP2 | precedence climb, `?:`, arithmetic/error rules   | 20/20 pa3; 81/81 through-pa2; 100k probe < 5s | DONE |
| CP3 | final audit: error model, `!` typing, pre-scan, whitespace discard | 104/104 with 3 new fixtures; 24k-line fuzz clean; linear scaling; envelope = reference | DONE |
