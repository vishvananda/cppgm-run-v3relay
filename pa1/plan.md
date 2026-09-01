# PA1 Plan — pptoken (translation phases 1–3)

## Stage Design

Owning boundary: PA1 is the front of the one production pipeline
(`source buffers -> classified tokens`, spec.md §1). The lexer gets one
production implementation in `dev/src/`, reused by every later tool; the tool
entry `dev/pptoken.cpp` stays a thin adapter (read stdin → lex → emit through
`DebugPPTokenStream`). No intermediate token vector — emit directly into
`IPPTokenStream` (spec §1: no unused representations).

Structure (2 new files + 2 edits):

- `dev/src/pptoken_lexer.h` — declares
  `void PPTokenize(const std::string& input, IPPTokenStream& output);`
  (forward-declare `struct IPPTokenStream;` and use `std::string` explicitly;
  do not include `IPPTokenStream.h` here — it assumes `using namespace std`).
- `dev/src/pptoken_lexer.cpp` — everything else:
  - `SourceDecoder`: buffer + index pull-decoder over the whole input. Methods:
    `next()`/`peek(k)` returning code points, `mark()`/`rewind(mark)`, and a
    raw-mode flag. Normal mode applies, in order:
    UTF-8 decode → trigraph → line-splice (`\`+LF) → UCN decode.
    Raw mode applies UTF-8 decode only. Buffer-backed backtracking replaces all
    push-state lookahead machinery (trigraph `?` lookahead, `..`/`<::`
    retraction, ud-suffix and `u8"` lookahead, raw-mode entry).
  - `encode_utf8(int cp, std::string&)` for token spellings.
  - Annex E1/E2 tables (copy from stub `dev/pptoken.cpp:60-116` before
    deleting) + binary-search membership tests. Identifier-like operator set.
  - Tokenizer: single forward maximal-munch loop; a 3-state header-name
    context tracker fed by emitted tokens (start-of-file/new-line → `#` or
    `%:` → `include`; only whitespace-sequence is transparent).
- `dev/pptoken.cpp` — delete stub tables/`PPTokenizer`; keep the
  `--batch-stdin` fallback and catch-structure; `main` reads all of stdin and
  calls `PPTokenize`. Lex errors: throw `std::runtime_error` → `EXIT_FAILURE`
  (error text is never compared; failing fixtures check exit status only).
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_pptoken := pptoken_lexer`.

The test harness (`dev/src/test_runner.cpp`, built with
`-Dmain=test_runner_real_main`) already handles the batch protocol; the tool's
`main` only ever does plain stdin→stdout.

Global input rules: strip one leading U+FEFF BOM (it is inside an E1 range — a
naive identifier DFA would eat it); empty input emits only `eof`; non-empty
input not ending in LF gets one synthesized `new-line` token; invalid UTF-8
(e.g. 0xFF) is an error; `'` and `"` never match non-whitespace-character.
Never pass code points > 127 to `isspace`/ctype — use explicit sets.

## Failure Map

Single root cause today: the stub throws `NotImplementedException` → exit 86,
so all 53 tests fail with "exit status mismatch" and `make test-pa1` exits 2.
`priorThroughTests` is trivially green (PA1 is first); file audit is green.
Fixture groups by owning component:

1. Decoder + core tokens (~24 tests): identifiers, pp-number, op/punc,
   whitespace/comments, new-line, non-whitespace-character, UTF-8, trigraphs,
   splicing, UCN, BOM, bad-UTF-8/bad-UCN/unterminated-comment errors.
   Fixtures: pa1/tests `100-a, 100-empty, 100-comments, 100-floating,
   100-line-splice, 100-preprocessing-op-or-punc, 100-trigraphs,
   100-trigraph-lookahead-utf8, 100-universal-character-name, 100-utf8,
   200-trigraphs, 250-dot2-ppnum, 300-ucn-trigraph-ordering, 300-utf8-ff,
   400-angle-colon-madness, 500-isspace-code-point-wrong, 100-partial-comment`;
   cppgm.tests/course/pa1 `100-extra-comments, 100-line-splice-comment,
   100-more-floating, 100-pp-number-exponent-sign-boundary,
   200-identifier-unicode-e2-ranges, 300-utf8-bom,
   300-invalid-universal-character-value-bad, 200-trigraphs-and-comments,
   400-angle-colon-template-tokenization`.
2. Literal family (~19 tests): character/string literals, escape validation,
   ud-suffixes, raw strings + raw mode. Fixtures: pa1/tests
   `100-character-literals, 100-string-literals, 100-example,
   100-raw-string-literal, 100-partial-string-literal, 100-universal-character-name-max,
   150-ud-character-literals, 150-ud-string-literals, 200-charname-allowed,
   200-escape-sequence, 300-quote-strange`; cppgm.tests
   `100-raw-string-literal, 150-user-defined-literals,
   150-user-defined-raw-string-literal, 200-escaped-universal-name-prefix-string,
   200-non-raw-string-prefix, 200-raw-string-delimiter,
   200-string-escape-sequences, 300-raw-string-delimiter-bad,
   300-raw-string-delimiter-trigraph-bad, 300-string-hex-escape-bad,
   300-string-octal-escape-bad`.
3. Header-name context + integration (~5 tests): pa1/tests `200-header-name,
   900-real-world`; cppgm.tests `200-header-name,
   200-alternative-include-header-context,
   300-trigraph-line-splice-comment-include`.

Fixture-proven behavior facts (the oracle for tricky cases):

- `??/u0040` → non-whitespace-character `@` (trigraph produces `\` before UCN
  decode; decoded non-E1 code point becomes non-whitespace-character, no error).
- `..4214` → op `.` then pp-number `.4214`; `1p+3` → `1p`, `+`, `3` (sign
  joins pp-number only after `e`/`E`).
- `<::<::>::<:::` → `<` `::` `<:` `:>` `::` `<:` `::` (2.5.3: `<::` followed by
  neither `:` nor `>` splits as `<` + `::`).
- E1/E2 via UCN: `¨` → identifier; `§` → non-whitespace-character;
  leading `̀` (E2) → non-whitespace-character, but `id1̀` is one
  identifier. Token data is the UTF-8 re-encoding of decoded code points.
- `"\U0010FFFF"` → 6-byte string-literal (UCN decodes inside plain literals);
  `\UFFFFFFFF` → error. `"\\u{"` stays verbatim (only complete, valid
  `\uXXXX`/`\UXXXXXXXX` decode).
- Escape validation in non-raw char/string literals: `\x` needs ≥1 hex digit,
  octal is 1–3 digits, else simple-escape or UCN; `"\xg…"`, `"\8…"` → error;
  `"\xabcz"`, `"\x0z"`, `"\04y"`, `"\08"` are valid and stay verbatim.
- Raw strings: delimiter ≤16 d-chars (17 → error); body reverts trigraph/
  splice/UCN (raw mode); LF inside body emits no new-line token; unterminated
  at EOF → error. `"not a "R"(raw-string)"R` → two user-defined-string-literals
  (ud-suffix is a plain identifier; a suffix `R` does not open a raw string).
- Header context: `(SOF|new-line) (#|%:) include` with only
  whitespace-sequences transparent; `#notinclude` and `%:%:` (single `##`
  token) do not open it; `#`+`include` may themselves be built from trigraphs,
  splices, and comments (`??=/**/ inc??/`+LF+`lude` → `#` `include` context).
- Unterminated `/*` comment and unterminated `"` literal → EXIT_FAILURE.

## Performance Risks

Scale is tiny (largest fixture 4.1 KB) but keep the boundary honest:

- One forward pass, O(n): backtracking only via `mark()/rewind()` with bounded
  distance (UCN ≤ 10 cps, raw delimiter ≤ 17, op lookahead ≤ 3). No
  `string::erase(0,1)` queues, no regex, no per-char heap churn — reuse one
  spelling buffer per token.
- E1/E2 membership by binary search over the sorted range tables; no
  locale/ctype calls on non-ASCII.
- The batch worker re-enters `main` in-process per test: keep static state
  const-only (tables), no mutable globals.

## Checkpoint Ledger

| id  | scope                                                      | proof of progress                          | status |
|-----|------------------------------------------------------------|--------------------------------------------|--------|
| CP1 | decoder pipeline + core tokenizer + build wiring            | 24 packet core fixtures pass; `make test-pa1` is 25/53 (28 failures vs 53 at start); prior-through and file audit pass | DONE |
| CP2 | char/string/ud/raw literals + escape validation             | all 22 listed group-2 fixtures pass; `make test-pa1` and through report are 49/53 (4 deferred group-3 failures); prior-through and file audit pass | DONE |
| CP3 | header-name context + integration endgame                   | all 5 group-3 fixtures pass; `make test-pa1` and `make test-report-through-pa1` are 53/53; prior-through and file audit pass | DONE |

Each checkpoint is one commit at a stable ownership boundary. Adding new tests
is out of scope; progress = existing failure reduction only.

## Completed Checkpoint: CP3 — header-name context + integration

Add header-name context tracking to the CP1/CP2 tokenizer: at start of file or
after a new-line, recognize `#` or `%:` followed by transparent whitespace and
the identifier `include`, then emit the following `<...>` or `"..."` as a
header-name token. Preserve ordinary operator/string behavior outside that
context and keep the public `PPTokenize` boundary unchanged.

### Implementation Packet

- Files/symbols:
  - EDIT `dev/src/pptoken_lexer.cpp`: extend `PPTokenize`/`EmitCoreToken`
    ownership with bounded header-name scanning and directive context; accept
    trigraph-, splice-, and comment-formed `#`/`include` sequences.
  - Keep `dev/src/pptoken_lexer.h`’s public `PPTokenize` boundary and the
    existing CP1/CP2 build wiring unchanged.
- Fixture group: target = Failure Map group 3 (`pa1/tests/200-header-name`,
  `pa1/tests/900-real-world`, `cppgm.tests/course/pa1/200-header-name`,
  `cppgm.tests/course/pa1/200-alternative-include-header-context`,
  `cppgm.tests/course/pa1/300-trigraph-line-splice-comment-include`);
  preserve all prior 49 passing fixtures.
- Focused commands: `make -C dev pptoken`, then the group-3 fixture loop.
  Broad proof remains `make test-pa1`, `make test-report-through-pa1`, and
  `perl scripts/cppgm_file_audit.pl --stage pa1 --paths dev/src`.

## Active Checkpoint: PA2 planning handoff

PA1 is complete through CP3. The next active checkpoint is to packetize PA2
from its assignment contract before implementation; no PA1 checkpoint remains
active.
