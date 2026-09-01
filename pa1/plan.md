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
| CP1 | decoder pipeline + core tokenizer + build wiring            | group-1 fixtures (~24) pass in `make test-pa1`; no test regresses to a new failure kind | ACTIVE |
| CP2 | char/string/ud/raw literals + escape validation             | group-2 fixtures pass; ~48/53 green        | todo   |
| CP3 | header-name context + integration endgame                   | 53/53; `make test-report-through-pa1` clean; file audit still green | todo   |

Each checkpoint is one commit at a stable ownership boundary. Adding new tests
is out of scope; progress = existing failure reduction only.

## Active Checkpoint: CP1 — decoder + core tokenizer

Build the `SourceDecoder` (UTF-8 → trigraph → splice → UCN, BOM strip,
raw-mode flag, mark/rewind), UTF-8 encoder, E1/E2 tables, and the tokenizer
loop for: whitespace-sequence (incl. `//` and `/* */`, error on unterminated
block comment), new-line, identifier (checking the identifier-like-operator
set → preprocessing-op-or-punc), pp-number, full op/punc DFA (incl. digraphs,
`...`, `<::` exception, `.`/`..` retraction), non-whitespace-character, eof.
On `'` or `"` (incl. after encoding prefixes `u8 u U L` / `R`) throw
`NotImplementedException` so literal tests keep failing exactly as today
(exit 86) — CP2 replaces that arm. Wire the build and the thin `main`.

### Implementation Packet

- Files/symbols:
  - NEW `dev/src/pptoken_lexer.h`: `void PPTokenize(const std::string&,
    IPPTokenStream&);` with `struct IPPTokenStream;` forward declaration.
  - NEW `dev/src/pptoken_lexer.cpp`: `SourceDecoder` (members: `const
    std::string& buf`, `size_t pos`, `bool raw`; methods `next`, `peek(k)`,
    `mark`, `rewind`), `encode_utf8`, `AnnexE1_Allowed_RangesSorted` /
    `AnnexE2_DisallowedInitially_RangesSorted` (copied from stub
    `dev/pptoken.cpp:60-116`), `Digraph_IdentifierLike_Operators`,
    `PPTokenize` loop. Audit caps: ≤240 lines/function, ≤8 nesting — split the
    op DFA by leading character into helpers if needed.
  - EDIT `dev/pptoken.cpp`: keep `HasBatchStdinArg`/batch fallback and the
    try/catch shape; body becomes read-stdin + `DebugPPTokenStream out;
    PPTokenize(input, out);`. Delete moved tables and the stub `PPTokenizer`.
  - EDIT `dev/frontend_source_sets.mk` line 9:
    `FRONTEND_OBJ_BASENAMES_pptoken := pptoken_lexer`.
- Fixture groups: target = Failure Map group 1 (24 fixtures listed above);
  must-not-change = groups 2–3 keep failing with exit-86 mismatch, nothing new.
- Required spec facts (all in pa1/README.md, restated here): trigraphs
  `??= ??/ ??' ??( ??) ??! ??< ??> ??-` → `# \ ^ [ ] | { } ~`; UCN forms
  `\u`+4 hex / `\U`+8 hex, valid values ≤ 0x10FFFF (invalid → error);
  pp-number = `digit | . digit` then `digit | identifier-nondigit | e/E sign |
  .`; identifier = nondigit/E1 start (not E2), continue nondigit/digit/E1;
  op list incl. `<: :> <% %> %: %:%: ... ->* >>= <<=` and the 13
  identifier-like ops; whitespace cps: space, \t (0x09), \v (0x0B), \f (0x0C)
  (LF is its own token; CR see uncertainties).
- Focused commands:
  - `make -C dev pptoken`
  - `./dev/pptoken < pa1/tests/100-a.t | diff - pa1/tests/100-a.ref`
  - per-fixture loop over group 1, e.g.
    `for t in 100-comments 250-dot2-ppnum 400-angle-colon-madness ...; do
    ./dev/pptoken < pa1/tests/$t.t | diff - pa1/tests/$t.ref || echo FAIL $t;
    done` (cppgm fixtures live in `cppgm.tests/course/pa1/`).
- Broad commands: `make test-pa1` (per-test report; expect ~24 newly passing),
  then `make test-report-through-pa1` (stage exit criterion), then
  `perl scripts/cppgm_file_audit.pl --stage pa1 --paths dev/src`.
- Performance probe:
  `time ./dev/pptoken < pa1/tests/900-real-world.t > /dev/null` — must be
  effectively instant (<10 ms; harness kills a test at 10 s).
- Known uncertainties (resolve by observing `pa1/pptoken-ref` on probe inputs
  — observation is allowed; fixtures remain the only gate):
  1. `"\\u0040"`: does a UCN after an escaped backslash still stream-decode?
     (`"\\u{"` is proven verbatim, but `u{` is not a valid UCN there.)
  2. UCN surrogate values `\uD800–\uDFFF`: error or accepted?
  3. CR (0x0D): whitespace member or non-whitespace-character? (Fixtures are
     LF-only; pick ref behavior.)
  4. UTF-8 strictness beyond 0xFF: overlong encodings, encoded surrogates,
     truncated tails — error like 300-utf8-ff, or accept? Probe ref.
  5. Non-leading U+FEFF: identifier char (E1 says yes) vs stripped — only the
     file-leading BOM is fixture-proven stripped.
