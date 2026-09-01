# PA4 Plan — macro (phases 1–6 + phase-7 tokenization with #define/#undef) [COMPLETE]

Final state: 184/184 fixtures pass through `make test-report-through-pa4`
(53 pa1 + 28 pa2 + 23 pa3 + 47 pa4 local including eight boundary fixtures
added at CP3 + 33 pa4 course); file audit passes (23 files). Architecture
review, findings, performance evidence, and conformance validation are
consolidated in `pa4/audit.md`.

## Stage Design (as built)

Owning boundary: macro owns splitting the phase-1–3 pp-token stream into
directives and text-sequences, the macro table, and 16.3 replacement
(spec.md [cpp.replace] plus the course deviations recorded in the audit).
PA1 `PPTokenize` and PA2 `PostTokenStream` are reused untouched. Pipeline:

    stdin → PPTokenize → PPTokenCollector (vector<PPToken>)
          → line carve: #define/#undef/null-#/#-error | text accumulation
          → MacroTable updates | MacroExpander deque rescan
          → replay all output tokens into ONE PostTokenStream
          → DebugPostTokenOutputStream → stdout

- `dev/src/macro_replace.h/.cpp` —
  - `PPToken` value struct: kind enum (one per `IPPTokenStream` data event +
    `NEW_LINE` + internal `PLACEMARKER`), spelling, `preceded_by_ws` flag
    (whitespace collapses at collection; new-lines stay tokens), paint set
    (sorted `vector<string>`), noninvokable flag.
  - `MacroTable::Define` parses and validates the whole directive line:
    fn-like by `(`-adjacency, parameter list, must-whitespace, `##` never at
    a boundary, `#` before a parameter when the macro has named parameters
    or is variadic (zero-parameter `#` is deferred to invocation),
    redefinition equivalence on kind/params/spellings/interior-ws flags.
  - `MacroExpander::Expand` — stack rescan over a deque; paint per the
    audit's model (incl. the helper-head boundary, rule 4a); `__VA_ARGS__`
    errors exactly when examined as an expansion candidate.
    `NormalizeArguments`: `M()` is one empty arg for 1+-param macros, too
    few args error, excess args merge (commas included) into the last named
    parameter, variadic tail → `__VA_ARGS__`.
    `FunctionReplacementBuilder`: lazy memoized per-parameter pre-expansion;
    `#` stringizes raw args (zero-param: the next replacement token
    itself); `##` uses raw args, placemarkers for empty ones, GNU
    `, ## __VA_ARGS__`; paste re-lexes via `PPTokenize` and rescans.
  - Directive dispatch: line-initial `#` always starts a directive —
    define, undef, lone-`#` no-op, otherwise error. Directives terminate
    text-sequences (a pending fn-like name is emitted plain; a directive
    inside active argument collection errors — deliberate, audit item 5).
- `dev/macro.cpp` — thin adapter: batch-stdin guard, stdin →
  `MacroProcessFile`, `MacroError`/exception → EXIT_FAILURE.
- `dev/src/posttoken_debug.h` — PA2 debug output formatting shared by
  `posttoken.cpp` and `macro.cpp` (single production implementation).
- `dev/frontend_source_sets.mk` — macro links `macro_replace pptoken_lexer
  posttoken_stream posttoken_tables unicode`.

Output: one `PostTokenStream` consumes the entire file's replaced output, so
phase-6 string concatenation crosses lines and text-sequence boundaries;
`#`/`##`/`@` in output surface as `invalid …` with EXIT_SUCCESS.

## Checkpoint Ledger

- CP1 (DONE) — directive layer, macro table with define-time validation,
  expansion core with paint model, arg collection + lazy pre-expansion,
  single-PostTokenStream replay. Proof: 51/72 pa4 (remaining failures
  confined to deferred #/##/varargs groups); 104/104 through-pa3; audit pass.
- CP2 (DONE) — stringize, paste + re-lex, placemarkers, variadics + GNU
  comma rule, ws-flag propagation. Proof: 72/72 pa4; 176/176 through-pa4;
  file audit pass; 100k-expansion probe 1.81s.
- CP3 (DONE) — final architecture audit + cleanup: moved `__VA_ARGS__`
  validation to its owning layer (the rescan), reference-pinned
  excess-argument merge, zero-parameter `#`, and line-initial `#` directive
  dispatch; documented the helper-head paint boundary; measured the
  nesting/paint-chain envelope against the reference. Proof: 184/184 with
  eight new fixtures (450/455/460/465/470/475/480/485); 3000-seed
  differential fuzz clean; linear 25k/50k/100k scaling; details in
  `pa4/audit.md`.
