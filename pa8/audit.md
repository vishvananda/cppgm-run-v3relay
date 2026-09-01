# PA8 Final Architecture Audit — nsinit

## Scope and method

Traced representative facts through their full ownership paths: srcfile →
PA5 `PreprocEngine::RunSingleFile` → `Pa6TokenCollector` → strict-mode
`Pa7Parser` (grammar extension in place: initializers, static_assert,
empty-body function definitions, constexpr/inline, expressions) recording
syntax facts into per-TU `Pa7Namespace` models → `Pa8ProgramSema` (the
program-wide identity table: linking, constant evaluation, conversions,
temporaries, string literals) → `Pa8ImageBuilder` (layout, relocation,
byte emission) → binary outfile via the `dev/nsinit.cpp` envelope.
Ran 72 directed differential probes against `nsinit-ref` (exit status +
byte-wise image compare) covering initializer folding, pointer
conversions, linking identity, declaration-order visibility, bound
agreement, string emission, and overload handling; measured scaling on
20k/40k-declaration, 10k-reference-chain, and 1000-deep-namespace
probes.

## Ownership trace (clean)

- **Syntax facts**: the parser owns grammar and declaration-attribute
  facts only. Expressions are single `Pa8Expr` nodes (literal or
  id-expression path — exactly pa8.gram) carrying their lookup scope,
  token index, and a `decl_epoch` snapshot of the TU's declaration count;
  no evaluation happens at parse time. Literal array bounds are
  value-stamped opportunistically; expression bounds stay symbolic on the
  type node until the semantic pass evaluates them (`ArrayBound` writes
  the value and drops the expression — single transition, no stale
  state).
- **Entity facts**: per-TU redeclaration merging is the model's
  authority (`AddOrMergeVariable`/`AddOrMergeFunction`: type agreement,
  storage/thread_local/linkage agreement, one definition per TU,
  signature = name + parameter types). Program-wide identity is the
  sema's authority (`MakeVariableKey`/`MakeFunctionKey` + the
  `linked_variables_` binding rule). Two const authorities are distinct
  and reference-pinned: linkage-const is the decl-specifier const flag
  (`const char* p` is internal, probe p68), object-const is
  `IsConstQualified` on the declared type (initializer requirement p69,
  5.19 constancy p74).
- **Value facts**: `Pa8Value` = bytes + relocations (+ separate address
  relocations for lvalues), position-independent until the image builder
  resolves offsets in one fix-up pass. `EvaluateVariable` is memoized
  per definition with an active-flag cycle check; every defined
  variable's image value is readable during translation (the mock has no
  runtime), while `is_constant` tracks 5.19 constancy separately
  (constexpr, or const-qualified object) so static_assert/array-bound/
  constexpr contexts stay strict.
- **Image facts**: BLOCK1 in first-declaration (unit, order) via one
  stable sort; BLOCK2/BLOCK3 in registration order (= evaluation order,
  which the reference also uses — probe p80); relocation is a single
  pass over final offsets; symbols with no image object resolve to
  address 0 (undefined variables p43/p59, suppressed static_assert
  strings p82).
- **Envelope**: `dev/nsinit.cpp` mirrors nsdecl — argv `-o` contract,
  batch-stdin not-implemented loop, fresh preproc+parser per srcfile,
  strict mode, link + binary write after all TUs, any exception →
  EXIT_FAILURE.

## Findings and changes (this cleanup)

All conformance findings were surfaced by directed differential probes
(p-numbers) against `nsinit-ref`; every listed probe now matches in exit
status and image bytes.

1. **Non-constant initializers rejected instead of folded (material,
   fixed)**: any lvalue-to-rvalue of a non-const variable threw, but the
   reference folds arithmetic reads (`int b = a;` → a's value, p1; via
   references p21; cross-TU p49; float→int p55; int→bool p48). Relaxed
   loads now fold known bytes; only truly unknown values (no definition
   anywhere, p11) are errors, and require_constant contexts still reject
   non-5.19-constant reads (500-static-assert, p67, p74 unchanged).
2. **Pointer-from-pointer-lvalue copies (material, fixed)**: the
   reference realizes them as the *source variable's address*, even for
   constexpr sources and constexpr targets (p60/p70/p76) — not the
   loaded value. Emulated exactly; pointer→bool still folds the value
   and demands constancy (p31 rejected, p47 accepted).
3. **Pointer conversions were unchecked (material, fixed)**: cv-dropping
   and cross-type copies (p7/p14/p15), wrong-element array decay (p8),
   signature-mismatched function decay (p10b), wrong-element string
   decay (p39/p57), and non-constant integer→pointer (p79) all exited
   success. Added a 4.4 qualification-conversion checker
   (`PointerConvertible`, const-climbs rule), exact function-type match,
   string element-type match, and the 8.5.2 element-kind rule for
   string→array (ordinary literal → char/signed char/unsigned char only,
   u/U/L exact — p56/p58).
4. **Declaration-order visibility missing (material, fixed)**: post-hoc
   resolution saw the whole TU, accepting use-before-declaration in
   initializers, bounds, and static_assert (p6/p16/p17/p18). Expressions
   now carry `decl_epoch` and resolution skips candidates declared at or
   after it (continuing outward for unqualified lookup, so later
   same-name inner declarations don't shadow earlier outer ones).
5. **Overloaded names in expressions (material, fixed)**: the reference
   rejects any use of an overloaded function name, judged on the final
   overload set, with no target-type resolution (p38/p52/p53/p63); we
   silently took the first overload. Now rejected.
6. **Return-type-only redeclarations (material, fixed)**: `int f();
   double f();` in one TU created two entities (p4); 3.5 signatures are
   name + parameter types, so the model now rejects a same-TU
   redeclaration whose signature matches but whose type doesn't.
   Cross-TU the reference keys by full type — two entities, two stubs
   (p5) — so the sema's function key keeps the return type.
7. **Array-bound agreement compared expression pointers (material,
   fixed)**: every strict-mode bound carries an expression, so `int
   a[3]; extern int a[3];` and its cross-TU form were rejected
   (p2/p3). Literal-known bounds now compare by value at parse;
   expression bounds are re-checked per variable after evaluation
   (`declared_types` + `CheckDeclaredTypeAgreement`: p41/p42 accept, p78
   rejects). Cross-TU bounds are not checked at all — the reference
   accepts disagreement and the definition's type governs layout
   (p22/p22b/p22c).
8. **Linking identity refined (material, fixed)**: the reference binds a
   variable whose first declaration is non-defining to the first linked
   entity of its name; a variable whose first declaration *defines* it
   binds only to a still-undefined entity and otherwise starts a new one
   — duplicate plain definitions coexist in the image (p71/p85), while
   extern-declare-then-define collides (p84). Cross-TU duplicate
   *function* definitions merge silently into one stub (p83); same-TU
   double definition is always a model error, now including inline
   functions (p86). Entity keys stay unique via a per-name suffix.
9. **Extern reference declarations rejected (material, fixed)**:
   `extern const int& r;` demanded an initializer (p19); the
   needs-initializer checks now apply only to defining declarations, and
   const-needs-initializer keys off object const-qualification
   (`const char* p;` legal, p69; const arrays still rejected,
   340-array-const).
10. **static_assert string emission (material, fixed)**: a string
    literal in a static_assert *condition* converts to bool true but is
    not part of the image (p82); registration is suppressed during the
    assert pass and the dangling symbol resolves to 0. Bool conversion
    now also accepts array/function decay.
11. **Structure cleanups**: removed the operator-expression layer the
    grammar doesn't contain (precedence climbing, unary/binary/
    conditional kinds and their evaluators, ~350 lines of untested
    non-grammar surface — non-grammar inputs now fail instead of
    silently evaluating); removed write-only state (`Pa8Expr::
    annotated_type/lvalue`, `Pa8Temporary/Pa8StringLiteral::first_use`,
    `Pa7Variable::initializer`), the dead `IsExpressionStart`, and the
    sema's duplicate `CompatibleTypes`; re-indented the mis-nested
    declarator block in `ParseSimpleDeclaration`.
12. **Quadratic symbol scans (performance, fixed)**: reading through a
    reference scanned temporaries and strings linearly per lookup —
    10k reference chains cost 0.72 s and grew 2.7× per doubling; a
    symbol→index map makes it 0.44 s at 2.1× (linear).

## Performance evidence

Release build, `/usr/bin/time`, final binary vs `nsinit-ref` on
identical inputs (byte-identical images on all):

- `600-deep-namespace-initialization` (graded canary): 0.03 s / 9.8 MB.
- 20k mixed declarations (const/constexpr chains, cross-references):
  0.48 s / 87 MB vs ref 37.96 s / 168 MB (79×). 40k: 0.94 s / 169 MB —
  2.0× time and RSS, linear.
- 10k reference chains + 3.3k string literals: 0.44 s / 78 MB vs ref
  3.84 s / 76 MB at half that size; 5k→10k is 2.1× after the symbol
  index (2.7× before).
- 1000-deep nested namespaces with per-level constexpr initializers,
  leaf and 50-level qualified reads: 0.06 s / 18 MB vs ref 0.96 s /
  63 MB.
- Peak RSS remains ~4 KB per declaration (token spellings + per-node
  type allocations), the same measured envelope recorded for pa6/pa7,
  accepted at stage scale (graded fixtures ≤ a few KB; slowest graded
  test ≪ 1% of the 10 s cap).

## Conformance validation

- `make test-report-through-pa8`: 414/414 (354 through-pa7 unchanged +
  41 pa8 local + 19 pa8 course); pa8 file audit passes (one pre-existing
  non-fatal header-division warning on `recog_parser.h`).
- 72 differential probes match the reference in exit status and, where
  both succeed, in image bytes: folding (arithmetic, cross-TU, through
  references), pointer/array/function/string conversions and their
  rejections, null-pointer constants, bound agreement and completion,
  declaration-order visibility, linking identity and duplicate
  definitions, overload rejection, static_assert semantics, undefined-
  variable addresses, BLOCK2/3 ordering and non-dedup (p81).
- **Divergence envelope (recorded, deliberate)**:
  - *Using-declaration position*: an expression between an entity's
    declaration and a later using-import of it can resolve through the
    import (imports carry no position); the reference would reject.
    Namespace names in qualified paths are likewise not epoch-filtered
    (namespaces carry no order) — their members are, which covers the
    reachable cases.
  - *BLOCK2 order*: temporaries append in evaluation order, which
    equals program first-use order for every fixture and probe;
    a cross-TU forward dependency chain could in principle deviate.
  - *Bound-agreement error site*: non-literal bound disagreement
    reports after evaluation (sema) rather than at the redeclaration,
    same exit status.

## Checkpoint ledger

| id  | scope | proof | status |
|-----|-------|-------|--------|
| CP1 | semantic core end-to-end: grammar extension, annotated expressions, constant evaluation, scalar/pointer initialization + conversions, 3.5 linking, BLOCK1 layout + writer, static_assert | 57/60 pa8 from 0/60; through-pa7 354/354; audit pass | DONE |
| CP2 | references + lifetime-extended temporaries (BLOCK2), unknown-bound char arrays + string literals (BLOCK3), chained reference lvalues, aligned relocations | 60/60 pa8; through-pa7 354/354; three focused images byte-match | DONE |
| CP3 | final audit: relaxed folding + 5.19 separation, pointer-conversion checking, declaration-epoch visibility, linking-identity rules, bound agreement via declared types, overload rejection, static_assert string suppression, operator-layer removal, symbol-index maps | 414/414; 72 probes byte-identical; 20k→40k and 5k→10k linear; 79× lead on ref at 20k | DONE |
