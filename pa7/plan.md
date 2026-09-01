# PA7 Plan — nsdecl (phases 1–7 + namespace/declaration semantics) [COMPLETE]

Final state: 354/354 fixtures pass through `make test-report-through-pa7`
(313 through-pa6 + 25 pa7 local + 16 pa7 course); pa7 file audit passes.
Architecture review, findings, performance evidence, differential-probe
results, and the divergence envelope are consolidated in `pa7/audit.md`.

`nsdecl -o <outfile> <srcfiles...>` preprocesses each srcfile with the
PA5 pipeline, tokenizes, parses each TU against `pa7.gram` (35
nonterminals — declarations only), builds a namespace/entity/type model
with real name lookup, and dumps the model per the README format. Exit
status always graded; outfile compared byte-wise only when the ref exits
EXIT_SUCCESS; stdout/stderr never compared. Runner timeout 10 s/test.

## Stage Design (as built)

Owning boundaries (the semantic model started here is the seed for pa8+;
the pa6 recognizer is untouched in behavior):

- `dev/src/preproc_engine.h/.cpp` — reused unchanged; `RunSingleFile`
  feeds a `Pa6TokenCollector`.
- `dev/src/parser/recog_token.h/.cpp` — additive extension: `Pa6Token`
  carries the scalar literal payload (`lit_scalar`, `EFundamentalType`
  `lit_type`, value bytes as u64 `lit_value`) filled only in
  `emit_literal`; default-inert, recog behavior unchanged.
- `dev/src/parser/nsdecl_model.h/.cpp` — immutable canonical `Pa7Type`
  nodes behind factories that enforce canonicalization at build time (cv
  re-roots into arrays, cv on reference drops, ref-to-ref collapses);
  `DescribeType` prints the README grammar; entities
  `Pa7Namespace/Pa7Variable/Pa7Function/Pa7Typedef` with ordered member
  vectors (print authority) + name→`Pa7Decl` map (lookup authority);
  namespaces owned solely by the parent's vectors, aliases/using imports
  are origin-tagged map references; `FindDirectOrInline` implements the
  3.4.3.2p2 first stage (namespace ∪ transitive inline set); output
  walker prints variables, functions, namespaces in first-declaration
  order.
- `dev/src/parser/nsdecl_parser.h/.cpp` — single-pass recursive descent
  with semantic actions into the model (no AST). Typedef/namespace
  lookup happens during the parse. One declarator engine
  (Named|Abstract|Either): `ParseDeclaratorChain` records prefix
  ptr-operators, an optional grouped child, and suffixes per level;
  folding `ops ++ reverse(suffixes) ++ child` over the decl-specifier
  type applies 8.3 binding inside-out (no post-hoc type surgery).
  Lookup: per-scope two-stage (direct+inline set, then live transitive
  using-directives, DFS with visited set) applied up the scope chain;
  qualified declarator-ids match direct+inline only. 8.2p7 paren
  disambiguation scans a full `::?`-qualified name against the same
  typedef lookup the parse uses.
- `dev/nsdecl.cpp` — envelope: argv `-o` contract, batch-stdin
  not-implemented loop, fresh engine+collector+global+parser per
  srcfile, all TUs analyzed before writing, exception → EXIT_FAILURE.
- `dev/frontend_source_sets.mk` — nsdecl links the parser pair +
  recog_token + full preproc set.

Fixture-pinned semantic rules (beyond the grammar):

1. Table 10 mapping with PA2 canonical names; default base int;
   order-free combinations (130).
2. decl-specifier-seq stop rule 7.1.6.2p2: after a non-cv
   type-specifier, an identifier is the declarator.
3. Typedefs resolve immediately; the model stores only canonical types
   (150, 340, 360, 370); cv via array typedef re-roots (340), cv via
   reference typedef drops, `&` ⊕ `&&` = `&` (370).
4. Parameter adjustment 8.3.5p5: array→pointer, function→pointer,
   top-level cv dropped (350); `(void)` — also via qualified typedef —
   → empty list (course 360); `...` with/without comma (310).
5. Array bound 8.3.4: literal payload as size_t; any integral/character
   literal form (course 320-literal-forms); value > 0; redeclaration
   completes an unknown bound, entity prints once at first-declaration
   position with the final type (320, 330).
6. Entity kind from the canonical type: a function-typedef declares a
   function (360); typedefs/aliases/using-decls never print (150, 260,
   270).
7. 8.2p7: `(` opens parameters iff the contents can start a
   parameter-declaration-clause — `)`, `...`, specifier keyword, or a
   (qualified) name resolving to a typedef (course 350).
8. Namespace-name contexts consider only namespaces (course
   220-shadowing); directives are live and transitive (course
   200-transitive-extension); reference semantics attach directive names
   at the directive site (anchoring closed in the ref's favor —
   audit.md envelope).
9. Unnamed namespace: one per enclosing namespace, reopened by later
   blocks, implicit directive in the parent (240, 120); may be inline.
10. Inline namespaces: implicit parent directive for unqualified lookup;
    qualified lookup searches N ∪ transitive inline set first (course
    280-qualified-lookup, probe-pinned priority over explicit
    directives); reopening non-inline as inline fails (course
    280-reopen-bad); reopening inline without `inline` is legal and
    stays inline (probe-pinned).
11. Qualified declarator-id: resolve the NNS, match by direct+inline
    members only, merge types; a miss declares a fresh member of the
    resolved namespace (250, course 320-completion, probe-pinned).
    Duplicate using-declarations and duplicate aliases are no-ops (260,
    290, course 270-reuse).

## Checkpoint Ledger

- CP1 (DONE) — semantic core: model + types + printer + parser +
  envelope; named namespaces, scope-chain lookup, qualified
  declarator-id merge, full declarator machinery. Evidence:
  `make test-pa7` 36/41; through-pa6 313/313.
- CP2 (DONE) — namespace semantics breadth: unnamed/inline namespaces,
  aliases, using-declarations, anchored-at-site transitive directives.
  Evidence: 41/41; through-pa7 354/354; 200-chain probe 0.012 s.
- CP3 (DONE) — audit + hardening: five material conformance fixes
  (inline-set priority, one-directional reopen, declarator-id matching,
  grouped-declarator composition, qualified 8.2p7 lookahead), bound
  validation, dead-state/leniency cleanups; ~30 differential probes with
  recorded divergence envelope; linear 20k→40k scaling evidence.
  Consolidated in `pa7/audit.md`.
