# PA3 Plan — ctrlexpr (controlling-expression evaluation)

Target: 20/20 pa3 fixtures + clean `make test-report-through-pa3`; file audit
clean. Baseline: 0/20 — every failure is the stub `dev/ctrlexpr.cpp` throwing
`NotImplementedException` (EXIT_NOT_IMPLEMENTED vs expected EXIT_SUCCESS).

## Stage Design

Owning boundary: ctrlexpr owns splitting the phase-1–3 pp-token stream into
logical lines, per-line reuse of PA2 post-tokenization, and grammar parse +
64-bit signed/unsigned evaluation of each line. PA2 code is reused untouched;
no shared interface changes.

- `dev/src/ctrlexpr_eval.h/.cpp` (new) —
  - `CtrlExprToken`: Literal{is_unsigned, uint64_t value} | Identifier{text}
    | Op{ETokenType} | Bad.
  - `CtrlExprTokenCollector : IPostTokenOutputStream` — maps PA2 emissions to
    tokens. `emit_simple`: `type <= KW_WHILE` (keywords precede ops in
    `ETokenType`) → Identifier carrying source text; else Op. `emit_literal`:
    integral `EFundamentalType`s → Literal (sign-/zero-extend `nbytes` of data
    per course signedness lists in pa3/README.md); float/double/long
    double/void/nullptr → Bad. `emit_invalid`, `emit_literal_array`, all
    `emit_user_defined_*` → Bad. `emit_identifier` → Identifier. Ignore eof.
  - `CtrlExprLineSplitter : IPPTokenStream` — buffers raw pp-token events
    (kind + string) per logical line. On `emit_new_line`/`emit_eof`: if the
    line has only whitespace events, no output; else replay events into a
    fresh `PostTokenStream` + collector (then eof to flush), parse/evaluate
    the token vector, print one line. Per-line replay keeps PA2's cross-line
    string concatenation semantics out of PA3 correctly.
  - Evaluator: recursive-descent parse-and-evaluate, no AST. Each function
    returns `EvalResult{bool is_unsigned; uint64_t value; bool error;}`. All
    subexpressions are evaluated eagerly with wrapping uint64 arithmetic and
    *flagged* errors (never UB); `&&`/`||`/`?:` discard the error flag (not
    the type) of unevaluated operands. This yields short-circuit error
    suppression plus static type propagation in one linear pass.
  - `defined` predicate injected as a callback; the mock stays in the tool so
    preproc (pa6+) can reuse the evaluator with a real macro table.
- `dev/ctrlexpr.cpp` — thin adapter mirroring `dev/posttoken.cpp`: keep
  `PA3Mock_IsDefinedIdentifier`, batch-stdin guard, and the catch envelope
  (lex exception → EXIT_FAILURE); read stdin, `PPTokenize` into the splitter,
  print final `eof`, EXIT_SUCCESS.
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_ctrlexpr :=
  ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode`.

Output: unsigned → decimal + `u`; signed → value printed as int64_t (may be
negative). Trailing unconsumed tokens → `error`. Final line `eof`.

## Behavior facts (fixture-proven)

- Short-circuit suppresses evaluation errors of unevaluated operands
  (`5||(5%0)`→1, `0&&(5/0)`→0, `true?5:5/0`→5) but the result *type* of `?:`
  propagates statically even from the unevaluated arm
  (`false?5/0u:-5`→18446744073709551611u).
- Types: `* / % + - & ^ |` and `?:` arms use UAC (unsigned if either side
  unsigned); comparisons, `!`, `&&`, `||` → signed 0/1; unary `+ - ~`
  preserve signedness; shifts take the LEFT operand's type only
  (`1 << 60u` → signed 1152921504606846976).
- Evaluated-only errors: div/mod right operand 0; shift right operand
  negative or ≥ 64; signed `INTMAX_MIN / -1` and `INTMAX_MIN % -1`
  (`(1<<63)/-1`→error, but `(1u<<63)/-1`→0u, `(1u<<63)%-1`→9223372036854775808u).
  All other signed overflow wraps (`1<<63` legal; `521*521*-132`→-35830212).
- Identifiers, including every keyword (`auto`) → 0 signed; `true`→1,
  `false`→0. `defined X` / `defined ( X )` — operand is any
  identifier-or-keyword including `defined` itself; mock = first byte odd;
  malformed operand (`defined(A+B)`, unclosed paren, missing id) → error.
- Any non-integral token in the line → error: strings, floats, arrays, UDLs,
  PA2-invalid tokens (`u'\U00102222'`). Char literals keep PA2 types:
  `'a'`→97, `u'a'`/`U'a'`→97u, `L'a'`→97, `'\U00102222'`→FT_INT 1057314.
- Alternative tokens (`and`, `bitor`, `not`, `compl`, …) already map to OP_*
  in `StringToTokenTypeMap` — zero new work.
- Empty/whitespace-only (incl. comment-only) lines produce no output line.

## Failure Map

Single root cause today (stub). By owning checkpoint once the pipeline exists:

- CP1 — primary/unary/paren/defined (6): pa3/tests/100-primary, 110-paren,
  120-defined; course/pa3/100-unicode-character-literals,
  120-defined-keyword-operand, 120-defined-malformed-operands.
- CP2 — binary ladder + conditional + arithmetic/error semantics (14):
  pa3/tests/200-ops, 200-ops-alts, 250-eval-order, 260-cond-ret-type,
  300-triple; course/pa3/200-adjacent-subtraction, 200-chained-multiplicative,
  200-chained-shifts, 200-operator-precedence, 200-signed-unsigned-comparison,
  300-incomplete-expression-bad, 300-logical-line-error-isolation,
  300-unconsumed-expression-tokens-bad, 500-integer-overflow.

## Performance Risks

- Recursion depth = O(line tokens) on nested parens/unary chains; fixtures
  are tiny; acceptable. No memoization needed: each subexpression is
  evaluated exactly once → linear per line, linear overall.
- Per-line fresh `PostTokenStream` construction is O(1) state; replay is
  O(tokens). Event buffer is cleared per line — no cross-line growth.
- Probe: `perl -e 'print "((((5))))*3+4\n" x 100000' | timeout 5
  ./dev/ctrlexpr > /dev/null` must finish well under the 10s harness timeout.

## Checkpoint Ledger

- CP1 (ACTIVE) — line pipeline + primary/unary evaluator. Proof: the 6 CP1
  fixtures above flip to pass (`make test-pa3` 6/20); pa1/pa2 stay clean.
- CP2 — binary precedence climb + `?:` + full arithmetic/error semantics.
  Proof: `make test-pa3` 20/20; `make test-report-through-pa3` clean.
- CP3 — audit + cleanup: file audit clean, stub remnants pruned, plan marked
  complete.

## Active Checkpoint: CP1 — pipeline + primary/unary

Build the whole data path end-to-end with the grammar restricted to
`controlling-expression := unary-expression` where
`unary := (+|-|!|~)* primary` and `primary := integral-literal |
( controlling-expression ) | defined id | defined ( id ) | identifier`.
Lines using binary/conditional operators fail parse → `error` (no crash, no
regression); CP2 inserts the binary climb + conditional between the top rule
and unary at a stable seam.

### Implementation Packet

- Files/symbols: create `dev/src/ctrlexpr_eval.h` + `.cpp` with
  `CtrlExprToken`, `CtrlExprTokenCollector : IPostTokenOutputStream`,
  `CtrlExprLineSplitter : IPPTokenStream` (ctor takes `std::ostream&` and an
  is-defined callback), and the evaluator entry
  (`EvaluateControllingExpression(tokens, is_defined) → EvalResult`).
  Rewrite the TODO branch of `main` in `dev/ctrlexpr.cpp` following
  `dev/posttoken.cpp:160-187` (keep batch-stdin guard + catch envelope; keep
  `PA3Mock_IsDefinedIdentifier`, pass it as the callback). Update
  `FRONTEND_OBJ_BASENAMES_ctrlexpr` in `dev/frontend_source_sets.mk` to
  `ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode`.
- Interfaces consumed: `PPTokenize(const std::string&, IPPTokenStream&)`
  (`dev/src/pptoken_lexer.h`); the 12 `IPPTokenStream` emit events
  (`dev/src/IPPTokenStream.h`) — buffer all except new_line/eof, blank-line =
  whitespace-only; `PostTokenStream(IPostTokenOutputStream&)`
  (`dev/src/posttoken_stream.h`); `ETokenType`/`EFundamentalType`
  (`dev/src/posttoken_types.h`) — keywords are enum values ≤ `KW_WHILE`,
  integral types per the signed/unsigned lists in pa3/README.md (signed:
  bool, wchar_t, char, signed char, short, int, long, long long; unsigned:
  unsigned char/short/int/long/long long, char16_t, char32_t).
- Fixture groups: must flip — pa3/tests/{100-primary,110-paren,120-defined}.t,
  cppgm.tests/course/pa3/{100-unicode-character-literals,
  120-defined-keyword-operand,120-defined-malformed-operands}.t. The other 14
  keep failing only on stdout diff (now EXIT_SUCCESS, `error` where CP2 will
  put values).
- Required spec facts: identifier/keyword → 0, `true`→1, `false`→0; unary
  `+ - ~` preserve operand signedness, `!` → signed 0/1; `defined` → signed
  0/1 via callback (first byte odd), operand may be any identifier incl.
  keywords/`defined`, whitespace inside parens fine, malformed → error;
  non-integral or PA2-invalid token anywhere in the line → error; unconsumed
  trailing tokens → error; blank line → no output; final `eof` line; signed
  prints as int64_t, unsigned appends `u`; literal bytes widen to 64-bit by
  sign-extension iff the PA2 type is signed.
- Commands: focused build `make -C dev ctrlexpr`; manual probe
  `printf "2\n(0x2)\n-'a'\ndefined foo\ntrue\n3.2\n" | ./dev/ctrlexpr`
  (expect `2 2 -97 0 1 error eof`); broad `make test-pa3` then
  `make test-report-through-pa3`; audit
  `perl scripts/cppgm_file_audit.pl --stage pa3 --paths dev/src`.
- Performance probe: the 100k-line paren pipe under Performance Risks (CP1
  variant: `"((((5))))\n"`), plus eyeball `make test-pa3` wall time.
- Known uncertainties: (a) unsuffixed decimal > INTMAX_MAX — trust PA2
  classification (invalid → error), no special-casing; (b) alternative
  tokens as `defined` operands (`defined(and)`) arrive as Op not Identifier →
  treated as error; no fixture exercises it, leave until one does; (c) exact
  stdout on phase-1–3 lex failure doesn't matter — failing tests compare
  exit status only.
