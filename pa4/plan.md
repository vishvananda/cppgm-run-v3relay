# PA4 Plan — macro (phases 1–6 + phase-7 tokenization with #define/#undef)

State at planning: 0/72 pa4 fixtures pass (tool is the EXIT_NOT_IMPLEMENTED
stub); through-pa3 green; file audit green. Oracle: `.ref` files; failing
fixtures compare exit status only.

## Stage Design

Owning boundary: macro owns splitting the phase-1–3 pp-token stream into
directives and text-sequences, the macro table, and 16.3 replacement
(spec.md [cpp.replace] 16.3.1–16.3.5 plus the course deviations below).
PA1 `PPTokenize` and PA2 `PostTokenStream` are reused untouched. Pipeline:

    stdin → PPTokenize → PPTokenCollector (vector<PPToken>)
          → directive carve → MacroTable updates | text-sequence expansion
          → replay all output tokens into ONE PostTokenStream
          → DebugPostTokenOutputStream → stdout

- `PPToken` value struct: kind (one enumerator per IPPTokenStream data event
  + NEW_LINE + internal PLACEMARKER), spelling, `preceded_by_ws` flag
  (whitespace-sequences collapse into the flag; new-lines stay tokens until
  text-sequence expansion collapses them into flags too), paint set,
  noninvokable flag.
- Directive carve happens BEFORE any replacement, on raw lines only:
  (start-of-file | new-line) [ws] `#` … new-line. `#` mid-line is text
  (150-hash-outside), `#` produced by expansion never starts a directive
  (600-hash-from-macro), directive-shaped tokens mid-line inside an
  invocation are plain text (300-directive-tokens-from-macro-argument).
- Text-sequences are maximal non-directive runs. Invocations span new-lines,
  blank lines, and comments (400-fun-macro-define, 200-whitespaces) but never
  a directive: a fn-like name pending at a directive boundary is emitted as a
  plain identifier (400-fun-macro-define first `A`).
- ONE PostTokenStream instance consumes the entire file's replaced output, so
  PA2 phase-6 string concatenation crosses source lines and text-sequence
  boundaries (700-strlit-q: `"vers2.h"` + `"hello"` concatenate); `emit_eof`
  once at end. `#`/`##`/`@` in output surface as `invalid …` lines with
  EXIT_SUCCESS (150-hash-outside, 200-invalid-token-macro-replacement).
- Errors throw a `MacroError : std::runtime_error`; main catches → 
  EXIT_FAILURE (stdout irrelevant for failing fixtures).

### Paint (blue-paint) model — course-specific, fixture-validated

Hand-verified against 600-recurse, 650-recurse, 900-recurse, 910-recurse2,
600-macro-rescan-boundaries, 600-{parameter-selected,pasted-helper,
tail-helper}-macro-rescans, 600-unavailable-paint-through-function-replacement,
920-deferred-helper-argument-prescan. It is Prosser's hide-set algorithm with
ONE deviation. Let head token T carry paint HS; for fn-like invocations let
HSr be the paint of the closing `)` token of the invocation:

1. Candidate check: identifier whose own name ∈ its paint is noninvokable —
   emitted as a plain identifier, permanently.
2. Object-like replacement: replacement-copy tokens get HS ∪ {T}
   (chain accumulation; 600-recurse).
3. Fn-like replacement-list copies AND ##-paste results get
   (HS ∩ HSr) ∪ {T}. The intersection is mandatory: source-origin parens
   (empty paint) strip inherited paint, which is what keeps `CAT` alive in
   600-pasted-helper and `IIF` alive in 600-parameter-selected, and what
   kills the re-pasted `G0` in 600-unavailable-paint (parens there come from
   a replacement and carry {G0}).
4. COURSE DEVIATION: substituted argument tokens (raw or pre-expanded) get
   own_paint ∪ HS ∪ {T} — NO intersection. This makes `g(f)(g)(3)` yield
   `2 1 g ( 3 )` (910-recurse2, README trace) and `g ( 3 )`
   (600-macro-rescan-boundaries group 3) where pure Prosser/gcc would expand
   the final g. Paint acquired during argument pre-expansion persists through
   substitution (README z example; IDENTITY(RECUR(x))).
5. Paste results additionally union both operand tokens' paint
   (conservative; all fixtures pass either way — see Uncertainties).
6. Argument pre-expansion = same algorithm run on the argument token list in
   isolation, lazily per parameter (only parameters with an ordinary
   occurrence expand; memoize once per parameter): 250-stringized-argument-
   not-expanded succeeds while 300-nonstringized-invalid-macro-argument must
   fail (expanding `max(0)` → wrong arg count → error).

Rescan is a stack: pop head, on invocation push replacement back; replacement
output may consume following source tokens (README `f(g)b)`). Fn-like
lookahead for `(` inspects the next non-ws/non-newline UNEXPANDED token
(`CALL OPEN )` → `CALL ( )`, never reconsidered).

### Directive semantics (fixture-pinned)

- `#define name` obj-like: must-whitespace before a nonempty replacement;
  `(` immediately after name (no ws) → fn-like (`#define f (x)` is
  obj-like). `#define X(a,)` / `Y(a,,b)` → error (300-identifier-missing).
- Define-time validation: fn-like — every `#` in replacement must be followed
  (ws allowed between: `# a` legal per 500-tricky-join) by a parameter name
  or `__VA_ARGS__`-if-variadic, else error (300-badhash1-1/2/3); `##` at
  either end of any replacement list → error (300-badhash2-1..4). Obj-like
  `#` is unrestricted (600-hash-from-macro).
- `__VA_ARGS__` anywhere outside a variadic replacement list → EXIT_FAILURE:
  obj repl, non-variadic fn repl, as macro name, bare in text, as parameter,
  in #undef (250-badvargs1-4, 250-badvaargs-param, 250-badvaargs-undef).
- Redefinition must match: kind, exact parameter NAMES (700-redeferr3/4),
  replacement spellings, and interior whitespace-PRESENCE flags
  (`(1-1)` vs `(1 - 1)` errs — 700-redeferr2; `D E` vs `D  E` equal —
  300-macro-redefinition-whitespace). Ignored: first-replacement-token flag
  (700-redef2 `)( ` vs `)    (`), trailing ws, all param-list ws; comments
  count as ws. Obj/fn mismatch errs (300-object-function-macro-redefinition).
- `#undef`: undefined name OK, trailing ws OK (100-undef-simple); any extra
  token → error (300-undef-extra). `#define X(a` unterminated → error
  (300-unterminated-function-macro-parameter[s], -after-comma).

### Invocation & operators (fixture-pinned)

- Args: paren-balanced, top-level commas split (250-nested-macro-argument-
  commas); named count must match exactly, else EXIT_FAILURE; `M()` is one
  empty arg for 1-param macros, zero args for 0-param (250-empty-function-
  macro-argument); EOF during collection → error. Variadic: trailing args
  incl. commas become `__VA_ARGS__` (250-goodvaargs → `3 , 4`); empty
  varargs OK (`CALL()`).
- Substitution: ordinary → expanded arg; `#p` → stringized RAW arg;
  `##`-adjacent → RAW arg. Multi-token raw arg: paste uses only its
  first/last token, middle tokens pass through
  (300-token-paste-multiple-parameter-tokens → `aA . Bb`). Only `##` copied
  from the replacement list pastes; `##` arriving via arguments or produced
  by paste is an ordinary (invalid-on-output) token (150-hash-outside
  `f(##)`, call_triple + hash_hash → `pP ## Qq`).
- Placemarkers for empty args in `##` context: pm##X→X, X##pm→X, pm##pm→pm,
  survivors deleted after substitution (250-join, 800-placemarker-q).
- GNU comma-paste: replacement-list `, ## __VA_ARGS__` — empty varargs
  deletes the comma; non-empty: no paste at all, comma + args verbatim
  (300-gnu-variadic-comma-paste ref: `target ( 1 )` / `target ( 1 , alpha ,
  beta )`).
- Paste: concatenate the two spellings, re-lex via PPTokenize (append `\n`,
  expect exactly one data token before eof, else error); result is rescanned
  (600-pasted-helper `CALL_0`, tail-helper `FILLER_1_END`). Must handle
  `#`+`#`→`##` (500-tricky-join), raw-string assembly `R` ## `"abc(…)abc"`
  ## `_id` → UD raw string (500-raw-string-token-paste), u8/L prefixes and
  `̀` identifier continuation (300-double-hash).
- Stringize: raw arg spellings joined with one space exactly where the
  token's `preceded_by_ws` flag is set; no outer spaces; escape `\` and `"`
  only within string/char-literal spellings; raw-string spelling verbatim
  (410-trigraph → `"R\"(??=)\""`); empty arg → `""`. Flag propagation:
  expansion's first token inherits the macro-name token's flag, substituted
  param's first token inherits the occurrence's flag, paste result inherits
  left operand's, others keep their own (evidence: `"x ## y"` 500-tricky-join,
  `"A . ."` 200-whitespaces, `"strncmp(…) == 0"` 700-strlit-q spacing).

## Failure Map

All 72 failures = one stub; ownership after CP1 split:

- Directive layer + table (CP1): 100-*, 200-onedef, 300-identifier-missing,
  300-undef-extra, 300-unterminated-*, 300-redef, 300-define-missing-macro-
  name, 300-object-function-macro-redefinition, 300-macro-redefinition-
  whitespace, 700-redef2, 700-redeferr1-4, define-time halves of 300-badhash*
  and 250-badvargs/badvaargs*.
- Expansion core + paint (CP1): 150-max, 200-fnlike, 200-function-macro-
  invocation-boundaries, 250-empty/nested-*, 300-directive-tokens-from-
  macro-argument, 400-fun-macro-define, 600-recurse, 600-hash-from-macro,
  650-recurse, 700-redef-a, 900-recurse, 910-recurse2, pass-through text
  (700-strlit-a/a2, 800-placemarker-a, 850-varargs-a, 500-hello-world,
  200-invalid-token-*, 410-trigraph).
- Operators/varargs (CP2): 150-hash-outside, 200-whitespaces, 250-join,
  250-goodvaargs, 250-stringized-*, 300-nonstringized-*, 300-double-hash,
  300-token-paste-*, 300-gnu-variadic-comma-paste, 500-tricky-join,
  500-raw-string-token-paste, 600-*-macro-rescans, 600-unavailable-paint-*,
  600-macro-rescan-boundaries (group 2 variadic), 700-redef-q, 700-strlit-q,
  800-placemarker-q, 850-varargs-q, 920-deferred-*.

## Performance Risks

- Paint sets copied per produced token. Bounded: a chain can only grow a
  paint set by names of distinct macros, so |paint| ≤ #defined macros; store
  as sorted `vector<string>` (or interned ids) with set-union helpers; share
  the per-invocation replacement set. Optimize only if the probe fails.
- Stack rescan re-examines replacement output; BOOST_PP-style fixtures (920,
  600-*) are the worst present cases and are small. Guard against accidental
  O(n²) in token-vector splicing: expansion should append to a deque/stack,
  never insert into the middle of a vector.
- Whole-file token buffering is fine at fixture scale; probe with 100k-line
  input to confirm linear behavior (< 2s, matching pa3 probe budgets).

## Checkpoint Ledger

- CP1 (ACTIVE) — directive layer, macro table with full define-time
  validation, expansion core with paint model, arg collection + lazy
  pre-expansion + ordinary substitution (no #/##/__VA_ARGS__ substitution
  yet), single-PostTokenStream replay, tool main. Proof: `make test-pa4`
  failures drop 72 → ≈30 (every fixture whose semantics avoid #/##/varargs,
  incl. all define-error and recursion fixtures above);
  `make test-report-through-pa3` stays green; file audit green.
- CP2 — stringize, paste + re-lex, placemarkers, variadics + GNU comma rule,
  ws-flag propagation polish. Proof: 72/72 `make test-pa4` and clean
  `make test-report-through-pa4`; through-pa3 unchanged.
- CP3 — architecture audit + cleanup: perf probe recorded, optional
  differential run against `pa4/macro-ref` on generated inputs (observation
  only), consolidate findings in `pa4/audit.md`. Proof: clean
  `make test-report-through-pa4`, audit script green, git clean.

## Active Checkpoint: CP1

Build the whole pipeline minus the #/##/varargs operators. Include ALL
define-time validation (badhash/badvargs errors are parse-time and cheap) so
error fixtures pass even though operator substitution waits for CP2. Where an
operator would fire in substitution (a `#p`/`##` actually reached), CP1 may
throw MacroError — those fixtures stay red until CP2 and prove nothing.

### Implementation Packet

Files/symbols (new files go in `dev/src/`, listed in
`dev/frontend_source_sets.mk`):

- NEW `dev/src/macro_replace.h` / `macro_replace.cpp`:
  - `enum EPPTokenKind` (header-name, identifier, pp-number, char-lit,
    ud-char-lit, string-lit, ud-string-lit, op-or-punc, non-ws-char,
    new-line, placemarker);
  - `struct PPToken { EPPTokenKind kind; std::string data;
    bool preceded_by_ws; PaintSet paint; bool noninvokable; }` with
    `PaintSet` = sorted `std::vector<std::string>` + `PaintContains`,
    `PaintUnion`, `PaintIntersect` helpers;
  - `struct PPTokenCollector : IPPTokenStream` → `std::vector<PPToken>`
    (whitespace event → flag on next token; new-line kept as token; note
    `using std::string;` before including `IPPTokenStream.h`, as
    `ctrlexpr_eval.h:12` does);
  - `struct Macro { bool function_like; bool variadic;
    std::vector<std::string> params; std::vector<PPToken> replacement; }`;
  - `class MacroTable` — `Define` (parse from directive-line tokens,
    validate per plan, redefinition compare incl. ws flags), `Undef`,
    `Lookup`;
  - `class MacroExpander` — stack rescan over a text-sequence with an output
    callback; invocation detection, arg collection, lazy arg pre-expansion,
    substitution, paint rules 1–4 and 6 above;
  - driver `void MacroProcessFile(const std::string& input,
    IPostTokenOutputStream& output)` — collect, carve directives, expand
    text-sequences, replay every output token into one internal
    `PostTokenStream` via a kind→emit_* switch (mirror
    `CtrlExprLineSplitter::flush_line` in `dev/src/ctrlexpr_eval.cpp`),
    final `emit_eof`;
  - `class MacroError : public std::runtime_error`.
- NEW `dev/src/posttoken_debug.h` — hoist `DebugPostTokenOutputStream`
  verbatim from `dev/posttoken.cpp:57-137` (plus HexDump/ValueToHexChar);
  EDIT `dev/posttoken.cpp` to include it (pure move — through-pa2 gates it).
- EDIT `dev/macro.cpp` — replace stub body, mirroring `dev/posttoken.cpp`
  main: keep `--batch-stdin` guard and catch envelope (`MacroError`/any
  `std::exception` → EXIT_FAILURE), read all stdin, call `MacroProcessFile`.
- EDIT `dev/frontend_source_sets.mk:12` →
  `FRONTEND_OBJ_BASENAMES_macro := macro_replace pptoken_lexer
  posttoken_stream posttoken_tables unicode`.

Fixture groups for CP1 verification (see Failure Map): directive/table
errors + recursion + pass-through text; spot-check by hand:
`pa4/tests/600-recurse.t`, `910-recurse2.t`, `700-redef2.t`,
`cppgm.tests/course/pa4/400-fun-macro-define.t`,
`200-function-macro-invocation-boundaries.t`, `100-undef-simple.t`.

Required spec facts: spec.md 16.3 [cpp.replace] paras 1–4 (redefinition
identity, param lists, arg-count matching), 16.3.1 (argument substitution +
pre-expansion), 16.3.4 (rescanning; course paint deviation per this plan and
pa4/README.md traces), 16.3.5 examples (700-redef-q, 700-strlit-q,
800-placemarker, 850-varargs mirror them). pa4/README.md "Features" +
"Design Notes" are authoritative where they diverge from ISO.

Commands:
- focused: `make test-pa4` (builds tool, runs all 72)
- prior gate: `make test-report-through-pa3` (must stay green — posttoken.cpp
  is touched by the header hoist)
- broad: `make test-report-through-pa4`
- audit: `perl scripts/cppgm_file_audit.pl --stage pa4 --paths dev/src`
- single fixture: `dev/macro < pa4/tests/600-recurse.t | diff - pa4/tests/600-recurse.ref`

Performance probe (after CP1, again at CP3):

    perl -e 'print "#define max(a,b) ((a)>(b)?(a):(b))\n";
             print "max(1,2)\n" for 1..100000' \
      | timeout 10 dev/macro > /dev/null   # expect < 2s, linear

Known uncertainties (resolve against ref only if a fixture forces it):
- Non-define/undef directive lines (`# foo`, null `#`) are outside the input
  contract; choose EXIT_FAILURE.
- Duplicate parameter names: error per [cpp.replace]; untested.
- Paste not yielding exactly one pp-token: choose EXIT_FAILURE
  (300-double-hash comments deliberately avoid the case).
- GNU comma rule scope: implement narrowly (literal `,` `##` `__VA_ARGS__`
  in the replacement list only).
- Paste-result paint: operand union included (rule 5); fixtures pass with or
  without.
- String concatenation across a directive boundary: single-stream design
  concatenates (standard phase order); untested.
