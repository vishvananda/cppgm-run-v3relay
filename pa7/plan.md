# PA7 Plan — nsdecl (phases 1–7 + namespace/declaration semantics)

Target: clean `make test-report-through-pa7` = 313 prior fixtures + 41 pa7
(25 `pa7/tests` + 16 `pa7/course/pa7`). Contract: `nsdecl -o <outfile>
<srcfiles...>` preprocesses each srcfile with the PA5 pipeline, tokenizes,
parses each TU against `pa7.gram` (35 nonterminals — declarations only: no
expressions, classes, or templates), builds a namespace/entity/type model
with real name lookup, then dumps the model per the README format. Inputs
are well-formed except where a fixture pins failure. Grading: exit status
always compared; outfile compared byte-wise (trailing whitespace stripped)
only when the ref exit is EXIT_SUCCESS; stdout/stderr never compared (ref
stdouts are empty — keep diagnostics on stderr). Runner timeout 10 s/test.

## Stage Design

Owning boundaries (the semantic model started here is the seed for pa8+;
the pa6 recognizer stays recognition-only and untouched in behavior):

- `dev/src/preproc_engine.h/.cpp` — reused unchanged; `RunSingleFile`
  feeds a `Pa6TokenCollector`.
- `dev/src/parser/recog_token.h/.cpp` — additive extension only: retain
  the scalar literal payload (`EFundamentalType` + value bytes as u64) on
  `Pa6Token`, filled in `emit_literal`; pa7 array bounds need it. Fields
  default-inert; recog behavior is unchanged.
- `dev/src/parser/nsdecl_model.h/.cpp` — NEW. Immutable canonical `Pa7Type`
  nodes (fundamental / cv / pointer / lvalue-ref / rvalue-ref / array /
  function) behind factories that enforce canonicalization at build time:
  cv on array re-roots to the element type, cv on reference is dropped,
  `&`/`&&` over a reference collapses (8.3.2p1,p6). `DescribeType` prints
  the README recursive grammar. Entities: `Pa7Namespace` (parent, name,
  inline flag, unnamed-child slot, ordered member lists for
  variables/functions/child-namespaces, name→decl map, using-directive
  list), `Pa7Variable` (type mutable only for array-bound completion),
  `Pa7Function`, `Pa7Typedef`. Lookup lives here: unqualified (3.4.1p2 +
  7.3.4 anchored transitive directives), qualified (3.4.3.2: N ∪ inline
  set, then transitive directive union), both filter-parameterized
  (any / namespaces-only / types-only) with visited sets. Output walker:
  variables, then functions, then namespaces, each in first-declaration
  order; global and unnamed namespaces print `start unnamed namespace`.
- `dev/src/parser/nsdecl_parser.h/.cpp` — NEW. Single-pass recursive
  descent over `pa7.gram` with semantic actions directly into the model
  (no AST). Typedef/namespace lookup happens during the parse — required
  for `simple-type-specifier: nested-name-specifier? type-name` and for
  the 8.2p7 paren disambiguation in parameters. Declarator engine has one
  code path with mode Named|Abstract|Either; no named-vs-abstract
  backtracking.
- `dev/nsdecl.cpp` — envelope copied from `dev/recog.cpp` (argv `-o`
  parsing, PA5GetFileId, build-info snapshot, batch-stdin
  not-implemented loop). Fresh engine+collector+parser+global-namespace
  per srcfile; write outfile after analysis; any thrown error →
  EXIT_FAILURE for the whole run (no per-file OK/BAD in pa7).
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_nsdecl :=
  parser/nsdecl_parser parser/nsdecl_model parser/recog_token
  preproc_engine macro_replace ctrlexpr_eval pptoken_lexer
  posttoken_stream posttoken_tables unicode`.

Fixture-pinned semantic rules (beyond the grammar):

1. Table 10 mapping with PA2 canonical names (130): default base int;
   `signed`→int, `long`→long int, `double`+`long`→long double,
   `signed char` distinct from `char`; order-free combinations.
2. decl-specifier-seq stop rule (7.1.6.2p2): once a type-specifier other
   than cv is seen, a following identifier is the declarator.
3. Typedefs resolve to canonical types immediately; the model stores only
   canonical types (150, 340, 360, 370).
4. cv via typedef: array typedef + cv → cv element (340); reference
   typedef + cv → cv dropped (370); ref collapse & ⊕ && = & (370).
5. Parameter adjustment 8.3.5p5: array→pointer-to-element,
   function→pointer-to-function, top-level cv dropped (350, `int
   (*const volatile)(void)` → plain pointer). `(void)` (including via
   typedef, checked after resolution — course 360) → empty list. `...`
   with or without preceding comma → trailing `...` (310).
6. Array bound (8.3.4): TT_LITERAL payload converted to size_t; integer
   and character literals of any form (hex/octal/suffixed/u''/U''/L'' —
   course 320-literal-forms); value > 0 guaranteed. Redeclaration
   completes an unknown bound (320, 330, course 320-completion); entity
   prints once, at first-declaration position, with the final type.
7. Entity kind comes from the canonical type: `G g;` with G a function
   typedef declares a function (360). Only variables/functions print;
   typedefs, aliases, using-decls never do (150, 260, 270).
8. 8.2p7 paren disambiguation in parameters: `(` opens a
   parameters-and-qualifiers iff the next token can start a
   parameter-declaration-clause — `)`, `...`, storage-class/cv/typedef
   keyword, simple-type keyword, `::`, or identifier resolving to a
   typedef-name; otherwise it is a parenthesized declarator (course 350:
   `int (I (*)(I))` vs `int (x)`).
9. Namespace-name lookup contexts (NNS root, using-directive target,
   alias target) consider only namespaces — a variable of the same name
   does not shadow (course 220-shadowing).
10. Using-directives are live and transitive (nominate the namespace,
    not a snapshot — course 200-transitive-extension) and anchor names
    at the nearest enclosing namespace containing both the directive and
    the nominated namespace (course 200-anchor-nested/sibling).
11. Unnamed namespace: one per enclosing namespace, reopened by later
    blocks, implicit using-directive in the parent, found by qualified
    lookup through that directive (240, 120); may be inline (120).
12. Inline namespaces: implicit using-directive in the parent covers
    unqualified lookup (course 280-alias-lookup); qualified lookup
    searches N ∪ transitive inline set first (X::TZ two levels deep,
    course 280-qualified-lookup), and namespaces inside inline members
    are reachable qualified (I0::Deep). Reopening a non-inline namespace
    with `inline namespace` must exit EXIT_FAILURE (course
    280-reopen-bad — the only pinned failing fixture; outfile content is
    not compared for it).
13. Qualified declarator-id (`int V::i;`): resolve the NNS to a
    namespace, match the terminal name by qualified lookup (inline set
    included — `int X::deep;`), merge with the existing entity (250,
    course 320-completion). No lookup after the qualified id is required
    (README relaxation). Duplicate using-declarations and duplicate /
    self-referential namespace aliases are legal no-ops (260, 290,
    course 270-reuse).

## Failure Map

The CP1 baseline was 41 pa7 failures because the stub
`dev/nsdecl.cpp` threw before doing anything. CP1 now passes 36/41; the
remaining five are the deferred namespace-semantics group. No prior-stage
failures (through-pa6 is clean at 313). Ownership of the failures:

- Tool envelope: implemented; all 41 now reach the pa7 pipeline and output
  comparison.
- Type/declarator core (130, 140, 300, 310, 320, 330, 340, 350, 360,
  370; course 320-literal-forms, 320-parenthesized-array-abstract,
  350-parenthesized-parameter-declarators, 360-void-typedef,
  600-deep-parenthesized-declarator): no Type model, Table-10 mapping,
  declarator engine, adjustments, or collapse.
- Namespace structure + direct lookup (100-empty, 100-empty-decl, 110,
  150, 190, 230, 250; course 320-array-completion): no Namespace
  entities, scope-chain lookup, `::`-rooted NNS, qualified declarator-id
  matching, or printer.
- Using/alias/unnamed/inline machinery (120, 200, 220, 240, 260, 270,
  280, 290; course 200-anchor-nested, 200-anchor-sibling,
  200-transitive-extension, 220-shadowing, 270-reuse, 280-alias-lookup,
  280-qualified-lookup, 280-reopen-bad, 600-deep-using-directive-chain):
  no directives, using-declarations, aliases, unnamed/inline semantics.

## Performance Risks

- Deep parenthesized declarators (course 600-deep-parenthesized, ~1200
  levels): the single-pass Either-mode declarator and redundant-parenthesis
  fast path avoid exponential reparse and remain safe at the required 4000
  levels (measured at 0.011 s on the scaled probe).
- Transitive directive chains (course 600-deep-using-directive-chain,
  ~100 namespaces): per-lookup BFS with a visited set is O(V+E) and
  cycle-safe (mutual directives are legal); no memoization needed at
  fixture scale. Do not recompute closures eagerly per declaration.
- Types are shared immutable nodes; no deep copies; description recursion
  is bounded by type nesting (small in all fixtures).

## Checkpoint Ledger

- CP1 (DONE) — semantic core: model + types + printer + parser +
  envelope; named namespaces, scope-chain unqualified lookup, NNS/
  qualified lookup over direct declarations only, qualified
  declarator-id merge, typedef/alias-declaration, full declarator
  machinery. Evidence: focused packet fixtures 23/23; `make test-pa7`
  36/41 (failures 41 → 5); `make test-report-through-pa6` 313/313;
  file audit passes with only the pre-existing warning.
- CP2 (NEXT) — namespace semantics breadth: unnamed namespaces, inline
  namespaces + inline-set qualified lookup + reopen check, namespace
  aliases, using-declarations, anchored transitive using-directives.
  Proof: 41/41 `make test-pa7`; clean `make test-report-through-pa7`.
- CP3 — audit + hardening: differential probes vs `pa7/nsdecl-ref` on
  hand inputs (lookup anchoring edges, UB envelope recorded, not
  chased), perf evidence on scaled probes, keep implementation bodies in
  .cpp (file-audit division warning hygiene), write `pa7/audit.md`,
  close out plan. Proof: through-pa7 clean; audit passes.

## Completed Checkpoint — CP1: semantic core

Build the model/parser/envelope so every declaration form works at global
scope and inside named namespaces, with lookup over direct declarations.
Using-directives/declarations, aliases, unnamed and inline namespace
semantics parse but may throw "unimplemented" (those fixtures already
fail; exit-status noise is acceptable until CP2). Reduction proof: the 23
fixtures in the envelope + type/declarator + structure groups flip to
passing; no through-pa6 regression.

### Implementation Packet

Files/symbols (new unless noted):

- `dev/src/parser/nsdecl_model.h/.cpp`: `Pa7Type` (kind enum + shared_ptr
  children; factories `MakeFundamental(EFundamentalType)`,
  `ApplyCV(cv, type)` — re-roots into arrays, no-op on references,
  `MakePointer`, `MakeReference(is_rvalue, inner)` — collapsing,
  `MakeArray(has_bound, bound, elem)`, `MakeFunction(params, varargs,
  ret)`); `DescribeType(type) -> string`; `IsVoid/IsFunction` helpers;
  `AdjustParameter(type)`; entities `Pa7Namespace/Pa7Variable/
  Pa7Function/Pa7Typedef` + tagged `Pa7Decl` map entry;
  `Pa7Namespace::FindDirect(name, filter)`; `PrintTranslationUnit(out,
  global)`.
- `dev/src/parser/nsdecl_parser.h/.cpp`: `Pa7Parser(tokens, global)`,
  `ParseTranslationUnit()`; internals: `ParseDeclaration` (dispatch:
  `;` empty; `namespace` → definition or alias by `identifier =`
  lookahead; `using` → directive (`using namespace`) / alias-decl
  (`identifier =`) / using-declaration; else simple-declaration);
  `ParseDeclSpecifierSeq -> {canonical base type, typedef flag}` with
  Table-10 counters and stop rule 2; `ParseDeclarator(mode)` returning
  declarator-id (possibly NNS-qualified) + type-transformer applied
  inside-out (suffixes bind before prefix ptr-operators; parens group);
  `ParseParametersAndQualifiers` (rule 8 lookahead, rule 5 adjustments);
  array bounds via token literal payload (rule 6); declaration commit:
  typedef-register / variable-or-function create-or-merge (kind by
  canonical type, rule 7), qualified-id path resolves NNS then merges
  (rule 13, direct declarations only in CP1). Scope state: stack of
  `Pa7Namespace*`; named namespace definition finds-or-creates child
  (reopen extends; member-list append on first creation only).
- `dev/src/parser/recog_token.h/.cpp` (edit): add `bool lit_scalar;
  EFundamentalType lit_type; unsigned long long lit_value;` to
  `Pa6Token`, populated only in `Pa6TokenCollector::emit_literal` by
  memcpy of min(nbytes,8) bytes (values are unsigned-representable;
  bounds are guaranteed positive). All other emit paths leave
  `lit_scalar=false`.
- `dev/nsdecl.cpp` (rewrite stub): copy the `dev/recog.cpp` envelope
  (PA5GetFileId syscall wrapper, build-info stamp, batch-stdin
  not-implemented loop, `-o` argv contract). Per srcfile: fresh
  `PreprocEngine` + `Pa6TokenCollector` + fresh global `Pa7Namespace` +
  `Pa7Parser`; collect all TU models, then write `<n> translation
  units` and each `start translation unit <srcfile>` / model dump /
  `end translation unit`. Any exception → stderr message, EXIT_FAILURE.
  Keep stdout empty.
- `dev/frontend_source_sets.mk` (edit): set nsdecl list as in Stage
  Design.

Fixture groups for this checkpoint: `pa7/tests/{100-empty,
100-empty-decl, 110-namespace, 130-simple-type-specifiers,
140-declarators, 150-lookup, 190-namespace, 230-outer-inner, 250-outside-def,
300-fn-void, 310-varargs, 320-arrays, 330-multid-array, 340-array-const,
350-function-adjust, 360-function-typedef, 370-ref-collapse}` and
`pa7/course/pa7/{320-array-bound-literal-forms, 320-array-completion,
320-parenthesized-array-abstract, 350-parenthesized-parameter-declarators,
360-function-void-typedef, 600-deep-parenthesized-declarator}`.

Required spec facts: rules 1–8 and 13 above are the complete list for
CP1; the README output grammar is reproduced in Stage Design and the
canonical fundamental-type spellings are pinned by
`tests/130-simple-type-specifiers.ref`. Global namespace prints
`start unnamed namespace`; empty namespaces still print start/end;
`inline namespace` flag line is printed right after `start` (parse and
record the flag now; semantics wait for CP2).

Commands:

- Build + focused: root `make test-pa7` (builds dev/nsdecl and runs the
  41 fixtures with comparison).
- Single test byte-diff: `cd pa7 && ../dev/nsdecl -o /tmp/o tests/150-lookup.t;
  diff /tmp/o tests/150-lookup.ref` (srcfile argument must be the
  ref-matching relative path, e.g. `tests/150-lookup.t`).
- Reference observation on hand inputs: `cd pa7 && ./nsdecl-ref -o /tmp/r
  <file>` (observation only; fixtures gate).
- Broad (exit criterion): `make test-report-through-pa7`; prior-stage
  guard `make test-report-through-pa6` (313, must stay clean).
- Audit: `perl scripts/cppgm_file_audit.pl --stage pa7 --paths dev/src`
  (currently passes with one pre-existing recog_parser.h warning; keep
  new implementation bodies in .cpp files).

Performance probe: `cd pa7 && time ../dev/nsdecl -o /tmp/o
course/pa7/600-deep-parenthesized-declarator.t` (expect ≪1 s) and a
scaled input `perl -e 'print "int ", "("x4000, "x", ")"x4000, ";"' >
/tmp/deep.t; time ../dev/nsdecl -o /tmp/o /tmp/deep.t` — time must scale
linearly with depth and not overflow the stack at 4000 levels.

Known uncertainties:

- 7.3.4 anchoring edge cases are only partially fixture-pinned; CP2 must
  implement the anchored algorithm (rule 10) rather than naive
  attach-at-directive-site; validate extra cases against nsdecl-ref
  during CP2/CP3.
- Only inline-reopen failure behavior is pinned; other ill-formed inputs
  are UB — any exception → EXIT_FAILURE is sufficient; do not chase ref
  parity on UB inputs.
- Storage-class specifiers have no output effect; parse and discard
  (`static`/`extern` fixtures pin that extern-only declarations still
  print).
- All fixtures are single-srcfile; keep the multi-file loop per README
  but it is untested.
- `Pa6Token` field additions must stay inert for recog (no behavior or
  layout assumptions elsewhere — verified: through-pa6 green is the
  guard).

## Active Checkpoint — CP2: namespace semantics breadth

Implement the deferred namespace model semantics while preserving the CP1
direct-declaration and type/declarator behavior: unnamed namespace reuse and
implicit visibility, inline namespace visibility and qualified lookup, live
anchored using-directives, namespace aliases, and using-declarations.

### Implementation Packet

- `dev/src/parser/nsdecl_model.h/.cpp`: make namespace lookup filter-aware
  for direct, unqualified, and qualified paths; retain using-directive edges
  with visited sets and anchored search; model unnamed/inline namespaces and
  aliases without adding aliases to output member lists; import using-
  declarations as visibility-only entries.
- `dev/src/parser/nsdecl_parser.h/.cpp`: resolve namespace-only NNS and
  alias/directive targets, enforce inline-reopen consistency, create/reopen
  the one unnamed child per enclosing namespace, and commit using forms
  without changing CP1 declaration ownership.
- Fixtures: `pa7/tests/{120-namespace-special,200-using-directives,
  220-namespace-alias,240-unnamed,260-double-alias,270-using-declaration,
  280-inline-namespace,290-double-using}` plus
  `pa7/course/pa7/{200-using-directive-anchor-nested,
  200-using-directive-anchor-sibling,200-using-directive-transitive-extension,
  220-namespace-name-lookup-shadowing,270-using-declaration-reuse,
  280-inline-namespace-alias-lookup,280-inline-namespace-qualified-lookup,
  280-inline-namespace-reopen-bad,600-deep-using-directive-chain}`.
- Proof: current CP1 36/41 remains green, the five deferred failures are
  reduced to zero, then `make test-report-through-pa7` and the pa7 file audit
  pass.
