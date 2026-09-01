# PA6 Plan — recog (syntactic recognition of translation-unit)

Goal: `recog -o <outfile> <srcfiles...>` preprocesses each srcfile with the
PA5 pipeline, tokenizes, and recognizes the token sequence against
`pa6.gram` (197 nonterminals). Outfile: first line `recog <N>`, then one
`<srcfile> OK|BAD` line per srcfile. Any per-file failure (open error,
preprocess/lex error, invalid token, parse failure) → `BAD` for that file
only; tool exits EXIT_SUCCESS. Harness compares exit status + byte-exact
outfile (`compare_text`); stdout is never compared, so the parse-tree dump
is optional. Per-test runner timeout: 10s (`test_runner.cpp`).

## Stage Design

Owning boundaries (spec.md §1: one production pipeline; the PA6 parser is
the seed of the production parser, later stages extend it — no parallel
parser later):

- `dev/src/preproc_engine.h/.cpp` — token production. Add a public
  per-srcfile entry that feeds a caller-supplied `IPostTokenOutputStream`
  instead of the PA5 text writer. PA5 output path stays byte-identical.
- `dev/src/parser/recog_token.h/.cpp` (new) — PA6 terminal layer. A
  `Pa6Token` carries typed classification computed once at construction
  (spec.md §2): `ETokenType` for simple tokens, kind tags for
  identifier/literal/ST_RSHIFT_1/ST_RSHIFT_2/ST_EOF, spelling, a name
  category bitmask (letters C/T/Y/E/N → class/template/typedef/enum/
  namespace-name), and `is_empty_string` (spelling `""`), `is_zero`
  (spelling `0`), `is_final`/`is_override` flags. A collector implementing
  `IPostTokenOutputStream` builds `vector<Pa6Token>`: `emit_simple` splits
  OP_RSHIFT into ST_RSHIFT_1 + ST_RSHIFT_2, `emit_invalid` throws,
  `emit_eof` appends ST_EOF. Name classification is a single helper so
  later PAs can swap real name lookup for the mock mask.
- `dev/src/parser/recog_parser.h/.cpp` (new) — recursive-descent
  recognizer `Pa6Parser` over `vector<Pa6Token>`: one `parse_foo` per
  nonterminal, bool result; success advances `pos_`, failure restores it.
  Recognition-only for PA6 (no AST; graded artifact is OK/BAD), but keep
  one-function-per-nonterminal boundaries so pa7+ can return typed nodes.
- `dev/recog.cpp` — thin adapter: argv, build-info snapshot (as
  `dev/preproc.cpp`), per-srcfile engine run → collector → parser → OK/BAD
  line, per-file `catch (exception&)` → BAD, outer envelope, EXIT_SUCCESS.
- `dev/frontend_source_sets.mk` — recog gains parser + full preproc set.

Core semantic rules the grammar alone does not express:

1. decl-specifier-seq type commitment: while parsing `decl-specifier+`, a
   type-name (category identifier or simple-template-id) is accepted as a
   type-specifier iff no previous type-specifier other than cv-qualifiers
   was seen. Thread a `seen_type` flag as a parameter (keeps memo sound).
2. close-angle-bracket (14.2.3): maintain a bracket-context stack
   (`() [] {} <>`). While the innermost open bracket is `<`, refuse to
   match OP_GT / ST_RSHIFT_1 / ST_RSHIFT_2 as relational/shift operators;
   they are reserved for `close-angle-bracket`. `(`/`[`/`{` push a
   non-angle context that re-enables them. Each ST_RSHIFT half closes one
   angle level (`TC1<TC2<6>>` — halves close inner then outer).
3. template-name angle commitment: when an identifier with the T category
   (or an operator/literal-operator-id in template-id position) is
   directly followed by OP_LT, commit to template-argument-list; on
   failure do NOT fall back to `<` as an operator. Proof fixture:
   `course/pa6/500-template-name-angle-commit-bad.t` (`int x = T1 < 2;` →
   BAD).
4. 6.8 statement disambiguation: in `parse_statement`, try
   declaration-statement before expression-statement (after labeled/
   keyword-led alternatives); expression wins only if declaration fails.
5. 8.2 declarator disambiguation: in noptr-declarator-suffix and
   parameter parsing, try `( parameter-declaration-clause )` before
   treating `(...)` as an initializer; type-id before expression in
   template-argument, then id-expression (ordered alternatives).
6. Greedy `foo*`/`foo+` matching, no shorter-sequence retry (README
   design notes); factor shared `attribute-specifier*` prefixes once.

## Failure Map

Single root cause: `DoRecog` is the handout stub throwing
`NotImplementedException` → all 47 pa6 tests fail with
EXIT_NOT_IMPLEMENTED (32 in `pa6/tests/`, 15 in `pa6/course/pa6/`).
No prior-stage failures: through-pa5 is 0-fail; fileAudit passes.
Ownership of the fix: token acquisition seam (preproc_engine), terminal
layer + parser (new `dev/src/parser/`), tool envelope (recog.cpp).

Fixture groups: 1xx smoke (empty/main/bad-token); 12x–20x expressions
(primary, id-expression, lambda, postfix, unary, cast, pm, binops,
condexpr); 15x statements/attributes; 25x–30x declarations/declarators/
enum/members; 180/400/450 linkage/exceptions/dots/templates; 270 typeid;
5xx angle brackets (3 tests + 3 course); 600/700 ambiguity (6.8/8.2);
course `-bad` negatives (empty case expr, invalid token in balanced scan,
try without handler, ellipsis without comma, deep template failure).

## Performance Risks

- Exponential backtracking on nested template arguments:
  `course/pa6/500-deep-template-argument-failure-bad.t` nests `TC1<` ~96
  deep and fails; template-argument tries constant-expression / type-id /
  id-expression, each re-descending into the same nested
  simple-template-id → 3^depth without memoization. Mitigation: memo
  table keyed `(rule_id, pos, angle_refusal_flag)` → `{ok, end_pos}` via
  a common wrapper on the hot family (simple-template-id,
  template-argument-list, type-id, expression entry points; uniform
  application is acceptable). Bound: O(rules × tokens × 2). The angle
  flag MUST be in the key or the 5xx angle fixtures flip.
- Do not memoize context-parameterized rules (decl-specifier-seq family
  with `seen_type`) unless the flag is part of the key.
- Budget: fixtures are < 2KB; every test must finish far under the 10s
  runner cap — target < 100ms each; deep-template probe is the canary.
- File audit caps (`cppgm_file_audit.pl`): source ≤ 3000 lines, function
  ≤ 240 lines, duplicate-block detector — 197 parse functions will need
  the parser split across `dev/src/parser/` files (e.g. expressions vs
  declarations) and shared helpers for repeated shapes (bracketed lists,
  ordered alternatives).

## Checkpoint Ledger

- CP1 (ACTIVE) — token pipeline + parser core: engine seam, terminal
  layer, recog envelope, parser infra (backtracking, bracket/angle
  context, memo), expression + statement + simple-declaration spine
  (declarators, parameters, initializers, attributes, new/delete,
  function-definition). Progress proof: `make test-pa6` rises from 0/47
  with the 1xx/12x/13x/14x/15x/20x/5xx-angle/600/700 groups flipping
  (expect ≳ 20/47); `make test-report-through-pa5` stays 0-fail.
- CP2 — declaration breadth: class-specifier/members/base clauses, enum,
  namespace/using/linkage/asm/alias, template-declaration + explicit
  inst/spec, operator/conversion/literal-operator ids, exceptions,
  full lambda. Proof: remaining 25x/30x/18x/40x/45x/course groups flip;
  target ≥ 45/47, no regressions.
- CP3 — disambiguation and hardening: exact 6.8/8.2 and angle-commit
  behavior on all fixtures, deep-template perf probe in budget,
  differential probes vs `recog-ref-stdin --trace` on grey "ill-formed
  but syntactically valid" cases, file audit clean, final
  `make test-report-through-pa6` 0-fail. New fixtures only if a real gap
  is found (regenerate via documented ref-test targets only).

## Active Checkpoint — CP1: token pipeline + parser core

Build the full data path (source → PA5 preprocess → Pa6Token vector →
recognizer → OK/BAD envelope) and the expression/statement/
simple-declaration spine of the grammar, with stubs returning failure for
CP2 nonterminals (class/enum/namespace/template-declaration/operator-id
families). Every alternative referencing a stubbed rule must fail cleanly
through the normal backtracking path.

### Implementation Packet

Files/symbols to create or edit:
- `dev/src/preproc_engine.h/.cpp`: extract the per-srcfile state reset at
  the top of `PreprocEngine::ProcessSourceFile(const string&)`
  (preproc_engine.cpp:200) into a private `ResetSourceFileState()`; add
  public `RunSingleFile(const std::string& srcfile,
  IPostTokenOutputStream& sink)` = reset + local `PostTokenStream(sink)`
  + existing private `ProcessSourceFile(srcfile, PostTokenStream&)` +
  `emit_eof()`. Re-express the one-arg overload on top of it with the
  existing `PreprocPostTokenOutputStream(output_)`. PA5 text output must
  stay byte-identical.
- `dev/src/parser/recog_token.h/.cpp` (new): `Pa6Token`,
  `Pa6TokenCollector : IPostTokenOutputStream` (all emit_* overrides;
  OP_RSHIFT → ST_RSHIFT_1+ST_RSHIFT_2; `emit_invalid` throws
  `Pa6LexError : std::runtime_error`; `emit_eof` appends ST_EOF), and
  `NameCategoryMask(spelling)` (bits for letters C/T/Y/E/N; also
  final/override/emptystr/zero flags at construction).
- `dev/src/parser/recog_parser.h/.cpp` (new): `Pa6Parser` with
  `bool ParseTranslationUnit()`; members `pos_`, bracket-context stack,
  memo `unordered_map<uint64_t, MemoEntry{bool ok; size_t end;}>` keyed
  `(rule_id, pos, angle_refusal)`; `parse_*` per pa6.gram nonterminal for
  the CP1 spine: translation-unit, declaration (simple/empty/attribute +
  function-definition only), primary/id-expression, unqualified/
  qualified-id, nested-name-specifier(+root/suffix), simple-template-id,
  close-angle-bracket, postfix(+root/suffix), unary, new/delete/noexcept,
  cast, pm → conditional chain, assignment, expression,
  constant-expression, all statements, condition, for-init/range,
  decl-specifier(+seq with threaded `seen_type`), storage-class/function-
  specifier, type-specifier/trailing-type-specifier(+seqs),
  simple-type-specifier, decltype-specifier, type-name, {class,enum,
  typedef,template,namespace}-name via mask, attribute-specifier family +
  balanced-token, alignment-specifier, init-declarator(-list),
  declarator/ptr/noptr(+root/suffix), parameters-and-qualifiers,
  trailing-return-type, ptr-operator, cv/ref-qualifier, declarator-id,
  type-id, abstract/pack declarators, parameter-declaration(-clause/
  -list), function-definition/body, initializer family, braced-init-list,
  expression-list, pseudo-destructor-name, lambda family, template-id/
  template-argument(-list/-dots), typename-specifier, static_assert-,
  alias-, opaque-enum-declaration headers as needed to fail cleanly.
  Implement semantic rules 1–6 from Stage Design (angle commitment via
  the T-category check before OP_LT).
- `dev/recog.cpp`: replace `DoRecog`; keep `PA6_Is*Name` semantics by
  delegating to `NameCategoryMask`; per srcfile: fresh
  `PreprocEngine(dummy_ostream, PA5GetFileId-equivalent, build_info)` —
  copy the `PA5GetFileId` + build-stamp code shape from `dev/preproc.cpp`
  — then `RunSingleFile` into a `Pa6TokenCollector`, `Pa6Parser` over the
  tokens, write `OK`/`BAD`; `catch (const exception&)` → BAD; keep the
  `--batch-stdin` guard as-is (harness never passes it); EXIT_SUCCESS.
- `dev/frontend_source_sets.mk`: `FRONTEND_OBJ_BASENAMES_recog :=
  parser/recog_parser parser/recog_token preproc_engine macro_replace
  ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode`.

Fixture groups targeted (all under `pa6/`, run from `pa6/` so paths in
outfiles match refs): tests/100-empty, 100-main, 101-bad (`@` → lex error
→ BAD), 120-primary, 121-id-expression, 130-postfix(+suffix), 131-unary,
140-cast, 150-{assignment,goto,jump-statement,pm-expression,statement,
attributes}, 200-binops, 201-condexpr, 400-dots,
500-closing-angle-bracket(+bad1,bad2), 600-ambig-68, 700-ambig-82;
course/pa6/150-bare-label-statement, 300-empty-case-expression-bad,
300-invalid-token-balanced-scan-bad, 400-ellipsis-without-comma,
500-template-name-angle-commit-bad, 500-deep-template-argument-failure-bad.
Lambda (122/300) and class/enum/template groups may stay failing until
CP2 — acceptable only if the CP1 groups flip.

Required spec facts (self-contained; grammar text is `pa6/pa6.gram`,
per-nonterminal FIRST/FOLLOW in `pa6/grammar/*.html`):
- Mock name lookup: identifier containing C/T/Y/E/N ⇒ class/template/
  typedef/enum/namespace-name; categories overlap; `TC1<...>` is a
  simple-template-id that matches class-name whole.
- Special tokens: TT_LITERAL = every literal kind incl. user-defined;
  ST_EMPTYSTR = literal spelled `""`; ST_ZERO = literal spelled `0`
  (both still TT_LITERAL); ST_OVERRIDE/ST_FINAL = identifiers spelled
  `override`/`final` (still TT_IDENTIFIER; only special where the grammar
  names them); ST_NONPAREN = any token except the six bracket tokens and
  ST_EOF; OP_RSHIFT never reaches the parser (always split).
- `decl-specifier-seq` must be present where used (constructors/
  destructors/conversion functions do not parse in PA6 — expected).
- Angle examples that must hold: `TC1< 1>2 > x1;` BAD; `TC1<(1>2)> x2;`
  OK; `TC1<TC2<1>> x3;` OK; `TC1<TC2<6>>1>> x4;` BAD;
  `TC1<TC2<(6>>1)>> x5;` OK; `int x = T1 < 2;` BAD (commitment).
- Outfile format: `recog <N>\n` then `<srcfile> <OK|BAD>\n` per file,
  byte-exact vs `.ref`; exit status EXIT_SUCCESS even for BAD files.

Commands:
- Build + focused: `make test-pa6` (root). Single fixture:
  `cd pa6 && ../dev/recog -o /tmp/o.txt tests/120-primary.t && diff /tmp/o.txt tests/120-primary.ref`
- Ref observation (never an implementation input):
  `cd pa6 && ./recog-ref-stdin < tests/600-ambig-68.t` (trace enabled).
- Broad gate: `make test-report-through-pa6`; prior gate stays clean:
  `make test-report-through-pa5`.
- Audit: `perl scripts/cppgm_file_audit.pl --stage pa6 --paths dev/src`.
- Performance probe:
  `cd pa6 && time ../dev/recog -o /tmp/p.txt course/pa6/500-deep-template-argument-failure-bad.t`
  — must print BAD and finish < 1s (memo working); without memo this
  hangs ~3^96.

Known uncertainties:
- Whether statement-level "declaration first, expression on failure"
  fully matches the ref on all 6.8 rows (`C(a)->m=7;` `C(a)++;`
  `C(a,5)<<c;` must fall back to expression) — verify with the 600
  fixture; refine ordering only if it fails.
- operator-function-id followed by `<` (`operator+<x>;` in
  500-operator-template-angle-boundary, a CP2 fixture): commit vs
  backtrack both pass that fixture; decide at CP2 via ref trace.
- Exact BAD boundary for grey ill-formed-but-parsable inputs: default to
  accepting (README guidance), match ref only where a fixture demands.
- Memo growth if applied uniformly (197 rules × tokens): fixtures are
  tiny; revisit only if the probe or audit flags it.
