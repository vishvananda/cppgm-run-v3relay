# PA11 Final Architecture Audit — cppgm++ --emit-types

## Scope and method

Traced representative scope/type facts through their full ownership paths:
srcfile → PA5 `PreprocEngine::RunSingleFile` → `Pa6TokenCollector::tokens`
(typed kinds, decoded literal bits) → `Pa10Parser` (arena AST; every named
node carries the token span it was rendered from) → `ScopeBuilder` (reads
names from spans through `ReadQualifiedName`, declares into `SemaModel`,
builds `TypeTable` types) → `PrintTypesUnit` (renders spellings on demand)
→ outfile in the `dev/cppgm++.cpp` envelope.  Facts followed end to end: a
class name (`struct C; class C {}` → one entity, two spellings, one scope;
an alias declared between them qualifying `D::T` after the definition), an
enumerator (value evaluated once, cached on the binding, reused by array
bounds and `static_assert`, reached through `E::A`, `Alias::A`,
`N::CY::EY::A` and using-declarations), a declarator (`int *(*p)[3]`,
`int a[2][3]`, `void (*fp)(void)`, qualified declarator-ids resolving
parameters in the named scope), an elaborated specifier (`struct S* p` as a
class member declaring `S` in the enclosing namespace), a qualified
out-of-class scoped-enum definition (the fixture-pinned two-spelling shape),
a `sizeof(T)` whose operand lookup decides type versus object, and the
anonymous-union identity derived from its declaration extent.

Measured scaling with generated inputs: 10k/20k/40k declarations in one
scope, 5k/10k/20k class definitions followed by as many typedefs, 5k/20k
chained enumerators, 10k function definitions, the plan's 200- and
400-namespace probe, a 5000-step constexpr chain and a 400-deep block
chain.  Compared the pre-cleanup and final executables (immutable copies)
on 228 checked-in `.t` inputs from pa6, pa7, pa8, pa10 and pa11 in both
`--emit-types` and `--emit-ast` mode, and timed the probes with
interleaved runs and medians of five on a quiet host.

## Ownership trace (final)

- **Name facts**: `ReadQualifiedName(tokens, first, last)` is the single
  reader of names: identifiers and `::` produce `QualifiedName {global,
  components}`; any other token in a name span (template-id, operator,
  destructor, decltype qualifier) is rejected there.  Nothing in `sema/`
  reads `AstNode::text`.
- **Type facts**: `TypeTable` owns every type.  Derived types are interned
  on typed keys (`(base, cv bits)`, `base`, `(base, lvalue)`, `(element,
  bound)`, `(result, variadic, parameters)`), so equal derived types are
  one id and equality is `==`.  Class, enum and template-parameter types
  carry `entity` plus the declaring spelling: the dump prints each
  declaration with its own class-key or qualified spelling, so one entity
  may own several type ids and semantic identity is `At(t).entity`.
  `FundamentalFromKeywords` and `FundamentalSize/IsIntegral/IsUnsigned` are
  the one keyword table and the one target table; `Spell` renders through
  the shared `FundamentalTypeToStringMap`.
- **Scope facts**: `SemaModel` owns scopes (bindings in declaration order,
  children in creation order, using-directives, a per-scope name index past
  8 bindings), bindings (`name, kind, type, namespace_scope, const value`)
  and entities (`ClassEntity {class_scope, is_union, defined}`,
  `EnumEntity {enum_scope, underlying, scoped, defined}`).  The scope a
  binding nominates is derived when a qualified lookup steps through it
  (`NominatedScope`: namespace → its scope; type or alias → the entity's
  class or enumerator scope), never copied onto the binding.
- **Constant facts**: enumerators and const integral objects record their
  value on the binding at declaration; `ConstEvaluator` reads them by
  lookup and obtains operand types (casts, `sizeof`, `alignof`) from the
  builder through `ConstantOperandTypes`, so there is one type-construction
  path.
- **Parser facts consumed**: `AST_ENUM_SPECIFIER` (definition) versus
  `AST_ENUM_DECLARATION` (body-less; opaque or elaborated, decided by
  sema), both printed as `enum-specifier`; bare unnamed class/enum
  specifiers record their declaration extent in the `AstArena` cold sidecar
  `DeclarationExtent`, keyed by node id, from which the fixture-pinned
  `__anonymous_union_type__<first>_<last>` identity is rendered.
- **Presentation**: `PrintTypesUnit` writes scope kind/name, binding
  kind/name and `TypeTable::Spell` into the stream; no spelling is stored
  ahead of the dump except the declaring name of a named type.

## Findings and changes (this cleanup)

1. **Parallel type resolver (material, removed)**: `ConstEvaluator` carried
   its own `ResolveType/ResolveTypeSequence/ResolveTypeSpecifier/
   ResolveAbstractDeclarator/ResolveExpressionType`, `BuildFundamental`,
   `IsBuiltin` and `NameComponents` — a second implementation of specifier,
   declarator and name construction next to the builder's (the file audit
   flagged the duplicated block).  The evaluator now asks the builder for
   operand types through the `ConstantOperandTypes` interface; the copies
   are gone.
2. **Quadratic declaration and lookup (material, fixed)**: every
   `AddBinding` scanned the whole scope for the namespace-conflict check
   and every lookup scanned it again; `TargetScopeForType` scanned every
   class and enum entity per alias declaration.  40000 declarations in one
   scope took 4.3 s and 20000 classes plus typedefs 7.1 s (×4.6 and ×3.3
   per doubling).  Scopes now index names once they exceed 8 bindings and
   entity scopes are derived in O(1); the same inputs take 0.33 s and
   0.56 s, indistinguishable from `--emit-ast` on the same files.
3. **Stale derived state (material, fixed)**: `Binding.target_scope` was a
   snapshot of the entity's scope at binding time, 0 for every forward
   declaration and for every alias declared before the class definition
   (`struct C; typedef C D; struct C { typedef int T; }; D::T x;` failed
   with "unknown type name").  Entity scopes are now derived on demand.
4. **Textual downgrades (fixed)**: intern keys rendered as
   `"cv:<id>:1:0"` strings, entity keys as `"<scope>:<name>"`, class keys
   and template-parameter keywords as strings, and the enum name joined by
   the parser into `text` and split on `::` by both sema consumers (with the
   node span overloaded to carry the definition extent).  Keys are typed
   tuples, keywords are the `TypeKeyword` enum, the enum node keeps its
   name span and a distinct kind marks body-less forms, and the anonymous
   extent lives in the arena's sidecar.  Template-ids were encoded into name
   strings as `<`/`>` characters and later searched for with `find('<')`;
   the reader now rejects them as tokens.
5. **Fix below the owning layer (moved)**: the parser rejected any `<` in a
   using-declaration target although `pa11.gram` accepts a template-id
   there; 7.3.3p5 is a semantic rule.  The check is gone from the parser and
   `300-using-declaration-template-id-bad` fails in sema, as before.
6. **Latent memory-safety defect (fixed)**: the class builder held a
   `ClassEntity&` across the member walk while nested class definitions
   grew the entity vector; the rewritten path surfaced it immediately
   (`100-nested-class` read a dangling scope id).  Builders now keep ids,
   not entity references, across recursion.
7. **Declarator composition (fixed)**: array suffixes were applied left to
   right (`int a[2][3]` printed array of 3 array of 2 int) and the nested
   declarator applied the prefix after the inner declarator (`int *(*d)[6]`
   printed pointer to pointer to array of 6 int).  Suffixes apply right to
   left onto the prefixed base and the parenthesised declarator wraps that
   type (8.3), giving array of 2 array of 3 int and pointer to array of 6
   pointer to int.
8. **Target facts (fixed)**: `wchar_t` was 2 bytes/16 bits (PA8 and the
   target say 4), `char16_t`/`char32_t` were converted as signed,
   `sizeof(reference)` was 8 instead of the referent's size, cv on a
   reference or function type produced a nonsense layer instead of being
   ignored, and cv on an array type did not reach the element.
9. **Standard-conformance holes (fixed)**: leading `::` was dropped
   (`::N::a` looked up `N::a` unqualified); an elaborated `enum E` reference
   to an enum with a non-`int` fixed underlying type failed as a
   "disagreeing redeclaration"; an elaborated `struct T*` member declared `T`
   inside the class instead of the enclosing namespace (7.1.6.3p2), so a
   later `T* q` at namespace scope failed; `E::A` for an unscoped enum was
   unsupported; union versus non-union redeclaration was never checked
   although the plan required it; `decltype((f))` for a function was not an
   lvalue; `sizeof(x)` had type `int` instead of `unsigned long`.
10. **Unsupported constructs skipped silently (fixed)**: `BuildNode`
    returned for unknown declaration kinds, so special members, bit-fields
    and explicit instantiations produced a partial dump with EXIT_SUCCESS.
    They now fail, as the plan states.
11. **Dead vocabulary and hidden state (removed)**: `Binding.print`
    (duplicate of `kind == BINDING_NAMESPACE`), `Binding.class_entity/
    enum_entity` (derivable from the type), `ClassEntity.name/class_key/
    parent_scope/current_type`, `EnumEntity.name/parent_scope/type`, the
    unused two-argument `ConstEvaluator` constructor, the by-name entity
    maps, `Scope.name = "block"`, and the `active_anonymous_name_` member
    used as an implicit parameter (now an explicit argument).  Non-type
    template parameters were bound as `variable`s although the plan and the
    README leave them unbound; they are typed and not bound.

## Behaviour on checked-in inputs

`--emit-types`, 228 inputs: 207 identical (all 68 pa11 fixtures among
them); 16 pa10 inputs with special members or bit-fields now fail instead
of dumping a partial model (finding 10); 2 pa10 inputs with template
specializations now fail on the template-id in the class name instead of
binding a class named `S<int>`; `200-friend-type-declaration` declares the
friend classes in the enclosing namespace rather than the class scope
(11.3p11, finding 9); `200-member-template-if-less-template-call` no longer
prints non-type parameters as variables (finding 11); and
`300-local-typedef-shadows-value-qualified-type` now succeeds with the
correct `variable d int` (finding 3).  `--emit-ast`, 228 inputs: identical
except `300-using-declaration-template-id-bad`, which now parses (finding
5).

## Performance evidence

Immutable executables, interleaved, medians of 5 on a quiet host; the last
column is the final executable's parser-only mode on the same input.

| probe | old --emit-types | final --emit-types | final --emit-ast |
| --- | --- | --- | --- |
| 10000 / 20000 / 40000 declarations, one scope | 0.32 / 0.93 / 4.31 s | 0.07 / 0.16 / 0.33 s | 0.07 / 0.16 / 0.32 s |
| 5000 / 10000 / 20000 classes + typedefs | 0.48 / 2.17 / 7.13 s | 0.13 / 0.27 / 0.56 s | 0.13 / 0.26 / 0.54 s |
| 5000 / 20000 chained enumerators | 0.16 / 1.74 s | 0.04 / 0.18 s | 0.04 / 0.18 s |
| 10000 function definitions with parameters and a block | 0.68 s / 84 MB | 0.35 s / 84 MB | 0.36 s / 84 MB |
| plan probe, 200 / 400 namespaces × 100 declarations | 0.41 / 0.82 s | 0.38 / 0.79 s | 0.40 / 0.79 s |
| 5000-step constexpr chain | 0.13 s | 0.08 s | 0.08 s |
| 400-deep block chain | 0.01 s / 5 MB | 0.01 s / 5 MB | 0.02 s / 5 MB |

Peak RSS is unchanged on every probe (the per-scope index adds under 0.1 %
on the 10000-function input, which has 30000 scopes).  The old executable
grew ×4.6 per doubling on the flat scope, ×3.3 on the alias probe and ×10.9
for ×4 enumerators; the final one grows ×2 everywhere and its semantic pass
is within measurement noise of the parser alone, so remaining cost is
tokenization, node construction and output (see the PA10 audit's profile).
Outputs of old and final are byte-identical on all 13 probes.

Accepted at stage scale: `Binding.name` and `Scope.name` are per-record
`std::string`s (arbitrary input spellings; interning to a `NameId` was
considered and rejected because lookup is already hash-and-compare once per
scope and the strings are the dump's model); `visited` in the
using-directive closure is a small vector searched linearly (namespaces
nominated per lookup are few); `Spell` builds strings recursively only at
print time.

## Conformance validation

- `make test-report-through-pa11`: 657/657 (589 through pa10 unchanged +
  68 pa11; the course pa11 set has 0 tests).  `make test-pa10` alone:
  157/157 after the parser changes.
- `perl scripts/cppgm_file_audit.pl --stage pa11 --paths dev/src`: passes
  with the one pre-existing `recog_parser.h` division warning; the
  `type_builder.cpp`/`const_eval.cpp` duplication warning is gone.
- Differential runs above: fixture-pinned output unchanged; every changed
  behaviour is listed with its finding.
- Semantic sources: 2818 lines over 13 files in `dev/src/sema/` (was 3043
  over 11), plus the `AST_ENUM_DECLARATION` kind, the arena extent sidecar
  and the using-declaration revert in the parser.

## Checkpoint ledger

- CP1 core model, driver, namespaces/classes/functions/types — 45/68,
  through-pa10 589/589.
- CP2 enums, constant evaluation, sizeof/alignof, static_assert, parser
  extents and qualified enum names — 66/68.
- CP3 template-parameter scopes and template-id rejection — 68/68,
  through-pa11 657/657.
- CP4 final architecture cleanup (this audit) — single type-construction
  path, typed keys and names, entity-linked types with derived scopes,
  indexed lookup, quadratic paths and the defects above fixed; 657/657
  through pa11, file audit passing, fixture outputs unchanged.
