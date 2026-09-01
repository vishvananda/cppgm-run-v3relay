# PA3 Plan — ctrlexpr (controlling-expression evaluation) [COMPLETE]

Final state: 104/104 fixtures pass through `make test-report-through-pa3`
(53 pa1 + 28 pa2 local/course + 11 pa3 local including three boundary fixtures
added at CP3 + 12 pa3 course); file audit passes. Architecture review,
findings, performance evidence, and conformance validation are consolidated in
`pa3/audit.md`.

## Stage Design (as built)

Owning boundary: ctrlexpr owns splitting the phase-1–3 pp-token stream into
logical lines, per-line reuse of PA2 post-tokenization, and grammar parse +
64-bit signed/unsigned evaluation of each line. PA2 code is reused untouched;
no shared interface changes.

- `dev/src/ctrlexpr_eval.h/.cpp` —
  - `CtrlExprToken`: Literal{is_unsigned, uint64_t value} | Identifier{text}
    | Op{ETokenType} | Bad.
  - `CtrlExprTokenCollector : IPostTokenOutputStream` — maps PA2 emissions to
    tokens. `emit_simple`: keywords (which precede operators in `ETokenType`)
    → Identifier carrying source text; else Op. `emit_literal`: integral
    `EFundamentalType`s → Literal (little-endian read, sign-extended when the
    course signedness list says signed); float/double/long double → Bad.
    `emit_invalid`, arrays, and all user-defined literals → Bad.
  - `CtrlExprLineSplitter : IPPTokenStream` — buffers raw pp-token events
    (kind + spelling) per logical line; whitespace-sequences are discarded at
    this boundary (pa3/README.md Features). On `emit_new_line`/`emit_eof`,
    a non-empty line replays into a fresh `PostTokenStream` + collector, then
    the token vector is evaluated and one line printed. Per-line replay keeps
    PA2's cross-line string concatenation out of PA3.
  - Evaluator: `EvaluateControllingExpression` first rejects any line holding
    a Bad token (the invalid-token check precedes parsing, so unparseable
    lines never recurse), then runs a precedence-climbing
    parse-and-evaluate returning `EvalResult{is_unsigned, value, error}`.
    Wrapping uint64 arithmetic with flagged errors (never UB); `&&`/`||`
    suppress unevaluated-operand errors; `?:` discards its condition's
    evaluation error (the carried value selects the arm) and takes the OR of
    both arm types. A failed div/mod/shift carries its left operand's value.
    `!` preserves operand signedness (reference-pinned; see audit).
  - `defined` predicate injected as a callback; the mock stays in the tool so
    preproc (pa6+) can reuse the evaluator with a real macro table.
- `dev/ctrlexpr.cpp` — thin adapter mirroring `dev/posttoken.cpp`:
  `PA3Mock_IsDefinedIdentifier`, batch-stdin guard, catch envelope (lex
  exception → EXIT_FAILURE); stdin → `PPTokenize` → splitter → final `eof`.
- `dev/frontend_source_sets.mk` — ctrlexpr links
  `ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode`.

Output: unsigned → decimal + `u`; signed → int64_t decimal; invalid line →
`error`; empty/whitespace-only line → no output; final line `eof`.

## Checkpoint Ledger

- CP1 (DONE) — line pipeline + primary/unary evaluator. Proof: 9/20 pa3 at
  the time, 81/81 through-pa2, file audit pass, 100k-line probe < 5s.
- CP2 (DONE) — binary precedence climb + `?:` + arithmetic/error semantics.
  Proof: 20/20 pa3, 81/81 through-pa2, file audit pass, probe < 5s.
- CP3 (DONE) — final architecture audit + cleanup. Proof: 104/104 with three
  new boundary fixtures (`255-cond-error-discard`, `265-lnot-unsigned-type`,
  `310-invalid-token-prescan`); 24,000-line differential fuzz clean; linear
  100k/200k scaling; details in `pa3/audit.md`.
