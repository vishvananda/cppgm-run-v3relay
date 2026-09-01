# PA1 Plan — pptoken (translation phases 1–3) [COMPLETE]

Final state: 53/53 fixtures pass through `make test-report-through-pa1`; file
audit passes. Architecture review, findings, performance evidence, and the
reference-quirk envelope are consolidated in `pa1/audit.md`.

## Stage Design (as built)

Owning boundary: PA1 is the front of the one production pipeline
(`source buffers -> classified tokens`, spec.md §1). The lexer has one
production implementation in `dev/src/`, reused by every later tool; the tool
entry `dev/pptoken.cpp` is a thin adapter (read stdin → lex → emit through
`DebugPPTokenStream`). No intermediate token vector — tokens are emitted
directly into `IPPTokenStream`.

- `dev/src/pptoken_lexer.h` — `void PPTokenize(const std::string&, IPPTokenStream&);`
  (forward-declares `IPPTokenStream`; does not include `IPPTokenStream.h`,
  which assumes `using namespace std`).
- `dev/src/pptoken_lexer.cpp` — `SourceDecoder` (buffer + index pull-decoder:
  UTF-8 → trigraph → line-splice → UCN in normal mode, UTF-8 only in raw mode;
  bounded `mark()/rewind()` backtracking; one shared `Lookahead` helper),
  Annex E1/E2 range tables with binary search, identifier-like operator set,
  single forward maximal-munch tokenizer, 4-state directive-context enum
  (`StartOfLine`/`NoDirective`/`AfterDirectiveHash`/`ExpectingHeaderName`) for
  header-name recognition.
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_pptoken := pptoken_lexer`.

Global input rules: strip one leading U+FEFF BOM; empty input emits only
`eof`; non-empty input not ending in LF gets one synthesized `new-line`;
invalid UTF-8 is an error (strict RFC 3629); `'` and `"` never match
non-whitespace-character; no ctype calls on non-ASCII.

## Fixture-proven behavior facts (oracle for tricky cases)

- `??/u0040` → non-whitespace-character `@` (trigraph `\` feeds UCN decode).
- `..4214` → `.` then pp-number `.4214`; `1p+3` → `1p`, `+`, `3` (sign joins
  pp-number only after `e`/`E`).
- `<::<::>::<:::` → `<` `::` `<:` `:>` `::` `<:` `::` (2.5.3 `<::` rule).
- E1/E2 via UCN: `¨` → identifier; `§` → non-whitespace-character; leading
  E2 combining mark → non-whitespace-character but is valid in identifier
  body. Token data is the UTF-8 re-encoding of decoded code points.
- `"\U0010FFFF"` → 6-byte string-literal; `\UFFFFFFFF` → error; `"\\u{"`
  stays verbatim (only complete valid UCNs decode).
- Escapes in non-raw literals: `\x` needs ≥1 hex digit, octal 1–3 digits,
  else simple-escape or UCN; `"\xg…"`, `"\8…"` → error.
- `''` and `''x` are emitted as (user-defined-)character-literal tokens;
  rejection belongs to literal evaluation (pa2 fixture depends on this).
- Raw strings: delimiter ≤16 d-chars; body reverts trigraph/splice/UCN; a
  ud-suffix `R` does not open a raw string.
- Header context: `(SOF|new-line) (#|%:) include` with only
  whitespace-sequences transparent; `#`/`include` may be built from trigraphs,
  splices, and comments; `%:%:` does not open it.
- Unterminated `/*` comment or `"`/`'` literal → EXIT_FAILURE.

Divergences from `pptoken-ref` outside the graded corpus (cooked pushback,
decoder permissiveness, EOF-splice, hex exponent-sign) are documented with
evidence in `pa1/audit.md`; the full corpus of all stages was swept to prove
no fixture can hit them.

## Checkpoint Ledger

| id  | scope                                             | proof of progress | status |
|-----|---------------------------------------------------|-------------------|--------|
| CP1 | decoder pipeline + core tokenizer + build wiring   | 25/53; prior-through and file audit pass | DONE |
| CP2 | char/string/ud/raw literals + escape validation    | 49/53; prior-through and file audit pass | DONE |
| CP3 | header-name context + integration endgame          | 53/53 on `make test-report-through-pa1`; file audit pass | DONE |
| CP4 | final architecture cleanup + audit (`pa1/audit.md`) | 53/53; flush fix, `''` ownership fix, lookahead/context consolidation; linear scaling measured | DONE |

## Handoff

PA1 is complete. Next: packetize PA2 (posttoken) from its assignment contract
before implementation.
