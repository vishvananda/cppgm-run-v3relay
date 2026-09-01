# PA7 Final Architecture Audit — nsdecl

## Scope and method

Traced representative facts through their full ownership paths: srcfile →
PA5 `PreprocEngine::RunSingleFile` → `Pa6TokenCollector` (typed
`Pa6Token`; scalar-literal payload `lit_scalar/lit_type/lit_value` filled
only in `emit_literal`, inert for recog) → `Pa7Parser` single-pass
recursive descent with semantic actions directly into the
`Pa7Namespace`/`Pa7Decl` model (no AST) → `PrintTranslationUnit` walking
member vectors in first-declaration order → outfile via the `dev/nsdecl.cpp`
envelope. Measured scaling on 20k/40k-declaration, 2000-namespace
directive-chain, 1000-deep inline-nesting, and 3000–4000-deep
parenthesized-declarator probes; ran ~30 directed differential probes
against `nsdecl-ref` (outfile bytes + exit status) targeting the plan's
recorded uncertainties: lookup anchoring, inline-set priority, qualified
declarator matching, and grouped-declarator composition.

## Ownership trace (clean)

- **Type facts**: `Pa7Type` nodes are immutable and shared; the factories
  are the single canonicalization authority — `ApplyCV` re-roots into
  arrays and is a no-op on references (8.3.2p1), `MakeReference`
  collapses ref-to-ref with `&& ⊕ && = &&`, else `&` (8.3.2p6). Typedefs
  resolve to canonical types at declaration commit, so the model never
  stores a spelled alias. `DescribeType` renders the README grammar;
  entity kind (variable vs function) is derived from the canonical type
  at commit, never stored separately.
- **Declarator facts**: one engine parses named/abstract/either
  declarators. `ParseDeclaratorChain` records each level as prefix
  ptr-operators, an optional grouped child, and suffixes; the fold order
  `ops ++ reverse(suffixes) ++ child` applies the 8.3 binding rules
  inside-out over the decl-specifier type. There is no post-hoc type
  surgery: the former `Insert*Suffix` walkers (which located the
  insertion point by inspecting the built type) are gone. The 8.2p7
  parameter-vs-parenthesized-declarator choice is a pure lookahead
  (`IsParameterStart`/`QualifiedTypeNameStartsAt`) that scans a full
  optionally-`::`-rooted qualified name and asks the same typedef lookup
  the parse itself uses, so disambiguation and parse cannot disagree.
- **Name facts**: `Pa7Namespace` owns members (ordered vectors for
  printing + one name→`Pa7Decl` map for lookup); namespaces are owned
  exclusively by the parent's vectors and `Pa7Decl` references them by
  plain pointer (aliases and using imports add map entries only, tagged
  by origin, and never print). Lookup is two-stage everywhere:
  `FindDirectOrInline` (the namespace and its transitive inline set,
  3.4.3.2p2 first stage) then live transitive using-directives (DFS with
  a visited set, cycle-safe, each nominated namespace searched
  direct+inline before its own directives). Unqualified lookup runs that
  per scope up the chain; qualified declarator-ids match by
  `FindDirectOrInline` only (no directive contribution — reference
  behavior, probe p8) and otherwise declare a fresh entity in the
  resolved namespace. Directives are recorded at the nominating
  namespace and consulted at lookup time, so later namespace extensions
  are visible (live, pinned by course 200-transitive-extension).
- **Envelope**: `dev/nsdecl.cpp` is a thin adapter — argv `-o` contract,
  batch-stdin not-implemented loop, fresh engine + collector + global +
  parser per srcfile (no cross-file state), all TUs analyzed before the
  outfile is written, any exception → EXIT_FAILURE, stdout empty.

## Findings and changes (this cleanup)

Conformance findings were surfaced by directed differential probes
against `nsdecl-ref` on the plan's recorded uncertainty list.

1. **Qualified lookup ignored inline-set priority (material, fixed)**:
   3.4.3.2p2 searches N ∪ inline set before directive-nominated
   namespaces, but the flat directive walk found an earlier explicit
   `using namespace M;` before a later inline member — `N::T` returned
   M's typedef where the reference returns the inline member's
   (probe p2: `int` vs ref `double`). `FindDirectOrInline` now runs as a
   first stage at every lookup site, including per nominated namespace
   inside the directive DFS.
2. **Inline reopen rejected in both directions (material, fixed)**:
   reopening an inline namespace (named or unnamed) without the `inline`
   keyword threw, but 7.3.1 permits it (the namespace stays inline) and
   the reference accepts it (probes p3/p4). The check is now
   one-directional; the pinned failing fixture (non-inline reopened *as*
   inline, course 280-reopen-bad) still throws.
3. **Qualified declarator-id matched through using-directives (material,
   fixed)**: `int V::i;` with `i` reachable only via V's using-directive
   merged into the other namespace's entity; the reference declares a
   fresh `V::i` (probe p8). Declarator matching now uses
   `FindDirectOrInline` only — inline-set merges (course
   280-qualified-lookup `int X::deep;`) still work.
4. **Grouped-declarator suffix insertion wrong (material, fixed)**: the
   `Insert*Suffix` helpers walked the built type to its innermost node,
   so any declarator with ptr-operators both inside and outside a group
   composed wrong: `int (*(*x)[5])(void)` printed "pointer to pointer to
   array of 5 function..." instead of "pointer to array of 5 pointer to
   function..." (probe b10), and even ungrouped `int *x[5][7]` put the
   pointer between the array dimensions (probe b15). Replaced by the
   wrapper-chain fold above; both probes now byte-match the reference.
5. **8.2p7 lookahead was single-identifier (material, fixed)**: `(` was
   taken as a grouped declarator whenever the next identifier alone was
   not a typedef, so a qualified type name misparsed:
   `int f(N::T (N::T));` dropped the parameter's function type
   (probe b11). `QualifiedTypeNameStartsAt` scans `::? id (:: id)*` and
   resolves the whole path; `(N::x)` with x a variable still parses as a
   parenthesized declarator (probe b16).
6. **Array-bound validation (hardening)**: the retained literal payload
   type was never read; 8.3.4 defines only integral bounds with value
   > 0, so the bound parse now rejects non-integral literal types and
   zero instead of reinterpreting payload bytes as a size.
7. **Structure cleanups**: removed dead `Pa7Namespace::alias_target`,
   dead `FindNamespaceDirect`, the undefined `ParseArraySuffix`
   declaration, and the always-true alias-origin test on the declarator
   merge path; removed the initializer-skip loop and its
   `IsDeclarationBoundary` helper (pa7.gram has no initializers);
   narrowed storage-class acceptance to the grammar's
   `static/thread_local/extern` and dropped the `auto`→`int` mapping
   (non-grammar inputs now fail instead of silently mis-typing);
   replaced the no-op-deleter `shared_ptr` alias reference with a plain
   pointer (namespace ownership lives only in the parent's vectors);
   dropped a redundant post-`}` semicolon consume already handled by
   empty-declaration.

## Performance evidence

Release build, `/usr/bin/time`, interleaved runs, final binary vs
`nsdecl-ref` on identical inputs (byte-identical outputs on all):

- 20k mixed declarations (qualified typedefs, parenthesized parameters,
  varargs): 0.98 s / 275 MB vs ref 137.9 s / 341 MB (ref is superlinear
  here; ours is the linear side of a 140× gap).
- 40k of the same: 1.98 s / 514 MB — 2.0× time and RSS, linear scaling.
- 2000-namespace transitive using-directive chain + lookup through it:
  0.03 s / 11 MB (ref 0.78 s / 19 MB). Per-lookup directive DFS with a
  visited set is O(V+E); no eager closure is materialized.
- 1000-deep inline-namespace nesting with lookups from the bottom:
  0.01 s / 7 MB (ref 0.58–0.93 s / 44 MB).
- Parenthesized-declarator depth 4000 (redundant parens fast path) and
  depth 3000 with ptr-operators per level (full chain fold): 0.00 s /
  0.01 s. Course 600 canaries: 0.00 s (paren), 0.01 s (directive chain).
- Peak RSS is ~13 KB per input line, dominated by per-token
  `std::string` spellings and per-node `shared_ptr` type allocations —
  the same measured spec §4 exception recorded for pa6, accepted at
  stage scale (graded fixtures < 2 KB, every test ≪ 1% of the 10 s
  cap); the recorded upgrade before compiler-scale inputs remains
  interned spellings + arena/value-interned types.

## Conformance validation

- `make test-report-through-pa7`: 354/354 (313 through-pa6 unchanged +
  25 pa7 local + 16 pa7 course); pa7 file audit passes (one pre-existing
  non-fatal header-division warning on `recog_parser.h`).
- 24 directed defined-behavior differential probes all byte-match the
  reference: anchoring shapes, inline-set priority (qualified and via
  alias), transitive directives, unnamed/inline combinations,
  using-declaration imports, qualified declarator merges and misses,
  cv/array/reference typedef composition, reference collapse chains,
  function/array declarator groupings, multi-dimensional pointer arrays,
  literal bound forms, `(void)` via qualified typedef, absolute
  qualified parameters and declarators.
- **Divergence envelope (recorded, deliberate)**:
  - *Anchoring*: `nsdecl-ref` attaches directive-nominated names at the
    directive's scope during the scope-chain walk, so a name in a
    nominated sibling namespace beats the same name in an intermediate
    enclosing namespace — the standard's 7.3.4p2/p5 anchoring implies
    the opposite. We match the reference (probe p1); the plan's open
    anchoring question is closed in the reference's favor.
  - *Same-scope ambiguity*: when a name is reachable in one scope both
    through an inline member and another directive, the reference
    errors ("ambiguous"); we return the inline-set answer (the
    standard's, since anchoring would separate the two). Ill-formed
    under the reference's own model, so ungraded (probe p5).
  - *Definition tracking*: the reference rejects duplicate definitions
    (`int a[2]; int a[];` → "duplicate variable definition"); we treat
    storage-class specifiers as output-neutral and merge redeclarations.
    Ill-formed inputs (UB per README); definition/linkage state is pa8+
    territory (probe p7).

## Checkpoint ledger

| id  | scope                                                    | proof | status |
|-----|----------------------------------------------------------|-------|--------|
| CP1 | semantic core: type model + factories, declarator engine, Table-10 seq, parser + envelope, named namespaces, direct/qualified lookup, typedef commit | 36/41 pa7 from 0/41; through-pa6 313/313; audit pass | DONE |
| CP2 | namespace semantics breadth: unnamed/inline namespaces, aliases, using-declarations, live transitive directives, inline-set qualified lookup, reopen check | 41/41 pa7; through-pa7 354/354; 200-chain probe 0.012 s | DONE |
| CP3 | final audit: inline-set priority staging, one-directional reopen check, declarator-id matching without directives, wrapper-chain declarator fold (2 composition bugs), qualified 8.2p7 lookahead, bound validation, dead-state/leniency cleanups | 354/354; 24 probes byte-identical, envelope recorded; linear 20k→40k scaling, 140× lead on ref's superlinear case | DONE |
