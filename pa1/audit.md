# PA1 Final Architecture Audit — pptoken

## Scope and method

Traced representative facts through their full ownership paths: raw byte →
decoded code point (`SourceDecoder`) → token spelling → `IPPTokenStream`
emission → tool presentation (`DebugPPTokenStream`). Measured scaling on 1–8 MB
representative and adversarial inputs, counted syscalls, and ran differential
conformance (fixtures, self-tokenization, 2,400+ randomized trials) against
`pptoken-ref`.

## Ownership trace (clean)

- **Code points**: `SourceDecoder` is the single owner of BOM strip, UTF-8
  decode, trigraph, line splice, UCN decode, and raw mode. Backtracking is
  buffer-position rewind with bounded distance (UCN ≤ 10 cps, raw delimiter
  ≤ 17, operator lookahead ≤ 3); one shared `Lookahead` helper owns
  save/restore semantics for `peek`/`peek_is_ucn`.
- **Tokens**: one forward maximal-munch loop; spellings are UTF-8 re-encoded
  once from decoded code points into a per-token string and emitted directly
  into `IPPTokenStream` — no intermediate token vector, no second render.
- **Directive context**: single 4-state enum (`StartOfLine → AfterDirectiveHash
  → ExpectingHeaderName → NoDirective`) owned by the tokenize loop; only
  whitespace-sequences are transparent. Header context correctly forms from
  trigraph/splice/comment-built `#`/`include`.
- **Presentation**: `DebugPPTokenStream` renders once at the tool boundary;
  the lexer core never touches iostreams. Static state is const tables only
  (Annex E1/E2 ranges, identifier-like operator set), so in-process batch
  re-entry is safe.

## Findings and changes (this cleanup)

1. **Per-token flush in `DebugPPTokenStream` (material, fixed)**: `endl` after
   every token forced one `write(2)` per token — 2,097,154 writes for a 2.1M
   token input; ~88% of tool wall time. Changed to `"\n"` (byte-identical
   output; `cout` flushes at exit). Writes drop to 15,361; 8 MB input: 5,912 ms
   → 1,376 ms.
2. **Empty character literal rejected at the wrong layer (material, fixed)**:
   the lexer threw on `''`, but the reference (and GCC's preprocessor) emit a
   `character-literal` token and defer rejection to literal evaluation.
   Graded fixture `cppgm.tests/course/pa2/300-invalid-character-string-boundary`
   depends on acceptance; it now matches the reference byte-identically
   (including ud-suffix form `''x`).
3. **Duplicated lookahead machinery (fixed)**: `peek` and `peek_is_ucn`
   each carried their own 20-line save/restore try/catch; consolidated into one
   private `Lookahead(lookahead, from_ucn&)`.
4. **Parallel line-position state (fixed)**: `at_start_of_line` bool duplicated
   what the directive enum could own; merged as the `StartOfLine` state.
5. **Inverted comment-scan conditional (fixed)**: `ConsumeWhitespaceSequence`
   nested block-comment handling inside a negated line-comment test; flattened
   to three positive cases.

## Performance evidence

Tool wall time (release build, output to /dev/null), representative C++-like
input: 1 MB 187 ms / 2 MB 372 ms / 4 MB 717 ms / 8 MB 1,419 ms — linear.
Lexer core via null sink: 4 MB, 1.69M tokens in 346 ms (~4.9M tokens/s);
remaining tool cost is buffered formatting. Adversarial 4 MB inputs are all
linear and token-count-bound, none superlinear: raw-string body of `)` with
16-char delimiter 285 ms, dense trigraphs 702 ms, line splices 119 ms, one
4 MB identifier 197 ms, dense UCN identifiers 164 ms, 4M one-char operators
1,329 ms, escape-heavy literal 276 ms. Per-char temporaries stay within SSO
(no heap churn); per-position lookahead constants are bounded (≤ 4-char
operator probes, ≤ 18-char raw-string closer).

## Conformance validation

- `make test-report-through-pa1`: 53/53; file audit: 12 files pass.
- Self-tokenization: `pptoken_lexer.cpp`, `pptoken.cpp`, `test_runner.cpp`
  produce byte-identical output and exit status vs `pptoken-ref`.
- Randomized differential fuzz (2,400+ token-soup trials incl. invalid bytes):
  every mismatch falls into a documented reference-quirk envelope, and a byte
  sweep of **all 335 test inputs across every stage (pa1–pa39)** proves the
  envelope is absent from the graded corpus (only `pa1/tests/300-utf8-ff.t` is
  invalid UTF-8, where both implementations agree on error).

Reference-quirk envelope (reference deviates from N3485/RFC 3629; our
standard-conformant behavior kept, not emulated):

- **Cooked pushback**: characters flushed by a failed trigraph/splice/UCN
  lookahead bypass later translations in the reference (`?A` leaves `\`
  raw; `\\`+LF does not splice; `\`/`??/` before `??=` suppresses the
  trigraph; failed UCN hex flush cooks the failing char). Standard phases 1–2
  are position-ordered rescans; ours re-scans.
- **Decoder permissiveness**: reference maps stray 0x80–0x9F via CP1252,
  accepts overlong encodings (`C1 81` → `A`) and UTF-16 surrogates; RFC 3629
  (cited by the assignment) forbids all three; ours rejects.
- **EOF splice/backslash**: reference silently drops a trailing `\` and does
  not append the terminating newline after an EOF splice (errors if that
  splice ends a `//` comment); C++11 2.2p1 appends a new-line in these cases.
- **Hex-token exponent-sign**: reference disables the `e/E`+sign pp-number
  rule for tokens starting `0x` (`0x1e-2` splits); the C++11 pp-number grammar
  has no hex mode (ours: one pp-number).

## Checkpoint ledger

| id  | scope                                          | proof | status |
|-----|------------------------------------------------|-------|--------|
| CP1 | decoder pipeline + core tokenizer + build wiring | 25/53, prior-through + file audit pass | DONE |
| CP2 | literal family + escape validation              | 49/53, prior-through + file audit pass | DONE |
| CP3 | header-name context + integration               | 53/53 through-report + file audit pass | DONE |
| CP4 | final architecture cleanup (this audit)         | 53/53 + file audit pass; flush fix, `''` layer fix, lookahead/context consolidation; linear-scaling + differential evidence above | DONE |
