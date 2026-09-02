# PA11 Plan — cppgm++ --emit-types (scope/type dump)

Grading: `cppgm++ --emit-types -o x.my x.t`; exit status must match
`x.ref.exit_status` (18 fixtures expect EXIT_FAILURE, 50 expect success and
a byte-exact dump).  No reference binary: the 68 checked-in `.ref` files
under `pa11/tests/{spec,general}` are the only format oracle, so every dump
convention below is fixture-pinned.  Harness: forked batch runner, 10 s per
test.  `cppgm.tests/course/pa11` is empty.  Final review, findings,
performance evidence and validation are in `audit.md`.

## Stage Design

Data flow per translation unit (fresh state per file, argv order):
`PreprocEngine::RunSingleFile` → `Pa6TokenCollector::tokens` →
`Pa10Parser` (arena AST, dump unchanged) → `ScopeBuilder(tokens, arena,
model).Build(root)` → `SemaModel` (scope tree, entities, interned types) →
`PrintTypesUnit`.  Any exception → EXIT_FAILURE.  The builder reads every
name from a node's token span through `ReadQualifiedName`; it never splits
dump text and never re-parses tokens beyond the typed checks named below.

Owning boundaries (`dev/src/sema/`; parser files change only as listed):

- `type_table.h/.cpp` — `TypeId` (0 = null), `TypeKind`, `TypeKeyword`
  (struct/class/union, typename/class, template-parameter), `TypeNode`,
  `TypeTable`.  Derived types are interned on typed keys (`cv_`,
  `pointers_`, `references_`, `arrays_`, `functions_`) so equal types share
  one id.  Class, enum and template-parameter types carry `entity` plus the
  spelling of the declaration that introduced them: one entity may own
  several type ids (one per declaration spelling, as the dump prints them);
  semantic identity is the entity.  `Cv` merges qualifier layers, applies to
  array elements and is ignored on references and functions; `Pointer`,
  `Reference`, `Array`, `Function` reject the ill-formed compositions
  (8.3.1/8.3.2/8.3.4/8.3.5).  `Spell(ostream&, TypeId)` renders on demand
  (fundamentals via `FundamentalTypeToStringMap`).  `SizeOf`/`AlignOf` use
  `FundamentalSize` (char 1, short/char16_t 2, int/wchar_t/char32_t/float
  4, long/long long/double/pointer 8, long double 16); enum → underlying,
  reference → referent, array → element × bound; class, function, void →
  error.  `FundamentalFromKeywords` is the one 3.9.1 keyword table;
  `FundamentalIsIntegral/IsUnsigned` back constant evaluation.
- `qualified_name.h/.cpp` — `QualifiedName {global, components}` and
  `ReadQualifiedName(tokens, first, last)`: identifiers and `::` only; a
  template-id, operator name, destructor or decltype qualifier in the span
  throws (unsupported in PA11), which is how using-declarations and type
  names that contain template-ids fail.
- `scope_model.h/.cpp` — `ScopeKind`, `BindingKind`, `LookupFilter`,
  `Binding {name, kind, type, namespace_scope, const value}`, `Scope {kind,
  name, parent, inline flag, bindings (declaration order), children
  (creation order), using_directives, index}`, `ClassEntity {class_scope,
  is_union, defined}`, `EnumEntity {enum_scope, underlying, scoped,
  defined}`, `SemaModel`.  The per-scope `index` (name → bindings in
  declaration order) is built once a scope exceeds 8 bindings; output order
  always comes from the vectors.  Lookup: `DirectBinding` (latest match in
  one scope), `LookupUnqualified(scope, name, filter)` (parents, then each
  namespace's inline children and using-directives with a visited set),
  `LookupTypeName` (3.3.10: a later object/function/enumerator/parameter
  hides a type name), `LookupQualified(scope, QualifiedName, filter)`
  (leading `::` → global; first component with `LOOKUP_QUALIFIER`; later
  components inside the nominated scope: namespace with inline set and
  using-directives (3.4.3.2p2), class scope, scoped enum scope, or the
  enumerators of an unscoped enum), `Lookup` dispatching on the name form.
  The scope a binding nominates is derived on demand (`NominatedScope`:
  namespace binding → its scope; type binding → the entity's class or
  enumerator scope), so an alias declared before the class is defined still
  qualifies once the definition exists.
- `const_eval.h/.cpp` — `ConstEvaluator::Evaluate(AstId, ScopeId)` over
  `literal`, `true/false`, `id-expression` (binding with a recorded constant
  value), parentheses, unary `+ - ! ~`, binary arithmetic/shift/relational/
  equality/bitwise/comma, `&&`/`||` evaluating only the selected operand,
  conditional, casts and `sizeof`/`alignof`.  Signed 64-bit with checked
  overflow; evaluated division by zero → error.  Operand types come from the
  builder through `ConstantOperandTypes` (`TypeOfTypeId`,
  `TypeOfExpression`) so there is one type-construction path.
- `scope_builder.h/.cpp` (declarations, scopes, classes, enums, templates,
  statements) + `type_builder.cpp` (specifier sequences, declarators,
  type-ids, parameters, decltype and operand types).  `ScopeBuilder`
  implements `ConstantOperandTypes`; `BuildSpecifierType(seq, scope,
  anonymous_name)` threads the first declarator's name explicitly for
  unnamed class/enum specifiers.  Unsupported constructs (special members,
  bit-fields, explicit instantiations, template-ids in names, trailing
  return types) throw rather than being skipped.
- `types_dump.cpp` — `PrintTypesUnit`: `start/end translation unit k`,
  `translation-unit`, then each scope at depth d as `scope <kind>[ <name>]`
  (root `<global>`; blocks and template scopes have no name), its bindings
  `<kind> <name> <type>[ <value>]` (enumerator values in decimal; namespace
  bindings unprinted; an empty parameter name still prints both spaces),
  then child scopes.  Two spaces per depth.
- `dev/cppgm++.cpp::run_emit_types_mode` — one `TypeTable`, `SemaModel`,
  parser and builder per file.
- Parser facts consumed (`dev/src/parser/ast_model.h`): node `kind`,
  `children`, and the token span `[first, last)` of every node that carries
  text; `AST_ENUM_SPECIFIER` is a definition (possibly empty body) and
  `AST_ENUM_DECLARATION` a body-less `enum-key name enum-base?` (opaque
  declaration or elaborated specifier — both print `enum-specifier`);
  `AstArena::DeclarationExtent` is a cold sidecar recording the declaration
  extent of a bare unnamed class/enum specifier for its generated identity.

Declaration rules (fixture-pinned):

- Every declaration appends a binding line; redeclarations are not merged
  (`struct C; class C {};` → `type C struct C`, `type C class C`, one
  `scope class C`; a use between them spells the latest declaration's key).
- Namespaces: `namespace N` reopens the child scope; a namespace name and
  another entity of that name in one scope → error (3.3.1p4).  `inline`
  sets the flag; unnamed → `scope namespace <unnamed>` plus an implicit
  using-directive.  Aliases bind an unprinted namespace binding; a
  non-namespace target → error.  Using directives add to the current scope
  only.  Using declarations copy the found binding's kind, type and
  constant value (`type-alias` stays `type-alias`, class/enum stay `type`);
  unresolved, namespace or template-id target → error.
- Specifiers: 3.9.1 keyword combinations; cv applies to the base;
  `constexpr` adds top-level const to object declarators (7.1.5p9);
  `static extern thread_local inline virtual friend mutable explicit
  register` change nothing; a name must resolve to a type (ordinary lookup
  with 3.3.10 hiding when unqualified); `decltype(e)`: `id-expression` →
  declared type, `(lvalue)` → lvalue reference to it, enumerators are
  prvalues; other operands → error.  Elaborated `struct S` uses
  types-only lookup, must name a class if found, and otherwise declares S in
  the nearest enclosing namespace or block scope when it is part of a
  declaration (7.1.6.3p2, 3.3.2p7); elaborated `enum E` must name an
  enumeration.
- Declarators (8.3): ptr-operators with their cv-qualifiers apply to the
  base left to right, then suffixes right to left (`int a[2][3]` is array of
  2 array of 3 int), then a parenthesised declarator takes that type as its
  base (`int *(*p)[3]` is pointer to array of 3 pointer to int).  Array
  bounds via `ConstEvaluator`, converted value > 0.  `(void)` alone → empty
  parameter list; `...` → variadic; parameter types stay as declared (no
  decay).  `noexcept`/`throw()`, attributes and virt-specifiers are ignored;
  a trailing return type → error.
- Declarators bind `type-alias` (typedef), `function` (function type) or
  `variable`; a const integral variable whose initializer evaluates records
  its value.  Function definitions bind in the scope the declarator-id
  names, create `scope function <name>` there, bind `parameter`s resolved
  in that scope, and own the body `scope block`; nested compound statements
  nest; other statements contribute only declarations and blocks.
- Classes: a definition completes a class declared earlier in the same
  scope (or the qualified scope), else introduces one; struct/class are
  interchangeable, union versus non-union → error; redefinition → error.
  Members: simple declarations, member function definitions, nested
  classes/enums, typedefs/aliases, using declarations, static_assert;
  access specifiers ignored; special members and bit-fields → error.  An
  unnamed class/enum in a declaration takes the first declarator's name
  (`typedef struct {…} S;` → `type S struct S` and `type-alias S struct S`).
  A bare anonymous union is `__anonymous_union_type__<first>_<last>` from
  its declaration extent (`__anonymous_union_type__0_10` for a 10-token
  declaration) and its members are also bound as variables in the enclosing
  scope.
- Enums: unscoped definitions bind `type E enum E` and each `enumerator <n>
  enum E <v>` in the enclosing scope (no scope line); scoped enums bind the
  type and create `scope enum <name>` holding the enumerators, also for
  opaque `enum class E;`.  Opaque unscoped without enum-base → error;
  redeclaration with another underlying type or scoped-ness → error.  A
  qualified definition (`enum class writer::state : char {…}`) completes
  the member entity but prints as a new declaration in the current scope
  spelled with the joined name (`type writer::state enum class
  writer::state`, `scope enum writer::state` holding the enumerators);
  the member declaration keeps its local spelling.
- Templates: `scope template-parameters` under the current scope, `type T
  typename T` / `type T class T` per type parameter and `type TT
  template-parameter TT` per template template parameter (its inner clause
  is not a scope); non-type parameters are typed but not bound; the
  declaration is built inside that scope.  Template-ids in names → error.
- `sizeof(T)` / `alignof(T)` whose id-expression resolves to a type name
  is taken over the type; `sizeof`/`alignof` results have type
  `unsigned long int`.

## Performance Bounds

- Declaration in a scope: O(1) amortized (per-scope name index past 8
  bindings; linear scan below that).  Lookup: O(scope depth) hash probes
  plus the using-directive closure, each namespace visited once.
- Types are interned once on typed keys; spelling is rendered only when
  printing, linear in the dump.  Entity scopes are derived per lookup step
  in O(1); no scan over entities anywhere.
- Constant expressions are evaluated once at their declaration; values are
  cached on the binding, so chained enumerators and constexpr chains stay
  linear.  No retry loops, no deferred worklists.
- Recursion depth follows AST depth, bounded by the PA10 parser.
- Evidence (medians of interleaved runs, immutable executables) is in
  `audit.md`; every probe is linear and sub-second at 20000–40000
  declarations.

## Checkpoint Ledger

- CP1 core model, driver, namespaces/classes/functions/types — 45/68 stage
  tests, through-pa10 589/589.
- CP2 enums, constant evaluation, sizeof/alignof, static_assert, parser
  extents and qualified enum names — 66/68.
- CP3 template-parameter scopes and template-id rejection — 68/68,
  through-pa11 657/657.
- CP4 final architecture cleanup (this audit) — parallel type resolver,
  textual keys and joined names removed; entity-linked types with derived
  scopes; indexed lookup; quadratic paths and several correctness defects
  fixed; 657/657 through pa11, file audit passing, fixture outputs
  unchanged.
