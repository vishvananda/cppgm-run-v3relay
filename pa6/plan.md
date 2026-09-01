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

- CP1 (COMPLETE) — token pipeline + parser core: engine seam, terminal
  layer, recog envelope, parser infra (backtracking, bracket/angle
  context, memo), expression + statement + simple-declaration spine
  (declarators, parameters, initializers, attributes, new/delete,
  function-definition). Evidence: final `make test-pa6` is 34/47 (13
  deferred CP2 failures) from 0/47 at checkpoint start; all named CP1
  groups pass, the deep-template probe is 0.009s, prior-through-pa5 is
  260/260, and the pa6 source audit passes.
- CP2 (ACTIVE) — declaration breadth: class-specifier/members/base clauses, enum,
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

## Active Checkpoint — CP2: declaration breadth

Extend the completed token/parser path through class and enum declarations,
members and bases, namespaces and using/linkage forms, templates,
operator/conversion ids, exceptions, and lambdas. Preserve CP1’s ordinary
backtracking and angle commitment behavior while replacing only the explicit
CP2 stubs.

### Implementation Packet

Files/symbols to edit:
- `dev/src/parser/recog_parser.h/.cpp`: replace the explicit CP2 stubs for
  class/enum specifiers and members, namespace/using/linkage/asm/alias
  declarations, template declarations and explicit inst/spec, exception
  specifications, and lambda expressions. Reuse the CP1 token, bracket,
  memo, and backtracking helpers.
- `dev/recog.cpp`: keep the CP1 preprocessor/token/parser envelope and
  add no fixture-specific routing.

Proof targets: the remaining `180`, `250`, `270`, `300`, `400`, `450`,
`122`, and course member/operator-template groups flip without regressing
the 34 passing tests; then rerun `make test-report-through-pa6`, the file
audit, and the deep-template performance probe.
