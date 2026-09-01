# PA11 Plan — cppgm++ --emit-types (scope/type dump)

Grading: `cppgm++ --emit-types -o x.my x.t`; exit status must match
`x.ref.exit_status` (18 fixtures expect EXIT_FAILURE, 50 expect success and
a byte-exact dump).  No reference binary: the 68 checked-in `.ref` files
under `pa11/tests/{spec,general}` are the only format oracle, so every dump
convention below is fixture-pinned.  Harness: forked batch runner, 10 s per
test.  `cppgm.tests/course/pa11` is empty.

## Stage Design

Data flow per translation unit (fresh state per file, argv order):
`PreprocEngine::RunSingleFile` → `Pa6TokenCollector::tokens` →
`Pa10Parser` (arena AST, unchanged dump) → `ScopeBuilder(tokens, arena,
types).Build(root)` → `SemaModel` (scope tree + interned types) →
`PrintTypesUnit`.  Any exception → EXIT_FAILURE.  The builder reads names
from node token spans `[first, last)` (typed tokens), never by splitting
`text`; it never re-parses tokens except the typed checks named below.

Owning boundaries (new directory `dev/src/sema/`; PA10 parser files change
only where CP2 says so):

- `sema/type_table.h/.cpp` — `TypeId` (0 = null), `TypeKind {Fundamental,
  Cv, Pointer, LvalueRef, RvalueRef, Array, Function, Class, Enum,
  TemplateTypeParam, TemplateTemplateParam}`, `TypeNode {kind, fundamental,
  cv, inner, bound, params, varargs, ret, entity}`, `TypeTable` interning
  every constructor (`Fundamental/Cv/Pointer/Reference/Array/Function/
  Class/Enum/TemplateParam`) through a `std::map` key so equal types share
  one id and equality is `==`.  `Spell(TypeId)` renders on demand:
  fundamentals via `FundamentalTypeToStringMap` (`long int`, `unsigned
  char`), `const `/`volatile ` prefix, `pointer to X`, `lvalue-reference to
  X`, `rvalue-reference to X`, `array of N X`, `function of (P, Q, ...)
  returning R` (`()` when empty), `<key> <name>` for classes (`struct C`,
  `class C`, `union U`), `enum <name>` / `enum class <name>`, `typename T`,
  `template-parameter TT`.  `SizeOf/AlignOf(TypeId)`: fundamental table
  (char/bool 1, short 2, int/wchar_t/char32_t/float 4, long/long long/
  double/pointers 8, long double 16), enum → its underlying type, arrays
  multiply, incomplete class / class layout / function / void → error.
- `sema/scope_model.h/.cpp` — `ScopeKind {Namespace, TemplateParameters,
  Class, Enum, Function, Block}`; `Scope {kind, name span/spelling, parent,
  bindings (declaration order), children (creation order), using_directives,
  inline flag, name index: unordered_map<string, vector<index>> used only
  for lookup, never for output order}`; `Binding {BindingKind {Type,
  TypeAlias, Enumerator, Function, Variable, Parameter, Namespace}, name,
  TypeId, class_key (Type lines), has_value/value (enumerators and const
  integral objects with constant initializers), scope link (namespace,
  namespace alias, class, scoped enum)}`.  `ClassEntity {scope, key, complete}`,
  `EnumEntity {scope or enumerator list, scoped, underlying TypeId,
  fixed}`.  Lookup (3.4): `LookupUnqualified(scope, name, filter)` walks
  parents; in each scope the latest binding matching the filter wins
  (3.3.10: a value hides a type for ordinary lookup), then that scope's
  using-directive closure (visited set) and inline children (7.3.1p8).
  Filters: Any, TypesOnly (3.4.4 elaborated), ScopePrefix (3.4.3p1: skip
  values).  `LookupQualified(span)`: leading `::` → global; first component
  ScopePrefix-unqualified; later components inside the found namespace
  (direct, inline set, then using-directives, 3.4.3.2p2), class scope, or
  enum (scoped scope or unscoped enumerator list); a TypeAlias component is
  followed to its class/enum.  Any `<` inside the span → error.
- `sema/const_eval.h/.cpp` — `ConstEvaluator::Evaluate(AstId) → IntValue
  {TypeId, bits, is_signed}` over AST expression nodes: `literal` (token
  `lit_type/lit_value` at `node.first`; integer, character, bool only),
  `id-expression` (enumerator value, const integral variable/parameter with
  recorded constant), `parenthesized-expression`, unary `+ - ! ~`, binary
  arithmetic/shift/relational/equality/bitwise, `&&`/`||` evaluating only
  the selected operand, `conditional-expression`, `cast-expression`
  (`static_cast<integral>` and C-style `(int)e`), `sizeof-expression` /
  `type-trait-expression alignof` over a `type-id` or an `id-expression`
  that resolves to a type.  Usual arithmetic conversions (int promotion,
  signed/unsigned); signed overflow and evaluated division by zero → error.
  Consumers: array bounds (converted value must be > 0), enumerators
  (implicit = previous + 1, first 0), `static_assert` (false → error).
- `sema/scope_builder.h/.cpp` (+ `sema/type_builder.cpp` for
  specifier/declarator type construction) — the AST walker described under
  Declaration rules.  Unsupported constructs throw (EXIT_FAILURE) rather
  than being silently skipped.
- `sema/types_dump.cpp` — `PrintTypesUnit(out, k, model)`: framing from
  `ast_model` (`PrintHeader`, `start/end translation unit k`), then
  `translation-unit`, then each scope at depth d: `scope namespace <name>`
  (root `<global>`), `scope template-parameters`, `scope class <name>`,
  `scope enum <name>`, `scope function <name>`, `scope block`; then its
  bindings `<kind> <name> <type>[ <value>]` (Namespace bindings are not
  printed; an empty name still prints both spaces: `parameter  int`), then
  child scopes in creation order.  Two spaces per depth, `'\n'` per line.
- `dev/cppgm++.cpp::run_emit_types_mode` — mirror `run_emit_ast_mode`; one
  `TypeTable`, parser and builder per file.
- `dev/frontend_source_sets.mk` — add the `sema/*` units to
  `FRONTEND_OBJ_BASENAMES_cppgm++`.

Declaration rules (fixture-pinned):

- Every declaration appends a binding line; redeclarations are not merged
  (`struct C; class C {};` → `type C struct C` then `type C class C`, one
  `scope class C`; `void f(T); void n::f(T) {}` → two `function f` lines).
- Namespaces: `namespace N` reopens the existing child scope; a namespace
  name and an object/function name in one scope collide both ways →
  error.  `inline` sets the flag; no output marker.  Alias definitions
  bind an unprinted Namespace binding; a non-namespace target → error.
  Using directives add to the current scope only.  Using declarations copy
  the found binding with its kind (`type-alias` stays `type-alias`, class/
  enum stays `type`, `variable`, `function`, `enumerator`); unresolved
  target, namespace target, or template-id target → error.
- Specifiers → base type: keyword combinations per 3.9.1 (reuse the PA7
  table from `nsdecl_parser` if callable, else replicate); cv applies to
  the base; `constexpr` adds top-level const to object declarators only;
  `static extern thread_local inline virtual` ignored; an identifier or
  qualified name must resolve to Type/TypeAlias/template parameter (else
  error); `decltype(e)`: `id-expression` → declared type (enumerator → its
  enum), `parenthesized-expression{id-expression}` naming a variable →
  lvalue-reference to the declared type, naming an enumerator → the enum
  type; other operands → error.  Elaborated `class-forward-declaration`
  inside a specifier seq uses TypesOnly lookup and may find a class hidden
  by a function; not found → declare it in the nearest enclosing namespace
  or block scope.  Elaborated `enum E` must find an enumeration → else
  error.
- Declarator → type (8.3): suffixes right-to-left onto the base
  (`array-suffix` bound via `ConstEvaluator`; `parameter-clause`; `(void)`
  alone → empty; `parameter-pack ...` → varargs; parameter types are kept
  as declared — no array/function decay: `function of (array of 3 int)`),
  then `ptr-operator`s left-to-right with following `cv-qualifier`, then
  recurse into `nested-declarator`.  Pointer/array of reference, reference
  to reference, array of void/function, function returning array/function,
  bound ≤ 0 → error.  `function-qualifier` (noexcept/throw), attributes,
  `virt-specifier` ignored; `trailing-return-type` → error.
- Declarators: typedef → `type-alias`; function type → `function`; else
  `variable`.  A const/constexpr integral variable whose initializer
  evaluates records its value for later constant expressions (failure to
  evaluate only means "not a constant").  Function definitions bind the
  function in the scope named by the (possibly qualified) declarator-id,
  create `scope function <name>` under that scope, bind `parameter`s, and
  resolve parameter types in that scope; the body compound-statement is a
  `scope block`; nested compound statements nest; other statements are
  recursed only to find declarations and blocks.  Function declarations
  create no scope.
- Classes: definition → entity (reusing a forward declaration in the same
  scope), `type <name> <key> <name>`, `scope class <name>`; forward
  declaration → the type line only.  struct/class interchangeable; union
  vs non-union → error.  Members processed in order: simple declarations,
  member function definitions (function scope inside the class scope),
  nested classes/enums, typedef/alias, using declarations, static_assert;
  access specifiers ignored; special members and bit-fields → error.
  Anonymous class/enum inside a declaration takes the first declarator's
  name (`typedef struct {…} S;` → `type S struct S`, `type-alias S struct
  S`; `static struct {…} entries[2];` → `type entries struct entries`).
  Bare anonymous union at namespace scope: entity name
  `__anonymous_<key>_type__<first>_<last>` from the declaration's token
  extent (fixture: `__anonymous_union_type__0_10` for a 10-token
  declaration) and its members are also bound as `variable`s in the
  enclosing scope.
- Enums: unscoped definitions bind `type E enum E` then each `enumerator
  <n> enum E <v>` in the enclosing scope and print no scope; scoped enums
  bind the type and create `scope enum <name>` holding the enumerators
  (also for opaque `enum class E;`).  Opaque unscoped without an enum-base
  → error; opaque redeclaration with a different underlying type → error.
  A qualified enum name (`enum class writer::state : char {…}`) is a new
  entity in the current scope spelled with the joined name (`type
  writer::state enum class writer::state`, `scope enum writer::state`).
  Enumerator values print as decimal of the converted value.
- Templates: `template-declaration` creates `scope template-parameters`
  under the current scope, binds `type T typename T` per type-parameter
  and `type TT template-parameter TT` per template-template parameter (its
  inner clause is not a scope, so its names are invisible), ignores
  non-type parameters, then processes the declaration inside that scope
  (`type P struct P` and `scope class P` land there).  Template-ids in
  types → error.
- `sizeof(id-expression)` whose qualified/unqualified name resolves to a
  type is `sizeof(type)`.

## Failure Map

CP1 now passes its 35-fixture boundary and the stage reports 45/68.  The
remaining 23 failures are deferred by the checkpoint split.  By owning
boundary:

| group | owner | tests |
| --- | --- | --- |
| driver, scopes, namespaces, using, aliases, classes, declarator types, decltype, literal bounds | CP1 | 35 |
| enums, constant evaluation, sizeof/alignof, static_assert, anonymous union naming, qualified enum name (1 input fails PA10 parsing) | CP2 | 29 |
| template-parameter scopes, template-id rejection | CP3 | 4 |

## Performance Risks

- Lookup is O(scope depth × per-name index hits); using-directive closure
  uses a visited set, so mutually nominating namespaces terminate.  Output
  order comes from vectors, never from hash iteration.
- Types are interned once; spelling is rendered only when printing, cost
  linear in the dump.  No `SameType` structural recursion on hot paths.
- Constant expressions are evaluated once at their declaration; const
  variable values are cached on the binding, so 5000 chained enumerators
  (`B = A + 1`) stay linear.  No retry loops, no deferred worklists.
- Recursion depth follows AST depth (blocks, declarators, expressions),
  bounded by the PA10 parser's accepted depth.
- Probe evidence: 20000 declarations over 200 namespaces with qualified
  typedef lookups and a 10-deep block chain took 0.23 s / 57116 KB; the same
  input supplied twice as separate argv inputs took 0.41 s / 58972 KB, so
  time and peak memory remain linear and under the packet limits.

## Checkpoint Ledger

- CP1 core model + driver + namespaces/classes/functions/types — COMPLETE
  (45/68 stage tests; all 35 packet fixtures pass; through-pa10 is 589/589;
  file audit passes).
- CP2 enums, constant evaluation, sizeof/alignof, static_assert, parser
  extents and qualified enum names — ACTIVE (target 64/68).
- CP3 template-parameter scopes and template-id rejection — PENDING
  (target 68/68; `make test-report-through-pa11` clean).
- CP4 architecture cleanup, audit, performance evidence in `audit.md` —
  PENDING.

## Completed Checkpoint: CP1 — core scope/type model

The driver, type table, scope model, lookup, declarator-derived types,
declaration collection for namespaces/classes/functions/blocks, and printer
are complete. Enum specifiers, `static_assert`, templates and non-literal
constant expressions remain unsupported as planned for CP2/CP3. Evidence is
recorded in `audit.md`: the 35 CP1 fixtures pass, the stage is 45/68,
through-pa10 is 589/589, and the file audit passes. Expected-failure fixtures
remain counted only when the success fixtures of the same group pass.

### Implementation Packet

Files and symbols to create or change:

- `dev/src/sema/type_table.h/.cpp`: `TypeId`, `TypeKind`, `TypeNode`,
  `TypeTable` (`Fundamental`, `Cv`, `Pointer`, `Reference`, `Array`,
  `Function`, `Class`, `Enum`, `TemplateParam`), `Spell`, `SizeOf`,
  `AlignOf` (SizeOf/AlignOf may stay minimal until CP2).
- `dev/src/sema/scope_model.h/.cpp`: `ScopeKind`, `Scope`, `Binding`,
  `BindingKind`, `ClassEntity`, `EnumEntity` (shell), `SemaModel` (owns
  scopes, entities, the global scope), `LookupUnqualified`,
  `LookupQualified`, lookup filters.
- `dev/src/sema/const_eval.h/.cpp`: `ConstEvaluator` with `Evaluate`
  handling `literal` and `parenthesized-expression` now; all other node
  kinds throw until CP2.
- `dev/src/sema/type_builder.cpp` (declared in `scope_builder.h`):
  `BuildSpecifierType(seq, lookup_scope)`, `BuildDeclaratorType(decl,
  base, lookup_scope)`, parameter clause and `(void)` normalization.
- `dev/src/sema/scope_builder.h/.cpp`: `ScopeBuilder(tokens, arena,
  types, model)`, `Build(root)`, per-kind handlers for
  `AST_NAMESPACE_DEFINITION`, `AST_NAMESPACE_ALIAS_DEFINITION`,
  `AST_USING_DIRECTIVE`, `AST_USING_DECLARATION`, `AST_ALIAS_DECLARATION`,
  `AST_SIMPLE_DECLARATION`, `AST_FUNCTION_DEFINITION`,
  `AST_CLASS_SPECIFIER`, `AST_CLASS_FORWARD_DECLARATION`,
  `AST_LINKAGE_SPECIFICATION`, `AST_EMPTY_DECLARATION`,
  `AST_COMPOUND_STATEMENT` and statement recursion.
- `dev/src/sema/types_dump.cpp`: `PrintTypesUnit`.
- `dev/cppgm++.cpp`: replace the body of `run_emit_types_mode`.
- `dev/frontend_source_sets.mk`: extend `FRONTEND_OBJ_BASENAMES_cppgm++`.

AST facts to rely on (`dev/src/parser/ast_model.h`, `ast_parser.h`):
`decl-specifier` nodes are `make_token` for keywords and single
identifiers, `make_join` for qualified names (read tokens `[first,last)`),
`decltype(...)` nodes carry the operand as a child; `class-forward-
declaration NAME{class-key}` and `class-specifier NAME{class-key,
members…}` appear both bare and inside `decl-specifier-seq`; `declarator`
children are `ptr-operator`, `cv-qualifier`, `identifier` (span = tokens
of a possibly qualified declarator-id), `nested-declarator{declarator}`,
`parameter-clause{parameter-declaration{decl-specifier-seq, declarator?}
…, parameter-pack}`, `array-suffix{expr?}`, `function-qualifier`;
`namespace-definition NAME{inline?, decls}`, `using-directive{target}`,
`using-declaration{target}`, `namespace-alias-definition NAME{target}`,
`alias-declaration NAME{type-id{type-specifier-seq{type-specifier |
cv-qualifier | type-name | decltype-specifier}, abstract-declarator?}}`;
`function-definition{decl-specifier-seq, declarator, compound-statement}`;
`Pa6Token` literal fields `lit_type`, `lit_value`.

Fixture groups (35): spec `100-alias-and-function`, `100-bad-unknown-type`,
`100-class-scope`, `100-empty`, `100-global`, `100-namespace-alias`,
`100-namespace`, `100-qualified-type-lookup`, `100-using-declaration`,
`100-using-directive`, `200-class-key-compatible-redeclaration`,
`200-decltype`, `200-namespace-anonymous-class-array-object`,
`300-binding-after-namespace-bad`, `300-namespace-after-binding-bad`,
`300-namespace-alias-non-namespace-bad`; general `100-bad-using-target`,
`100-class-forward`, `100-function-pointer-void-parameter`,
`100-namespace-class`, `100-namespace-reopen`, `100-nested-class`,
`100-variadic-function-declaration`, `200-alias-qualified-class-lookup`,
`200-bad-pointer-to-reference-alias`, `200-class-qualified-lookup`,
`200-constexpr-object-vs-function-types`,
`200-elaborated-type-hidden-by-function`,
`200-inline-namespace-qualified-lookup`,
`200-namespace-alias-qualified-using-directive-target`,
`200-qualified-decltype`,
`200-qualified-namespace-function-definition-parameter-type`,
`200-void-parameter-normalization`, `300-block-zero-array-bound-bad`,
`300-noexcept-function-pointer-declarator`.

Required spec facts (N3485): 3.3.1 point of declaration is sequential;
3.3.6/3.3.7 namespace and class scopes; 3.3.10 a later object/function
hides a class name for ordinary lookup, elaborated lookup (3.4.4) ignores
non-types; 3.4.1 unqualified lookup walks enclosing scopes, using-directive
names visible in the nominating scope (7.3.4); 3.4.3p1 the name before
`::` ignores objects/functions/enumerators; 3.4.3.2p2 qualified namespace
lookup consults inline namespaces then using-directives of that namespace;
7.1.3 typedef/alias name denotes the same type; 7.1.5 constexpr object is
const; 7.1.6.2p4 `decltype(id)` is the declared type, `decltype((lvalue))`
is `T&`; 7.1.6.3p2 undeclared elaborated class declares in the enclosing
namespace/block; 7.3.1 reopening and inline namespaces (p8); 7.3.2 alias
must name a namespace; 7.3.3 using-declaration cannot name a template-id;
8.3.1/8.3.2 no pointer to or reference to reference; 8.3.4 array bound is
an integral constant > 0; 8.3.5 `(void)` is an empty parameter list, `...`
is varargs; 8.3p1 names in a qualified declarator's declarator are looked
up in the qualifier's scope.

Commands:

- Focused: `cd pa11 && make check TEST=tests/spec/100-global.t` (any single
  fixture); `make run INPUT=tests/spec/100-class-scope.t` prints the dump.
- Broad: `make test-pa11`, then `make test-report-through-pa11` (through
  pa10 must stay 589/589), `perl scripts/cppgm_file_audit.pl --stage pa11
  --paths dev/src` (limits: 3000 lines/source, 2400/header, 240/function).
- Performance probe: generate `/tmp/pa11_probe.t` with 200 namespaces each
  holding 100 typedef/variable declarations that qualify into earlier
  namespaces plus one function with 10-deep nested blocks; run
  `/usr/bin/time -f '%e s %M KB' ./dev/cppgm++ --emit-types -o /tmp/p.out
  /tmp/pa11_probe.t`, then double the input and confirm linear time and
  memory (< 1 s, < 100 MB).

Known uncertainties (decide by the listed default, note in `audit.md`):

- Class-key spelling at use sites after `struct C; class C {}`: default to
  the key of the latest declaration on the entity; binding lines use their
  own declaration's key.
- Unnamed namespaces (`namespace { }`): default `scope namespace
  <unnamed>` plus an implicit using-directive.
- Top-level cv on parameter types and function/array parameter decay:
  keep exactly as declared (fixture pins undecayed arrays).
- Block-scope function declarations and `extern "C"` blocks: bind in the
  current scope, no extra output.
- The bare anonymous union name needs the declaration's token extent,
  which the PA10 AST does not record; CP2 adds it (parser change) — CP1
  must not print such declarations and may throw on them.

## Active Checkpoint: CP2 — enums and constant evaluation

Implement the next coherent boundary in `dev/src/sema/const_eval.*`,
`scope_builder.*`, `type_builder.cpp`, and the minimal parser extent support
needed by the qualified-enum and anonymous-union fixtures. Add canonical enum
entities/types and enumerator bindings/scopes; extend constant evaluation for
literal, identifier, unary/binary, conditional, cast, `sizeof`, and `alignof`
expressions with short-circuiting and checked integer behavior; consume those
values for array bounds, enumerators, and `static_assert`. Preserve all CP1
behavior and reject malformed enum, reference, and bound cases with failure.
Progress proof: raise the stage to at least 64/68 while through-pa10 remains
589/589 and the file audit passes. Focused starting fixtures are
`tests/spec/200-const-int-static-assert.t`, `tests/spec/200-enum-scoped.t`,
and `tests/general/200-sizeof-qualified-type-idexpr.t`; template parameter
and template-id fixtures remain CP3.
