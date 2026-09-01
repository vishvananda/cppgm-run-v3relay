# PA4 Final Architecture Audit — macro

## Scope and method

Traced representative facts through their full ownership paths: stdin →
`PPTokenize` (PA1, untouched) → `PPTokenCollector` (whitespace collapsed to
per-token flags) → line carve + directive dispatch (`MacroProcessFile`) →
`MacroTable` (define/undef/redefinition authority) → `MacroExpander::Expand`
(deque rescan, paint, invocation collection, substitution, `#`/`##`) → replay
into ONE `PostTokenStream` (PA2, untouched) → `DebugPostTokenOutputStream`.
Measured scaling on 25k/50k/100k-line inputs, probed nesting-depth and
paint-chain pathologies against `pa4/macro-ref`, and ran differential
conformance: the README traces, ~40 directed reference probes, and a
3000-seed randomized grammar fuzzer (defines, variadics, `#`/`##`, undefs,
invocation soup) comparing stdout bytes and exit status.

## Ownership trace (clean)

- **Token facts**: `PPToken` is the single value vocabulary (kind enum per
  `IPPTokenStream` event + `NEW_LINE` + internal `PLACEMARKER`, spelling,
  `preceded_by_ws` flag, paint set, noninvokable flag). Whitespace-presence
  is decided once at collection; redefinition equivalence
  (`MacroEquivalent`) and stringization consume the flag, never re-derive
  from text. Placemarkers cannot escape: `ReplayToken` throws on one — a
  defensive invariant, unreachable through `ResolvePastes`.
- **Directive layer**: `MacroProcessFile` carves raw lines before any
  replacement and is the only judge of line-initial `#`: `#define` →
  `MacroTable::Define` (which owns the full grammar: fn-like detection by
  adjacency, parameter-list validation, must-whitespace, define-time `#`/`##`
  constraints, redefinition equivalence), `#undef` → `ProcessUndef`, a lone
  `#` → null directive (flush + no-op), anything else → error. Mid-line
  `#` is never a directive; expansion output cannot create one.
- **Expansion**: `MacroExpander::Expand` owns the rescan (deque; pop head,
  push replacement to the front, replacements may consume following source
  tokens). `TakeInvocation` owns argument carving (paren depth, top-level
  commas kept in `Invocation.commas`); `NormalizeArguments` owns the
  argument-count contract; `FunctionReplacementBuilder` owns substitution:
  lazy memoized per-parameter pre-expansion, raw args for `#`/`##`,
  placemarkers, the GNU `, ## __VA_ARGS__` rule. Paste re-lexes the joined
  spelling through `PPTokenize` — a spec-legitimate text boundary
  (spec.md §2: preprocessor paste and stringization).
- **Emission**: all replaced output replays into one `PostTokenStream`, so
  PA2 phase-6 string concatenation crosses lines, text-sequence boundaries,
  and macro-produced strings (fixture 470 concatenates a stringize result
  with a paste-produced u8 literal); `emit_eof` once. `dev/macro.cpp` is a
  thin adapter owning iostreams and the `MacroError` → EXIT_FAILURE envelope.

## Paint model (course-specific, reference-pinned)

Prosser hide-sets over sorted `vector<string>` with course deviations,
validated by the 6xx/9xx fixtures and the README traces:

1. An identifier whose own name is in its paint is permanently noninvokable.
2. Object-like replacement copies get `HS ∪ {T}`.
3. Fn-like replacement-list copies and `##` results get `(HS ∩ HSr) ∪ {T}`
   (HSr = closing-paren paint); source-origin parens strip inherited paint.
4. Substituted argument tokens get `own ∪ HS ∪ {T}` — no intersection
   (README `g(f)(g)(3)` trace; paint acquired during argument pre-expansion
   persists through substitution).
   4a. **Helper-head boundary** (previously implemented but undocumented):
   when the parameter occurrence is immediately followed by `(` in the
   replacement list, the substituted tokens skip the `HS` union and get only
   `own ∪ {T}`. This is what lets a dispatched helper name expand at its
   new call site (920-deferred-helper-argument-prescan,
   600-parameter-selected/pasted-helper/tail-helper rescans) while plain
   substitutions still chain paint.
5. Paste results additionally union both operands' paint.

## Findings and changes (this cleanup)

All four material findings were surfaced by the differential fuzzer or
directed probes derived from its counterexamples; each is now pinned by a
checked-in fixture regenerated from `macro-ref` via `make -C pa4 ref-test`.

1. **`__VA_ARGS__` validated below its owning layer (material, fixed)**: a
   raw pre-scan of each text-sequence rejected spellings the rescan never
   examines (stringized `S(__VA_ARGS__)`, arguments of unused parameters,
   a `##` operand consumed into a paste) and missed `__VA_ARGS__` tokens
   *produced* by paste (`CAT(__VA_AR, GS__)` succeeded; reference fails).
   The check now lives in the rescan loop — the error fires exactly when a
   `__VA_ARGS__` token is examined as an expansion candidate. Fixtures
   450-vaargs-scan-boundary, 455-vaargs-paste-error.
2. **Excess-argument rule diverged (material, fixed)**: non-variadic
   invocations required an exact argument count; the reference merges excess
   arguments, separating commas included, into the last named parameter
   (`B(a)` + `B(1, 2)` → `a = 1 , 2`), and a zero-parameter macro accepts
   only an empty list. `ValidateArgumentCount` became `NormalizeArguments`,
   which consumes the whole `Invocation` (arguments + commas — one
   authority, mirroring the variadic tail build). Fixtures
   470-excess-args-merge (incl. stringize and paste over a merged tail),
   475-excess-args-zero-param.
3. **Zero-parameter `#` over-rejected at define time (material, fixed)**:
   `#define G() # x` is legal; at invocation `#` stringizes the next
   replacement token itself — even a `#` or `##` token (`# ## y` → `"##" y`,
   so that `##` never acts as a paste). The define-time
   `#`-requires-parameter rule still applies whenever the macro has named
   parameters or is variadic (300-badhash1-* unchanged; variadic case now
   pinned by 465-badhash-variadic). Fixture 460-zero-param-hash.
4. **Line-initial `#` fell through to text (material, fixed)**: a
   non-define/undef directive line was expanded as text (`#` emitted as
   `invalid #`). Per the reference: a lone `#` is the null directive
   (no-op, terminates the text-sequence); any other unrecognized directive
   line is an error. Fixtures 480-null-directive, 485-invalid-directive.
5. **Documented divergence (accepted)**: a line-initial directive appearing
   while a function-like invocation is mid-collection (`F (` … `#define A 1`
   … `1 )`) is argument text to the reference but terminates the
   text-sequence here (→ unterminated-invocation error). The README states
   line-initial `#` "must start a preprocessing directive" and restricts
   inputs to define/undef directives; no fixture pins the interleaving, and
   TESTING_AND_REFERENCES.md prefers the handout over reference parity
   outside the suites. Keeping the carve-before-expand single pipeline
   avoids entangling directive recognition with expansion state.

## Performance evidence

Release build, output to /dev/null (`/usr/bin/time`, representative runs
re-measured after the fixes; unchanged before/after):

- Linear scaling: 25k/50k/100k-line invocation-heavy inputs (`#`/`##`/
  stringize per line): 0.45s / 0.90s / 1.85s, RSS 50/96/176 MB. Time and
  memory are linear; RSS is the whole-file token buffer (~1.7 KB/line),
  plan-accepted at stage scale.
- Nesting depth (`f(f(…f(q)…))`): depth 2000: 2.84s / 488 MB vs reference
  3.07s / 1.5 GB; depth 3000: ours 6.5s / 1.1 GB, reference survives;
  depth 4000: both SIGSEGV (ours after 1.8 GB, reference after 5.3 GB).
  Identical envelope class — eager per-level argument pre-expansion recurses
  once per nesting level — and we answer every depth at strictly lower cost.
- Paint chains (n-deep object chain invoked 500×): n=200/400/800 →
  1.34/4.93/18.7s vs reference 0.27/0.76/2.90s. Same O(n²) complexity class
  (hide-sets inherently grow with chain depth); the ~6.5× constant is the
  sorted `vector<string>` hide-set representation. Real-shape cost is
  unaffected (100k probe 1.85s), so the plan's decision rule ("optimize only
  if the probe fails") holds; interning paint names to small ids is the
  recorded upgrade when the shared core faces preproc-scale inputs (pa5+),
  and is the measured exception spec.md §4 requires for owning strings.
- No accidental quadratics on real shapes: expansion prepends to the deque
  (never splices a vector middle), `TakeInvocation` erases one front range,
  text accumulates by append. `HasVisibleReplacement` rescans a replacement
  prefix per appended element; the prefix is O(1) in practice (first visible
  element sits at a fixed index) and only an adversarial all-placemarker
  prefix — many empty `##`-adjacent parameters on one directive line — makes
  it O(line²). Accepted and recorded.

## Conformance validation

- `make test-report-through-pa4`: 184/184 (53 pa1 + 28 pa2 + 23 pa3 +
  47 pa4 local + 33 pa4 course), including the eight new boundary fixtures;
  file audit: 23 files pass. `make -C pa4 ref-test` regenerates every
  existing `.ref` byte-identically.
- README traces (`f(f(x))` chain, `z` repaint, `g(f)(g)(3)`, `f(g)b)`
  cross-boundary rescan) match the reference byte-for-byte.
- Randomized differential: 3000 seeded programs — 0 mismatches in stdout
  bytes or exit status (earlier rounds surfaced findings 1–4; one 10s
  timeout under load did not reproduce). Directed probes: ~40 reference
  comparisons across `__VA_ARGS__` placement, zero-parameter `#`, argument
  counts, directive shapes, EOF-without-newline, and stringize escaping.
- Divergence envelope: only item 5 above (directive during active argument
  collection), deliberate and README-faithful.

## Checkpoint ledger

| id  | scope                                              | proof | status |
|-----|----------------------------------------------------|-------|--------|
| CP1 | directive layer, macro table, expansion core, paint, single replay | 51/72 pa4 (deferred groups only); 104/104 through-pa3; file audit pass | DONE |
| CP2 | stringize, paste + re-lex, placemarkers, variadics, ws-flag polish | 72/72 pa4; 176/176 through-pa4; 100k probe 1.81s | DONE |
| CP3 | final audit: `__VA_ARGS__` owner, excess-arg merge, zero-param `#`, directive dispatch, paint doc, perf envelope | 184/184 with 8 new fixtures; 3000-seed fuzz clean; linear scaling; envelope ⊇ reference | DONE |
