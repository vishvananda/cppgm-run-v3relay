# PA6 Plan — recog (syntactic recognition of translation-unit) [COMPLETE]

Final state: 313/313 fixtures pass through `make test-report-through-pa6`
(260 through-pa5 + 38 pa6 local including six fixtures added at CP3 +
15 pa6 course); pa6 file audit passes. Architecture review, findings,
performance evidence, differential-probe results, and the divergence
envelope are consolidated in `pa6/audit.md`.

`recog -o <outfile> <srcfiles...>` preprocesses each srcfile with the PA5
pipeline, tokenizes, and recognizes the token sequence against `pa6.gram`
(197 nonterminals). Outfile: `recog <N>` then one `<srcfile> OK|BAD` line
per srcfile; any per-file failure → BAD for that file only; tool exits
EXIT_SUCCESS. Only exit status + outfile are graded; per-test runner
timeout 10 s.

## Stage Design (as built)

Owning boundaries (spec.md §1: the PA6 parser is the seed of the
production parser; later stages extend it):

- `dev/src/preproc_engine.h/.cpp` — token production.
  `RunSingleFile(srcfile, sink)` feeds a caller-supplied
  `IPostTokenOutputStream`, sharing per-srcfile reset and the token
  pipeline with the PA5 text writer (whose output stays byte-identical).
- `dev/src/parser/recog_token.h/.cpp` — PA6 terminal layer. `Pa6Token`
  is classified once at construction: kind (simple/identifier/literal/
  rshift-half/eof), `ETokenType`, and a flags bitmask holding the mock
  name-lookup categories (C/T/Y/E/N via `NameCategoryMask`, the single
  classification authority), final/override, and the empty-string/zero
  literal facts. The collector splits OP_RSHIFT into ST_RSHIFT_1 +
  ST_RSHIFT_2, throws on invalid posttokens, and appends ST_EOF.
- `dev/src/parser/recog_parser.h/.cpp/_cp2.cpp` — recursive-descent
  recognizer, one `parse_foo` per nonterminal returning bool; success
  advances `pos_`, failure restores position + bracket depth.
  Recognition-only for PA6, but per-nonterminal boundaries are kept so
  pa7+ can return typed nodes.
- `dev/recog.cpp` — thin adapter: argv, build-info snapshot, fresh
  engine + collector + parser per srcfile, per-file catch → BAD.
- `dev/frontend_source_sets.mk` — recog links parser + full preproc set.

Semantic rules beyond the grammar (all reference/fixture-pinned):

1. decl-specifier-seq type commitment (7.1.6.2p2): once a non-cv
   type-specifier is seen, an identifier/`::` ends the seq (it can only
   be the declarator). Enforced inside the memoized seq parse.
2. close-angle-bracket (14.2.3): one bracket stack; `<` pushes an angle
   context only at committed template points; while the innermost
   bracket is an angle, `>`/`>>`-halves are reserved for
   close-angle-bracket; each ST_RSHIFT half closes one level.
3. Template angle commitment: a T-category name (or operator/literal-id)
   directly followed by `<` commits to a template-argument-list; failure
   is a hard failure with no operator-`<` fallback (course 500 fixture).
4. 6.8: statements try declaration-statement before expression-statement.
5. 8.2: parameters before initializers, type-id before expression in
   template arguments and sizeof/alignof.
6. Greedy `foo*`/`foo+` matching, no shorter-sequence retry; a
   ptr-operator-free noptr-declarator followed by `->` takes a trailing
   return type (no other continuation is grammatical).

Performance design: memo keyed `(rule_id, pos, angle_refusal)` over six
net-zero-bracket rules — simple-template-id, expression,
constant-expression, type-id, template-argument (bounds the 3^depth
deep-template backtracking) and decl-specifier-seq (bounds the 2^depth
function-definition→simple-declaration re-parse of nested class bodies).
Key soundness rests on deterministic angle pushes (rule 3); the
angle-refusal bit covers template-argument extent. Evidence and scaling
measurements in `pa6/audit.md`.

## Checkpoint Ledger

- CP1 (COMPLETE) — token pipeline + parser core: engine seam, terminal
  layer, recog envelope, parser infra (backtracking, bracket/angle
  context, memo), expression + statement + simple-declaration spine.
  Evidence: 34/47 pa6 from 0/47; through-pa5 260/260; deep-template
  probe 0.009 s; pa6 source audit passes.
- CP2 (COMPLETE) — declaration breadth: class/members/base clauses,
  enum, namespace/using/linkage/asm/alias, template declarations and
  explicit inst/spec, operator/conversion/literal-operator ids,
  exceptions, full lambda. Evidence: 47/47 pa6; through-pa5 260/260;
  deep-template 0.07 s; audit passes.
- CP3 (COMPLETE) — final architecture audit and hardening: implemented
  the missing decl-specifier commitment rule, block-declaration
  statements (+ namespace-alias extraction, two restore bugs), trailing
  return types, statement attributes, noptr-new-declarator; memoized
  decl-specifier-seq to kill the exponential nested-class re-parse;
  moved name classification into token construction; deduplicated memo
  ids and removed dead code. Evidence: 313/313 with six new fixtures;
  ~150 differential probes clean modulo the recorded leniency envelope;
  linear scaling on 20k-line probes; audit passes.
