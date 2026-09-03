# PA16 Plan — cppgm++ --emit-lowir with the basic object model

Grading: `cppgm++ --emit-lowir -O0 -o x.my x.t`; exit status must match
`x.ref.exit_status` (19 fixtures expect EXIT_FAILURE, 224 expect EXIT_SUCCESS
plus a relaxed LowIR match).  243 fixtures: `tests/general` (228) and
`tests/spec` (15); no reference binary.  `pa16.gram` is byte-identical to
`pa15.gram`, so every syntax gap below is a parser gap, not a grammar change.
The harness links `dev/src/test_runner.cpp` and runs many inputs in one
process: no mutable static state.  Exit criterion: `make test-pa16` 243/243
and `make test-report-through-pa16` clean; through-pa15 is 1139/1139 today
and the PA11 `--emit-types` / PA12 `--emit-semantics` dumps are order- and
binding-sensitive, so class-model changes must not add printed bindings.

Relaxed compare (`scripts/compare_results_common.pl`): both texts are
validated (unique symbols and aliases, `tls_for` targets, one owner per
role, special-member order: a C2 definition must precede the C1 of the same
constructor and destructors go D2, D0, D1), metadata `linkage`, `binding`,
`object`, `tls_for`, `keep_alias`, `prefer_local`, `trivial_lifecycle`,
`effects`, `unwind`, `return`, `capture`, `access`, `alias`, `projection`
and `storage=readonly` are dropped, `alias object` lines are dropped,
function `@names` are paired (first by `object=` mangling, then same name and
signature, then unique body shape) and replaced by placeholders, and
top-level entries are sorted by kind then text.  Consequences: function
definition order is free, correct Itanium mangling is what makes pairing
robust, and everything else is exact: global `@names`, `$slots`, `%temps`,
`^labels`, `pass=`, `arity=`, `role=`, instruction order.

## Stage design

Data flow is unchanged from PA15: `dev/cppgm++.cpp` `run_emit_lowir_mode`
→ `Pa10Parser` → `ScopeBuilder(tokens, arena, model, tree)` → 
`ProgramLowering::AddUnit` per unit → `Finish` → `serialize_lowir_program`.
PA16 adds one owning boundary per layer; nothing is duplicated across layers.

1. Parser (`dev/src/parser/ast_parser_decl.cpp`, `ast_parser_expr.cpp`).
   Six accepted-by-grammar forms fail today: member `alignas(...)`
   (`parse_declaration(true)` attribute position), a qualified class-head
   (`struct B::D : B {}`, `struct alignas(T) object::table {}` in the
   class-specifier head), nested-qualified special-member definitions
   (`O::B::B(int) {}`: `parse_special_member_definition` compares
   `final_name` with the first component instead of the previous one), a
   qualified operator-function-id declarator (`bool B::operator!() const`),
   and pseudo-destructor / explicit destructor calls (`p->~I()`, `a.~A()`:
   postfix `.`/`->` followed by `~ identifier`, represented as an
   `AST_MEMBER_EXPRESSION` whose identifier text carries the `~`).  The AST
   kinds for class members already exist (`AST_SPECIAL_MEMBER_DEFINITION`,
   `AST_SPECIAL_MEMBER_DECLARATION`, `AST_CTOR_INITIALIZER`,
   `AST_MEM_INITIALIZER`, `AST_BIT_FIELD_DECLARATION`, `AST_BASE_CLAUSE`,
   `AST_ACCESS_SPECIFIER`, `AST_MEMBER_SPECIFIERS`).

2. Class model and layout (`dev/src/sema/scope_model.h`, `scope_builder.cpp`,
   `type_builder.cpp`, `type_table.cpp`).  `ClassEntity` becomes the single
   record of class facts: direct base (entity, access), fields in
   declaration order (binding, type, byte offset, bit offset/width, access,
   default-member-initializer `AstId`, anonymous-member class), size,
   alignment, requested alignment, constructors, destructor, inheriting
   constructor base, friends, hidden-friend functions, and the trivial
   default-ctor / trivial-dtor / aggregate flags.  `Binding` gains `access`
   and `is_static_member`.  One layout service `CompleteClassLayout(entity)`
   runs at the closing brace: base at 0, fields aligned to their natural
   alignment, bit-fields packed in units of the declared type without
   crossing a unit, zero-width bit-fields realign, anonymous struct/union
   members inline (union: max), empty class size 1, `alignas` on the class
   or a member, `#pragma pack`, size rounded to alignment; it publishes
   `TypeTable::SetClassLayout(entity, size, align)` so `SizeOf`/`AlignOf`
   answer `TYPE_CLASS` in O(1) with no SemaModel dependency.  Class scope
   declarations are accepted by `BuildNode`: special members (constructors
   named like the class, `~X`, `using Base::Base`), bit-fields, access
   specifiers (current access state per class), `static` members (a static
   member, not internal linkage), `friend` (declared in the innermost
   enclosing namespace, marked hidden so only ADL finds it), default member
   initializers (kept as `AstId`, analyzed per constructor), and out-of-class
   definitions through `ResolveDeclarationScope` (`YA::f`, `Outer::Buffer::
   Buffer`, `A::T* A::t`, member typedef return types).  9.2p2: member
   function bodies, default arguments and default member initializers are
   queued and analyzed when their class completes (the outermost-class rule
   for nested classes is still open; see the CP4b failure map).  `SemaModel::
   LookupMember(entity, name, filter, result)` is the one base-chain lookup
   (own scope, then base; derived names hide base names; using-declarations
   re-expose) used by member access, implicit-this names, qualified names and
   inherited typedefs.  Access checking (`CheckAccess(member, naming class,
   context scope)`: 11.2 with friends and protected-through-derived) is one
   function called from member access, qualified names, base conversions
   and type lookups; it lands with the initialization checkpoint.
   Review 1 fixed the ownership of two more facts: `ClassEntity::
   trivial_destructor` is computed with the layout beside
   `trivial_default_constructor` (no layer walks subobjects to ask), and a
   member binding reaches its `ClassField` through `Binding::field_index`
   / `SemaModel::FieldFor` (no `bit_field`/`bit_width` copies on the
   binding, no field scans).  `sema/class_completion.cpp` owns 9.2p2: a
   class body pushes every definition it contains (members, constructors,
   hidden friends, inherited constructors) on a pending stack, and
   completion pops that range in three passes: mem-initializers, bodies
   (a bodyless `= default` or inherited constructor gets an empty
   compound), default member initializers.  `EnsureDefaultConstructor` and
   `EnsureDestructor` synthesize subobject special members recursively, and
   completion ensures the ones a user-written constructor or destructor
   needs (12.6.2p8, 12.4p8), so lowering never asks for an entity sema did
   not create.  `ResolveConstructor` is the one public constructor
   selection and takes its candidates from the class scope's name index.
   Review 2 moved two more facts to their owners: `SemaModel::
   WalkUnqualified` searches a class level through `CollectClassMember`
   (own scope, then bases, 3.4.1p8) for value and type lookups alike, and
   `InjectedClassName` turns a constructor binding met by a type lookup
   into the class binding of its declaring scope (no innermost-class
   special case, no expression-level fallback).  Friendship lives on the
   befriended entity (`ClassEntity::friend_of`, `FunctionEntity::
   friend_of`, recorded by `ScopeBuilder::RecordFriend`) so
   `ContextCanAccess` asks only the classes that name the context; the
   implicit object argument of a member call is bound by
   `BindImplicitObject`, which checks viability but not the base path,
   because 11.2p5 checks the member in its naming class.  Review 3 added
   `ClassEntity::empty` (computed with the layout: no non-static data
   member in the class or any base) and made `ScopeBuilder::
   BuildFunctionBody` the one owner of a body's jump context (labels,
   gotos, switch entries, initialized locals), so a local class's member
   bodies set the enclosing function's state aside instead of clearing it.
   Review 4 named a qualified class-head's scope and type
   (`struct B::D : B {}`) by the last component under the prefix scope, so
   `NamespacePieces`, `QualifiedTypeName` and the ABI encoder read one
   typed chain (`_ZN1B1D1fEv`); no consumer splits a joined spelling.

3. Expression semantics (`dev/src/sema/expr_sema.cpp`, `overload.cpp`).
   `this` (keyword literal → prvalue `cv X*`), unqualified names inside
   non-static member functions that resolve to members (SEMA_MEMBER over an
   implicit `this`), `.`/`->` through `LookupMember`, static members through
   objects or `X::m`, member function calls with the implicit object
   argument: candidates from lookup, the object address passed as
   `OverloadArgument` 0 with type `cv X*` prvalue so the existing pointer
   qualification/derived-to-base ranking selects the cv-correct or base
   overload; `SelectBestOverload` gains an `implicit_object` parameter that
   non-static candidates consume and static candidates skip.  Constructor
   selection reuses the same path (`X x(args)`, `X x{args}`, `X x = v`,
   mem-initializers, placement `new`), yielding `SEMA_CONSTRUCTOR_ACTION`
   nodes as today.  Operator overloading: when an operand has class or enum
   type, the candidate set is member `operatorX` of the left operand's class
   ∪ unqualified lookup ∪ ADL (namespaces of the class, its bases and its
   enclosing class for hidden friends; no parent climbing; suppressed for
   member callees); the winner becomes an ordinary `SEMA_CALL`; when no
   user candidate is viable the built-in operator applies; a non-member
   operator needs one class/enum operand.  ADL also serves ordinary calls.
   Every named call, including the parameter-shaped forms CP7 reclassifies
   (`sizeof(derived::select(index()))`, `X x(f())`), resolves through
   `ResolveNamedCallee` and `FinishCall`; `BuildResolvedCall` is the
   operator-call tail.  A member expression naming a pointer-to-function
   data member is an indirect callee (`CallResolution::indirect_callee`,
   review 4), with or without parentheses; ordinary lookup finding a member
   function suppresses ADL (3.4.2p3, `suppress_adl`).  The four supported
   `__builtin_*` calls are a typed vocabulary: `BuiltinKindOf` classifies
   the spelling once at the call site into `FunctionEntity::builtin`
   (`BuiltinFunctionKind`), and the signature, the `cppgm_builtin_*` object
   symbol and the declaration's boundary metadata switch on the kind.

4. Lowering (`dev/src/lower/*`).  `LowInfoOf(TYPE_CLASS)` → `LK_OBJECT`
   (`obj<SxA>` slots); member access is `index i8 [projection=field] base,
   offset` after `index i8 [projection=base_subobject] base, 0` per
   inherited level; reference members use `projection=reference_field`;
   array members decay then `index T [projection=array_element]` (class
   elements: `binary mul i64 i, size` then `index i8`).  Methods take
   `%this : ptr` stored to `slot $this : ptr`; calls evaluate the object
   address first.  Symbols: `@Class__method`, nested `@Outer__Inner__f`,
   constructors `@X__X`, destructors `@X___X`, base-object entries
   `@X__X__base_entry` / `@X___X__base_entry`, `__ovN` through
   `TopLevelName`; `object=` uses the PA14 encoder with nested-name
   prefixes, `ABI_FUNCTION_RECORD_QUALIFIER` const, and
   `ABI_TERMINAL_SPECIAL` C1/C2/D1/D2 (`_ZN2YP3getEv`, `_ZNK6NumberixEi`,
   `_ZN2YPC1Ev`, `_ZN5Outer6BufferC2ENS_5TokenE`).  In-class member and
   friend definitions are `binding=weak`; out-of-class and namespace-scope
   definitions are `binding=strong`; `unwind=no` when noexcept.  Lifetime:
   every class-typed local emits `%t = addr $x` at its declaration, a
   constructor call uses that temp, a trivial implicit constructor emits no
   call and no helper; destruction is emitted inline at block end, before
   `return`/`break`/`continue`/`goto`, arrays element-wise (fixture order).
   Constructor bodies: `store ptr %this, $this`, base entry call through
   `base_subobject`, then fields in declaration order (mem-initializer >
   default member initializer > default constructor), then the body;
   destructor bodies: `eh_cleanup ^destructor_cleanup_N` when subobjects
   need destruction, body, `eh_end`, members in reverse order, base entry,
   `jump ^destructor_end_N`, the cleanup block repeats the calls with
   `eh_end` + `resume`.  `eh_try ^call_unwind_dispatch_N` regions appear only
   when a later subobject constructor may throw and an earlier subobject has
   a non-trivial destructor (3 fixtures pin the label spellings).  Helper
   emission is demand-driven by (function entity, complete|base variant) on
   a program-wide worklist in first-demand order; an emitted C1/D1 always
   adds `alias object <C2/D2> = @C1/D1`; a class constructed only as a base
   subobject gets only its `__base_entry` (C2/D2) definition; both variants
   are emitted when both are demanded (C2 first).  Namespace-scope objects:
   class globals are structured `{ zero N }` or constant items with padding
   `zero k` (a constructor whose body is empty and whose mem-initializers
   copy constants/parameters folds into data); constructor calls go to
   `@__cppgm_init` (created whenever a namespace-scope object has a
   constructor action, even trivial, so an empty init function appears) and
   destructor calls to `@__cppgm_fini [role=fini, binding=internal]` in
   reverse order; `thread_local` class objects get `@__cppgm_tls_guard__x`,
   `@__cppgm_tls_init__x` and `declare function @__cppgm_tls_wrapper__x()
   -> ptr [tls_for=@x]` (plus a wrapper for the guard); static data members
   are globals `@X__m` / `_ZN1X1mE`.  `dev/src/lowir_serialize.cpp` must
   learn `alias object` lines, `storage=thread_local` on globals, and the
   `eh_try` / `eh_cleanup` / `eh_end` / `resume` instructions.  Every
   object projection is formed by `LoadThis`, `ProjectField` and
   `ProjectArrayElement`, every lifetime call by `EmitVoidCall`; aggregate
   paths carry field indices.  A braced element of a non-aggregate class
   array at namespace scope is a constructor action: `FoldConstructorAction`
   folds it into data when the constructor's body is empty and each
   mem-initializer stores one parameter or constant (the fixture shape),
   otherwise `@__cppgm_init` constructs the element in place.  A symbol the
   unit only declares is declared in `BuildDeclarations` after every body
   has run, like a declaration-only function: `GlobalSymbol::referenced`
   is set by `GlobalFor`, and a constant static member with an in-class
   initializer that was only folded gets no `declare global`, while a
   thread-local one always does so its wrapper's `tls_for` target exists.
   `ZeroInitializeObject` owns zero-initialization of a value-initialized
   member (a scalar-width object is one integer store, a wider one is
   zeroed per base, member and element); a non-empty class passed by value
   is copied with `copyobj` through `EmitCopyObject` on both sides of the
   call, an empty one keeps the bare `$argobj__N` slot the fixtures pin;
   the serializer renders `copyobj` and `zeroinit`.  Object aliases are
   published after the emission walk, base-owned entities first.  Review 4:
   the thread-local, startup and shutdown bodies are built before the
   emission walk because they are demand roots outside the semantic call
   graph; a namespace-scope aggregate folds to constant data only when every
   omitted subobject is trivially constructible, otherwise the zeroed object
   is aggregate-initialized at startup by the same
   `LowerAggregateObjectInitializer` the locals use
   (`DynamicInitializer::aggregate_object`) and an omitted array element with
   a non-trivial default constructor is constructed in place
   (`default_construction`); every guarded subobject sequence (return
   cleanups, destructor suffix cleanups, member-array construction unwinds)
   shares `Lowerer::kInlineCleanupLimit`: below 32 the handler blocks repeat
   the fixture shape, at 32 they form one linked chain so the text stays
   linear; `ImplicitValueInitialization` is the one predicate for an
   expression-owned `T()` action, applied by `LowerVariable` (zero, call only
   a non-trivial constructor) and mirrored by the temporary-constructor
   demand walk (an expression temporary keeps its constructor call).

## Failure map at CP1 (218 failing, 25 passing; 19 fixtures expect EXIT_FAILURE)

| first error today | tests | owner | checkpoint |
| --- | --- | --- | --- |
| `parse failed` | 9 | parser gaps above | CP1 |
| `unsupported pa11 declaration` | 78 | class-scope declarations (special members, bit-fields, friends, statics, using, default member initializers) | CP1 accepts, CP2 lowers |
| `sizeof incomplete or non-object type` | 10 | layout service | CP1 |
| `non-scalar object type` 27, `member expressions` 3, `without a storage slot` 1 | 31 | object slots, projections, statics, methods | CP1 |
| `unsupported keyword literal` (`this`) | 11 | expression sema | CP1 |
| `no unique viable overload` 12, `wrong number of call arguments` 3, `duplicate function definition` 2, `unknown name` 7, `unknown class member` 4, `unknown type name` 7, `type or namespace used as expression` 2, `initializer conversion` 8, `unsupported declaration specifier` 2 (trailing return on members) | 47 | member lookup, implicit object overloads, inherited typedefs, ctor selection | CP1 / CP2 |
| operator errors (`invalid comparison operands` 4, `subscript base` 2, `called expression is not a function` 3, `ABI spelling of operator` 2, shift/bitwise/logical/additive/deref/postfix/assignment 8, `unsupported literal` 1) | 20 | operator overloading, ADL, functors | CP3 |
| `braced initializer` (array target 4, not an array 3), `unsupported expression` (placement new) 1, `unnamed type requires a declaration name` 1 | 9 | aggregate/value/placement initialization, anonymous members | CP4 |
| EXIT_SUCCESS where EXIT_FAILURE expected (`200-private-base-static-cast-bad`, `200-protected-member-typedef-access-bad`, `200-list-init-narrowing-bad`) | 3 | access control, narrowing | CP4 |

Regression watch-list: the 19 EXIT_FAILURE fixtures (`100-bad-member-
function`, `100-private-method-bad`, `200-bad-implicit-default-ctor-with-
nondefault-member`, `200-const-object-nonconst-member-call-bad`, `200-copy-
init-explicit-ctor-bad`, `200-copy-list-init-explicit-ctor-bad`, `200-default-
member-initializer-not-aggregate-bad`, `200-multilevel-qualification-
conversion-bad`, `200-private-member-not-aggregate-bad`, `200-qualified-
friend-not-hidden-adl-bad`, `300-nonmember-operator-requires-class-or-enum-
bad`, `300-under-aligned-class-bad`, `400-address-of-bit-field-bad`, `spec/300-
inherited-const-method-base-pointer-cv-bad`, the two access `-bad` tests and
`200-list-init-narrowing-bad`) pass today only because class code is
rejected wholesale; each checkpoint must re-run them and confirm the failure
comes from the intended check.

## Performance risks

- Layout is computed once per class at completion and stored; `SizeOf`
  must never walk members per query (nested classes would go quadratic).
- Member lookup walks the base chain once per name with `DirectBindings`;
  never scan a scope's binding vector to build candidate sets.
- ADL visits each associated namespace once per call (visited set);
  associated sets are bounded by the distinct argument class/enum types.
- Deferred member bodies are queued once and analyzed in one pass at class
  completion; default member initializers are analyzed per constructor
  (bounded by constructors × fields, both small).
- Destruction at exits: keep a live-object cleanup stack in the `Lowerer`
  (per scope, declaration order) so each exit emits its calls from the
  stack instead of re-walking the semantic tree; cost is exits × live objects.
- Helper emission: worklist keyed by (entity, variant), each emitted once.
- Probe (run after every checkpoint; generated inputs, time must scale
  linearly): a class with N fields and N methods each touching its own
  field; a single-inheritance chain of depth D whose last class reads the
  root field; one function declaring N locals with non-trivial destructors
  and N early returns.
  `perl -e '$n=shift; print "struct S {"; print "int f$_; int g$_() { return f$_; }\n" for 1..$n; print "}; int main(){ S s; return s.g1(); }\n"' 2000 > /tmp/wide.t`
  `perl -e '$n=shift; print "struct C0 { int v; };\n"; print "struct C$_ : C".($_-1)." { };\n" for 1..$n; print "int main(){ C$n c; return c.v; }\n"' 300 > /tmp/deep.t`
  `perl -e '$n=shift; print "int g; struct D { ~D(){ g = g + 1; } };\nint main(){"; print "D d$_; if (g == $_) return $_;\n" for 1..$n; print "return 0; }\n"' 400 > /tmp/exits.t`
  `for f in wide deep exits; do /usr/bin/time -f "$f %es %MKB" ./dev/cppgm++ --emit-lowir -O0 -o /dev/null /tmp/$f.t; done`

## Checkpoint ledger

| checkpoint | scope | proves progress by |
| --- | --- | --- |
| plan | this commit | stage design, failure map |
| CP1 class model, layout, members, methods (completed) | parser gaps; class-scope declarations accepted; layout and `sizeof`/`alignof`; `this`, member access, base-chain lookup, static members, implicit-object overloads; object slots, projections, method symbols/mangling, class globals as zero data | 55/243 pa16 tests pass (25/243 at start); through-pa15 remains 1139/1139; file audit passes |
| CP2 constructors, destructors, lifetime (completed) | user/implicit/inheriting ctors and dtors, mem-initializers, default member initializers, subobject plans, demand-driven C1/C2/D1/D2 with aliases, local and array lifetime at every exit, namespace-scope init/fini, thread_local family, EH cleanup shapes, serializer additions | 97/243 pa16 tests pass (146 failures, down from 188); through-pa15 remains 1139/1139; file audit passes |
| CP3 operator overloading and ADL (completed) | member/non-member/hidden-friend operators, ADL sets and source-point rules, built-in fallback, functors, subscript/call/increment operators, chained `<<`, enum operators, literal operator | 142/243 pa16 tests pass (101 failures, down from 146); focused operator/conversion set is 12/12; through-pa15 is 1139/1139; file audit passes |
| CP4a aggregate and bit-field initialization/lowering (completed) | local class aggregates, brace elision, nested arrays and string members, reference-member storage, complete-class deferred analysis, pack-state layout, anonymous aliases, and bit-field declaration/read/increment lowering | 149/243 pa16 tests pass (94 failures, down from 101) but eight CP3-passing fixtures regressed (hidden-friend bodies, defaulted constructor, global class array); through-pa15 is 1139/1139; file audit passes with five nonfatal warnings |
| review 1 (completed; `audit.md`) | CP4a regressions restored; recursive special-member synthesis; class-model ownership of destructor triviality and field records; bounded class completion; lowering projection helpers; global class-array elements | 157/243 (86 failures); no fixture that passed at CP3 or CP4a fails; through-pa15 1139/1139; file audit passes with four warnings; probes linear |
| CP4b.1 placement-new semantic/lowering (completed) | placement-new allocation overloads, placement arguments, class construction at the returned address, and aggregate-brace constructor calls | 159/243 (84 failures, down from 86); through-pa15 1139/1139; file audit passes |
| CP4b.2 copy/list diagnostics and access completion (completed) | explicit-constructor rejection, narrowing, anonymous storage, and the remaining private/protected/incomplete-reference paths | 169/243 pa16 tests pass (74 failures, down from 84); through-pa15 remains 1139/1139; file audit passes |
| CP5 audit and cleanup (completed) | static-member declaration identity, qualified/inherited lookup, global lvalue/address lowering, aggregate startup stores, and audit cleanup | 175/243 pa16 tests pass (68 failures, down from 74); packet 5/5; through-pa15 1139/1139; through-pa16 1314/1382; file audit passes with four pre-existing warnings |
| CP6 thread-local and static storage (completed) | canonical `Binding`/`GlobalSymbol` storage facts, TLS ABI wrapper and guarded initializer family, qualified private nested-type context, and use-sensitive constant declaration elision | 180/243 pa16 tests pass (63 failures, down from 68); packet 5/5; through-pa15 1139/1139; through-pa16 1319/1382; file audit passes with five warnings; specified scaling probe completed |
| review 2 (completed; `audit.md`) | CP4b.2 regression restored (using-declaration through a private base); unqualified lookup owns the base-chain search; friendship indexed by the befriended entity, bounded protected access; declaration-only globals emitted on demand; dead state removed | 185/243 (58 failures); no fixture that passed at review 1 or at the turn start fails; through-pa15 1139/1139; through-pa16 1324/1382; file audit passes with five warnings; probes linear |
| CP7 qualified and deferred member type contexts (completed) | declaration-scope trailing-return construction, injected-class-name and `decltype` qualified lookup, bounded ambiguous direct-initializer classification, deferred local reference binding, and derived-over-base same-signature member lookup | 192/243 pa16 tests pass (51 failures, down from 58); packet 7/7; through-pa15 1139/1139; through-pa16 1331/1382; file audit passes with five warnings |
| CP8 enclosing-scope destruction and incomplete types (completed) | qualified special-member ownership for deferred bodies, explicit and pseudo-destructor targets, base/complete ABI variants and aliases, reference-address initialization, incomplete class return declarations, and qualified nested-type mangling | 200/243 pa16 tests pass (43 failures, down from 51); packet 6/6 exact; through-pa15 1139/1139; file audit passes with five warnings; no fixture or reference changes |
| CP9 member initialization and layout paths (completed) | canonical aliased-base initializer matching, reference-member lvalue and address storage, qualified nested class definitions, retained `alignas` metadata, class/member alignment validation, discarded class glvalues, and bit-field aggregate stores | 213/243 pa16 tests pass (30 failures, down from 43) with unchanged coverage; packet 6/6 exact, extended focused set 7/7, and related alignment/layout set 6/6; through-pa15 1139/1139; file audit passes with five warnings; scaling probes remain linear; no fixture or reference changes |
| CP10 conversion and call-lowering paths (completed) | extern class declarations without lifetime actions, block-scope aggregate array elements through synthesized in-place constructors, reference-return lvalue/rvalue boundaries, assignment conversion ordering, and trivial four-byte value-initialization | 218/243 pa16 tests pass (25 failures, down from 30) with unchanged coverage; the three packet failures now pass exact comparison, as does the adjacent `Pair[]` constructor fixture; through-pa15 1139/1139; file audit passes with five warnings; no fixture or reference changes |
| review 3 (completed; `audit.md`) | wide value-initialized members zeroed; by-value class objects copied with `copyobj` (empty classes keep the fixture slot); one owner for a body's jump context; one named-call path; typed alias ordering; serializer writers shared; pseudo-destructor target by type | 218/243 (25 failures, the turn-start set; strict subset of review 2's 58); through-pa15 1139/1139; file audit passes with five warnings; probes linear |
| CP11 builtin, lookup, and common-reference boundaries (completed) | builtin declaration ownership and LowIR metadata; using-declaration overload sets; function/tag call disambiguation; derived/base conditional glvalue binding and address projection | 223/243 pa16 tests pass (20 failures, down from 25) with unchanged coverage; focused packet 6/6; through-pa15 1139/1139; file audit passes with five warnings; no fixture or reference changes |
| CP12 aggregate leaf evaluation and constant global data (completed) | supplied local aggregate values lower before destination projection, including member leaves; recursive transactional folding of constant class/array globals with layout padding; translation-unit string pooling by code-unit type and bytes | 227/243 pa16 tests pass (16 failures, down from 20) with unchanged coverage; focused aggregate/data set 4/4; through-pa15 1139/1139; file audit passes with five warnings; no fixture or reference changes |
| CP13 direct call resolution and nested LowIR naming (completed) | ordinary member lookup suppresses ADL; parenthesized member calls retain direct member resolution; nested class scope spellings use one LowIR component without changing ABI object names | 229/243 pa16 tests pass (14 failures, down from 16) with unchanged coverage; focused call set 3/3; through-pa15 1139/1139; file audit passes with five warnings; no fixture or reference changes |
| CP14 class member selection and lifetime initialization (completed) | const-qualified member projections with mutable/reference exceptions; recursive class completion through arrays; empty aggregate-constructor elision; synthesized array construction/unwind and reverse destruction; value-initialization zeroing | 231/243 pa16 tests pass (12 failures, down from 14) with unchanged coverage; packet 4/5 exact, with the remaining friend fixture retaining the standard-required base construction; through-pa15 1139/1139; file audit passes with five warnings; no fixture or reference changes |
| review 4 (completed; `audit.md`) | three review-3 regressions restored (function-pointer field callee, temporary-constructor demand); qualified class-head spelling owned by sema (correct ABI names); linked guard chains at `kInlineCleanupLimit`; omitted aggregate subobjects constructed, dynamic class aggregates initialized at startup; startup bodies built before the emission walk; builtin kinds typed | 234/243 (9 failures; strict subset of the turn-start 12 and of review 3's 25); through-pa15 1139/1139; file audit passes with five warnings; probes linear, quadratic array/member emission removed |
| CP16 conversion classification and temporary ownership (completed) | converting-constructor viability and copy-initialization actions; derived-pointer composite types and reference projection; source-ordered conversion temporaries without perturbing ordinary constructor/conditional slots | 242/243 pa16 tests pass (one failure, down from three) with unchanged coverage; focused conversion and recovery set passes; through-pa15 1139/1139; file audit passes with six warnings; no fixture or reference changes |
| CP17 defaulted-constructor emission boundary (completed) | recursive no-work proof for declaration-owned defaulted constructors; as-if elision of an empty complete wrapper; explicit retention of required non-trivial base ABI entries; bounded per-entity proof cache | 243/243 pa16 tests pass (the one remaining mismatch removed) with unchanged coverage; focused constructor plus five adjacent lifetime fixtures pass 6/6; through-pa15 1139/1139; file audit passes with six warnings; no fixture or reference changes |

CP1 evidence (2026-09-02): the semantic class model now owns direct bases,
fields, access/static metadata, layout size/alignment and member lookup; the
lowerer emits class object slots, field/base projections, implicit-`this` and
static calls, qualified/const member mangling, and zero-initialized class
globals. The focused member/layout loop passed. `make test-pa16` is 55/243
(188 failures, down from 218); the remaining failures are in the deferred
constructor/lifetime, inheritance completion, access-control, parser/layout
extensions, initialization, and operator groups. `make
test-report-through-pa15` is 1139/1139. The pa16 file audit passes with only
its three pre-existing header warnings. The required scaling probe measured
wide 0.06s/16420KB and doubled 0.13s/28452KB; deep 0.00s/6232KB and doubled
0.01s/7472KB; exits 0.01s/6836KB and doubled 0.02s/9644KB (the destructor
diagnostic is the planned CP2 boundary).

CP2 evidence (2026-09-02): special-member entities now own constructor and
destructor state, parameter defaults, mem-initializer targets, inherited
constructors, and detached default-member-initializer semantics. Lowering
emits canonical constructor/destructor variants and aliases, declaration-
ordered base/member initialization, aggregate and array subobject stores,
scope and return cleanup, EH cleanup edges, global init/fini, and the
serializer's EH instructions. The packet's constructor, default-member,
inheriting-constructor, global-init, array-lifecycle, and destructor-body
fixtures pass. The final `make test-pa16` run reports 97/243 (146 failures,
42 fewer than the CP2 start) with no coverage changes; `make test-report-
through-pa15` is 1139/1139. The pa16 file audit passes with four nonfatal
warnings. The specified probe measured wide 0.06s/17084KB and doubled
0.13s/29700KB; deep 0.00s/6556KB and doubled 0.01s/7800KB; exits
0.02s/10132KB and doubled 0.04s/15164KB after shared cleanup chains removed
the prior quadratic return-cleanup growth.

CP4a evidence (2026-09-02): aggregate analysis now walks class and array
subobjects with brace elision, direct class construction, defaulted elements,
reference-member binding, and character-array string initialization. The
complete-class semantic pass defers in-class bodies and constructor
initializers until later members exist; pragma-pack state is carried through
the canonical token stream into capped class layout, and aliases retain names
for anonymous class types. Bit-field declarations own width and allocation
metadata, while lowering performs masked reads, increments, and storage-unit
preserving writes. The final `make test-pa16` run is 149/243 (94 failures,
down from 101) with no coverage changes; the focused 15-test boundary set is
12/15, with the three remaining results being exact LowIR ordering/shape
comparisons in bit-field emission. `make test-report-through-pa15` is
1139/1139, and the pa16 file audit passes with five nonfatal warnings.

Review 1 evidence (2026-09-02, `audit.md`): diffing the CP4a failing set
against a rebuilt CP3 executable showed eight regressions hidden by the net
count; all pass again.  Nested implicit constructors and destructors are
synthesized recursively (a member's member destructor now runs; a member's
member constructor no longer aborts lowering).  `make test-pa16` is 157/243
(86 failures), `make test-report-through-pa15` 1139/1139, the file audit
passes with four warnings (the duplicate-block warning is resolved), and the
plan's probes plus `many 8000/16000` (0.35/0.71 s) and `chain 300/600`
(0.04/0.08 s) double per doubling.  Deliberate leftovers are listed in
`audit.md` finding 9.

Review 2 evidence (2026-09-02, `audit.md`): diffing the turn-start failing
set against a rebuilt review-1 executable showed one regression hidden by
the count (`300-private-base-using-method-call`); it passes again through
`BindImplicitObject`.  The unqualified scope walk now searches bases, which
also fixed `200-inherited-member-call-hides-outer-type` and three CP7
fixtures.  `make test-pa16` is 185/243 (58 failures), `make
test-report-through-pa15` 1139/1139, `make test-report-through-pa16`
1324/1382, the file audit passes with five warnings, and the plan's probes
plus `prot 8000/16000` (0.70/1.44 s) double per doubling.  Deliberate
leftovers are listed in `audit.md` review 2 finding 7.

Review 3 evidence (2026-09-03, `audit.md`): diffing the turn-start failing
set against a rebuilt review-2 executable showed no hidden regression (58
→ 25, a strict subset).  Probes found three material defects in the
reviewed checkpoints: a value-initialized member wider than eight bytes
was left uninitialized, a non-empty class passed by value arrived
uninitialized on both sides, and a `goto` before a local class with member
definitions lost its record because three copies of the goto-resolution
loop cleared the enclosing body's state.  `ZeroInitializeObject`,
`ClassEntity::empty` with `copyobj`, and `BuildFunctionBody` own those
facts now; CP7's second call path, CP8's alias sort and serializer copy,
and a spelling-based pseudo-destructor check were folded into their
owners.  `make test-pa16` is 218/243 with the identical failing set,
`make test-report-through-pa15` 1139/1139, the file audit passes with five
warnings, and the plan's probes double per doubling.  Deliberate leftovers
are listed in `audit.md` review 3 finding 10.

Review 4 evidence (2026-09-03, `audit.md`): diffing the turn-start failing
set against a rebuilt review-3 executable showed three regressions hidden
by the count (25 → 12): CP13's parenthesized-callee unwrap broke a
function-pointer field call and CP14's temporary-constructor filter left
the helpers of `F()(x)` and `Derived() - Derived()` undefined; all three
pass again.  Probes found four more material defects: a qualified class
head mangled with a doubled path (`_ZN1B1B1D1fEv`, hidden by the relaxed
compare), quadratic destructor-suffix and array-construction unwind
emission (a 1000-element member array took 13.95 s and 4.3 GB), constant
folding that zeroed omitted subobjects with user-provided default
constructors while a class aggregate with a dynamic leaf got no startup
code at all, and startup-body demand raised after the emission walk.  Sema
now names a qualified class head by its last component, the guard chains
share `kInlineCleanupLimit`, omitted subobjects and dynamic class aggregates
initialize at startup, the startup bodies precede the walk, and builtins
are a typed kind.  `make test-pa16` is 234/243 (9 failures, a strict
subset of the turn-start set), `make test-report-through-pa15` 1139/1139,
the file audit passes with five warnings (the aggregate analysis moved to
`expr_sema_aggregate.cpp`), and the plan's probes plus `array 1000/2000`
(0.08/0.17 s) and `members 1000/2000` (0.04/0.09 s) double per doubling.
Deliberate leftovers are listed in `audit.md` review 4 finding 10.

## Completed Checkpoint: CP1 — class model, layout, members, methods

Goal: every class-scope declaration in the suite is accepted and modeled,
complete classes have layout, member access and non-static / static member
calls resolve and lower, class objects occupy `obj<>` slots or zero data,
and method definitions are emitted with correct names and mangling.
Constructors and destructors are recorded as entities but not yet lowered
(default-initialized locals keep today's `SEMA_CONSTRUCTOR_ACTION` path;
a user constructor without a lowering path must still reject cleanly with
EXIT_FAILURE, never emit wrong LowIR).

### Implementation Packet

Files and symbols:

- `dev/src/parser/ast_parser_decl.cpp`: `parse_special_member_definition`
  (compare `final_name` with the previous qualifier component; keep the
  `AST_SPECIAL_MEMBER_DEFINITION` shape), class-specifier head with a
  qualified name and optional `alignas`, member-declaration `alignas(...)`
  (fold into `AST_DECL_SPECIFIER_SEQ` as an `AST_DECL_SPECIFIER` whose text
  is the `alignas` clause), declarator-id `X::operator!` (line ~789 handles
  only an unqualified `operator`).  `dev/src/parser/ast_parser_expr.cpp`
  (~464): postfix `.`/`->` `~name` member expression.  Existing PA10 AST
  dumps must not change (`make test-pa10`).
- `dev/src/sema/scope_model.h/.cpp`: `ClassEntity` fields listed in Stage
  design; `Binding::access`, `Binding::is_static_member`,
  `Binding::bit_width`; `SemaModel::LookupMember`; `FunctionEntity::
  special` (none, constructor, destructor) and `is_static_member`.
- `dev/src/sema/type_table.h/.cpp`: `SetClassLayout`, `ClassLayout`;
  `SizeOf`/`AlignOf` for `TYPE_CLASS` (AlignOf must stop deferring to
  SizeOf for classes and arrays of classes).
- `dev/src/sema/scope_builder.cpp`: `BuildNode` cases for
  `AST_SPECIAL_MEMBER_DEFINITION` / `_DECLARATION`, `AST_BIT_FIELD_
  DECLARATION`, `AST_ACCESS_SPECIFIER`, class-scope `AST_USING_DECLARATION`;
  `BuildClassDefinition` resolves the base clause, tracks access, queues
  member bodies, and calls `CompleteClassLayout`; `BuildSimpleDeclaration`
  / `BuildFunctionDefinition` handle `static` members (no `this`, no
  internal linkage), `friend`, out-of-class member definitions and
  `DeclareFunction` for constructors/destructors (`this` parameter, no
  result); `AddConstructorAction` stays for default-initialized locals.
- `dev/src/sema/expr_sema.cpp`: `AnalyzeName` (`this`, implicit members,
  `X::m`), `AnalyzeMember` (base chain, methods, statics, bit-field
  binding), `AnalyzeCall` (implicit object argument, static member
  functions, `Base::f()`), `AnalyzeSizeof`.  `dev/src/sema/overload.cpp`:
  `SelectBestOverload(..., implicit_object)`.
- `dev/src/lower/lowir_types.cpp`: `LowInfoOf(TYPE_CLASS)`, `AbiTypeOf`
  for class types (`ABI_TYPE_NAME_OR_REFERENCE` with the qualified name).
  `dev/src/lower/lowir_symbols.cpp`: `CollectSymbols` must reach the
  deferred member-function definition nodes (`DeferSemantic` /
  `EmitDeferredSemantics` decide where they attach), `MangleFunction` with
  class nested names, const qualifier and special terminals, `NameSymbols`
  base names `Class__method`, static data members as `GlobalSymbol`s,
  `BuildFunction` header binding (weak for in-class definitions).
  `dev/src/lower/lowir_expr.cpp`: `LowerLValue` for `SEMA_MEMBER`
  (field/base projections, `this` load), `LowerCall` for member calls.
  `dev/src/lower/lowir_program.cpp`: `LowerVariable` emits `addr $x` for
  class locals; `CollectSlots` types them `obj<SxA>`;
  `BuildGlobalDefinitions` emits class globals as `{ zero N }` and creates
  the (possibly empty) `@__cppgm_init` when a namespace-scope class object
  has a constructor action.

Fixture groups (focused loop, in this order): `100-empty-class-sizeof`,
`100-class-local-sizeof`, `100-large-class-local`, `100-self-pointer-layout`,
`100-global-class-zero`, `100-large-global-class-zero`, `300-bit-field-
layout-sizeof`, `300-zero-width-bit-field-layout`, `300-alignas-class-
layout`, `300-alignas-derived-base-layout`, `300-packed-class-layout`;
then `100-member-methods`, `100-out-of-class-methods`, `100-qualified-const-
method-definition`, `200-simple-class-member-object-access`, `200-base-field-
access`, `200-nested-class-member-object-access`, `100-this-arrow-member-
binds-lvalue-ref`, `200-mutable-member-const-method`, `200-method-cv-
overload-preference`, `200-member-call-implicit-this-cv-overload`,
`200-inherited-member-overload-set`, `100-static-member-object-access`,
`100-static-member-qualified-call`, `100-static-member-overload-skips-
nonstatic-this`, `200-static-thread-local-member`, `200-return-preserves-
value`, `200-nested-out-of-class-constructor-enclosing-type` (parser only;
its constructor lowering belongs to CP2).

Required spec facts: 9.2p2 (bodies, default arguments and default member
initializers are complete-class contexts); 9.2p13 later members at higher
addresses; Itanium layout (base at 0, member offset aligned to the member's
alignment, bit-fields allocated within units of the declared type, a
zero-width bit-field realigns to the next unit, size rounded up to
alignment, empty class size 1, `alignas` raises but may not lower
alignment: 7.6.2p5); 5.3.3 `sizeof` of a class includes padding; 3.4.5
member access lookup in the class of the object expression then its bases
(10.2 hiding); 9.4 static members have no implicit object and are named by
`X::m` or through any object; 13.3.1p2-4 implicit object parameter of a
non-static member is a reference to cv X and ranks by 13.3.3.2p3 (less
cv-qualified binding wins; derived-to-base is a conversion, identity beats
it); 13.3.1p4 static members in the same set ignore the object argument;
12.1 constructors have no return type and take the class's `this`; 9.3p3 a
const member function has `this` of type `const X*`.

Commands:

```sh
make -C dev cppgm++
cd pa16 && T=tests/general/100-member-methods.t && \
  CPPGM_APP_ARGS="--emit-lowir -O0" scripts/run_all_tests.pl ../dev/cppgm++ my $T && \
  CPPGM_APP_ARGS="--emit-lowir -O0" scripts/compare_results.pl ref my $T; cd ..
./dev/cppgm++ --emit-lowir -O0 -o /tmp/x.my pa16/tests/general/100-member-methods.t && \
  diff pa16/tests/general/100-member-methods.ref /tmp/x.my
make test-pa16                       # stage count; must only rise
make test-pa11 test-pa12             # after any scope_model/scope_builder change
make test-report-through-pa15        # broad; 1139/1139
perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src
```

Performance probe: the three generated inputs in Performance risks with
N=2000/D=300/N=400, then doubled; each timing must grow at most ~2.2×.

Known uncertainties:

- `#pragma pack` (`300-packed-class-layout`, `300-pragma-pack-followed-by-
  endif`): check how `preproc_engine` surfaces `#pragma` to the token
  stream; the layout service needs a pack stack keyed by token position.
- Deferred member bodies: `DeferSemantic` currently attaches member
  function definitions after the unit; confirm the PA12 dump order for the
  13 PA12 class fixtures is unchanged and that `CollectSymbols` sees them.
- Dump neutrality: `EnsureDefaultConstructor` adds a binding to the class
  scope; new synthesized entities must not become printed bindings in the
  PA11/PA12 dumps.  Run `make test-pa11 test-pa12` early.
- Static member functions today get `internal_linkage` from `KW_STATIC`
  and a `this` parameter; both must be conditional on the declaring scope.
- `200-member-function-default-arguments` / `200-method-cv-overload-
  preference` fail with `duplicate function definition`: verify
  `HasConstFunctionQualifier` on in-class definitions before assuming the
  canonical `this` type differs.
- By-value empty-class parameter (`%__param1 : obj<1x1>`, argument
  `$argobj__1`) appears in one fixture only; defer to CP4 if it costs more
  than a slot-typed direct operand.
- `400-signed-bit-field-read` masks with `binary and i32 %t, 7` and returns
  that value; the fixtures are the oracle, do not "correct" the sign.

## Completed Checkpoint: CP2 — constructors, destructors, lifetime

Goal achieved: lower explicit and synthesized construction/destruction consistently
for class subobjects, arrays, locals, globals, thread-local objects, and all
control-flow exits while preserving CP1's canonical class layout and member
lookup. Constructors and destructors produce the planned helper calls and
cleanup edges, or reject cleanly; they do not silently emit an uninitialized
or partially destroyed object.

### Delivered implementation packet

- `dev/src/parser/ast_parser_decl.cpp` and `dev/src/parser/ast_parser_expr.cpp`:
  retain the special-member AST shape while completing constructor,
  destructor, mem-initializer, explicit destructor-call, and qualified-member
  parsing needed by the lifetime fixtures.
- `dev/src/sema/scope_model.*` and `dev/src/sema/scope_builder.cpp`:
  model special-member declarations/definitions, default member initializers,
  mem-initializer targets, inherited constructors, deleted/defaulted state,
  and complete-class body/default-argument contexts.
- `dev/src/sema/expr_sema.cpp` and `dev/src/sema/overload.cpp`: resolve
  constructor actions and explicit destructor calls through the canonical
  class entities, including aliases, bases, and default arguments.
- `dev/src/lower/lowir_program.cpp`, `lowir_function.cpp`,
  `lowir_symbols.cpp`, and `lowir_expr.cpp`: build demand-driven C1/C2/D1/D2
  helpers, lower subobject/array construction, maintain a per-scope live
  object cleanup stack for every return/branch exit, and emit namespace/global
  and `thread_local` initialization/finalization.
- Focus first on `200-constructor-member-init`,
  `200-derived-base-constructor-member-init`,
  `200-destructor-body-local-before-base-destruction`,
  `200-member-object-lifetime`, `200-local-default-class-array-lifecycle`,
  `200-global-constructor`, `200-global-class-array-init`, and the
  inheriting-constructor fixtures. Keep the existing pa11–pa15 gate and
  file-audit checks as exit criteria; do not edit fixtures or `.ref` files.

## Completed Checkpoint: CP3 — operator overloading and ADL

Goal: resolve and lower member, non-member, hidden-friend, and built-in-fallback
operator expressions with source-point-correct ADL and overload ranking,
including functors, subscripting, increments, chained shifts, enum operators,
and user-defined literals.

### Implementation Packet

- `dev/src/sema/expr_sema.cpp`, `overload.cpp`, `scope_model.*`, and
  `scope_builder.cpp`: own operator-function lookup, ADL associated sets,
  hidden-friend visibility, implicit-object ranking, and source-point rules.
- `dev/src/lower/lowir_expr.cpp`, `lowir_program.cpp`, and
  `lowir_symbols.cpp`: lower selected operators, callable objects,
  subscript/increment forms, built-in fallback, and ABI operator spellings.
- Focus on the operator/ADL failures led by `300-basic-operator-overloads`,
  member prefix/postfix and subscript calls, chained shift stress tests,
  hidden-friend calls, enum operators, and the user-defined literal fixture.
  Preserve the pa11–pa16 gates and do not edit fixtures or `.ref` files.

Evidence (2026-09-02): operator expressions now share one canonical
candidate-specific ranking path for member and non-member functions;
associated namespaces/classes, hidden friends, and source-point lookup are
owned by `SemaModel`; derived-to-base and user-defined constructor conversions
are represented in the conversion metadata; callable objects, subscripts,
increments, chained shifts, enum operators, and string literal operators
lower through the selected function or built-in fallback. Qualified operator
declarators and ordinary identifiers beginning with `operator` retain their
separate parser/sema forms. The focused set passes 12/12, including the
`const volatile` member-overload witness. The final `make test-pa16` run is
142/243 (101 failures, 45 fewer than the 97/243 checkpoint start), with no
coverage changes; the exact through-PA15 command is 1139/1139 and the pa16
file audit passes with five nonfatal warnings.

## Completed Checkpoint: CP4a — aggregate and bit-field initialization/lowering

Goal: establish the canonical aggregate, reference-member, complete-class,
packing, anonymous-alias, and bit-field facts needed by the remaining
initialization and access rules while preserving the CP1–CP3 class, lifetime,
and operator paths. The final `make test-pa16` run is 149/243, through-PA15 is
1139/1139, and the pa16 file audit passes with five nonfatal warnings.

## Completed Checkpoint: CP4b.1 — placement-new semantic/lowering

Goal achieved: placement `new` now forms the allocation overload call from
the synthesized object size plus placement arguments, constructs class objects
at the returned pointer, and handles aggregate braced initialization through
the same canonical constructor-action path.  The aggregate constructor is
owned by the class model and lowered as a demand-driven special-member body,
so the returned storage and the object lifetime share one address.

Evidence (2026-09-02): the two focused placement-new fixtures pass, including
the aggregate-brace case and its synthesized constructor body.  The final
`make test-pa16` run is 159/243 (84 failures, down from 86) with no coverage
changes; `make test-report-through-pa15` is 1139/1139; and
`perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src` passes with
four nonfatal pre-existing header warnings.

## Completed Checkpoint: CP4b.2 — copy/list diagnostics and access completion

The semantic model now records declaring classes, friend relations, function
contexts, and explicit constructors.  Copy-list construction excludes
explicit constructors; direct-list and ordinary copy initialization retain
their distinct overload rules; list conversions diagnose floating/integral
narrowing; and access checks cover private/protected members, base paths,
nested classes, friends, and using-declarations.  Inherited field metadata is
lowered through one canonical non-virtual base displacement, including
transitive zero-offset bases.  Qualified type calls in block statements are
parsed as calls when the declaration form is not viable.

Evidence (2026-09-02): focused access, constructor, narrowing, nested-class,
friend, using-declaration, and inherited-member witnesses pass.  The required
`make test-pa16` run is 169/243 (74 failures, down from 84) with unchanged
coverage; `make test-report-through-pa15` is 1139/1139.  The pa16 file audit
passes with four pre-existing header warnings.  The specified probe measured
wide 0.06s/18008KB and doubled 0.13s/29668KB; deep 0.00s/6208KB and doubled
0.01s/6984KB; exits 0.02s/10128KB and doubled 0.04s/15528KB.

## Completed Checkpoint: CP5 — audit and cleanup

Goal achieved: static data-member declarations and out-of-class definitions
now retain one canonical storage fact, inherited qualified static calls use
the class member graph, and static member lvalues/addresses reach their
global symbols.  Static/non-static member functions with the same source
parameter list remain distinct by member signature and storage mode.  Dynamic
class-array leaves carry an aggregate path into startup lowering, while a
runtime class static aggregate is zero-initialized as one object before its
source-ordered leaf stores.  Redeclaration matching and the new static-member
matching helpers are isolated in `scope_builder_members.cpp`; the file audit
limits remain intact.

Evidence (2026-09-02): the five packet fixtures
`200-inherited-static-member-qualified-call`,
`200-static-nonstatic-same-pointer-signature`,
`300-static-class-member-object-definition`,
`300-static-const-member-address`, and
`300-static-member-aggregate-array-dynamic-init` pass 5/5.  The final
`make test-pa16` result is 175/243 (68 failures, down from 74) with unchanged
coverage; `make test-report-through-pa15` is 1139/1139, and
`make test-report-through-pa16` is 1314/1382 with only the current pa16
failures.  The pa16 file audit passes with four pre-existing header warnings.
The architecture findings and remaining failure ownership are recorded in
`audit.md`.

CP7 evidence (2026-09-02): type construction now carries the resolved
declaration scope through trailing return types, injected class names, and
`decltype`-qualified nested names.  The semantic path also bounds parser
ambiguity to the direct-initializer and `sizeof` type-id shapes it can prove,
then reuses normal overload selection and constructor/lifetime actions; class
member collection filters inherited same-signature candidates after direct
derived lookup.  The seven packet fixtures pass 7/7.  The final
`make test-pa16` run reports 192/243 (51 failures, down from 58) with unchanged
coverage; `make test-report-through-pa15` is 1139/1139,
`make test-report-through-pa16` is 1331/1382, and the pa16 file audit passes
with five nonfatal warnings.  No fixtures or reference files changed.

## Completed Checkpoint: CP7 — qualified and deferred member type contexts

Delivered: resolved the packet's remaining out-of-class and deferred member
type contexts through one scope-aware semantic path while preserving the
through-pa16 gate.

## Completed Checkpoint: CP8 — enclosing-scope destruction and incomplete types

Delivered: qualified special-member definitions now resolve their owning
class before parameter/body analysis, so nested out-of-class constructors and
namespace-scope destructor definitions retain canonical function scopes and
strong C1/D1 plus base C2/D2 variants.  Explicit destructor expressions share
the class destructor entity; scalar pseudo-destructors preserve evaluation as
a semantic no-op.  Reference globals lower dynamic initializers as addresses,
incomplete class function declarations use the pointer-sized declaration
boundary, and nested class/namespace names feed one qualified ABI spelling.

Evidence (2026-09-02): the six packet fixtures pass exact LowIR comparison:
`200-nested-out-of-class-constructor-enclosing-type`,
`300-const-pointer-explicit-destructor-call`,
`300-explicit-destructor-call-enclosing-namespace-type`,
`300-scalar-pseudo-destructor-call`,
`100-global-reference-incomplete-referent`, and
`100-incomplete-class-return-function-address`.  The required
`make test-pa16` result is 200/243 (43 failures, down from 51) with unchanged
coverage; `make test-report-through-pa15` is 1139/1139, and the pa16 file
audit passes with five nonfatal warnings.  The implementation is split across
the sema ownership path and its direct LowIR consumers; no fixtures or
reference files changed.

## Completed Checkpoint: CP9 — remaining member initialization and layout paths

Delivered: constructor initialization now resolves aliased direct bases and
functional-cast destinations through canonical semantic actions; reference
members expose their referents as lvalues while lowering preserves reference
slots; class definitions retain qualified names and `alignas` clauses without
changing the established PA10 AST dump; class/member layout separates
explicit alignment from `#pragma pack`, rejects weakening requests, and
rounds complete objects to the resulting alignment.  Discarded class glvalues
evaluate their designation, and fresh bit-field aggregate writes encode before
projecting the destination.

Evidence (2026-09-02): the six packet fixtures pass exact LowIR comparison;
the extended focused set passes 7/7, and the six related alignment/packing
fixtures pass 6/6.  The final
`make test-pa16` result is 213/243 (30 failures, down from 43) with unchanged
coverage; `make test-report-through-pa15` is 1139/1139, and the pa16 file
audit passes with five nonfatal warnings.  The wide/deep/early-exit probes
measured 0.06s/18432KB, 0.00s/6396KB, and 0.02s/10312KB respectively.  No
fixtures or reference files changed.

## Completed Checkpoint: CP10 — conversion and call-lowering paths

Delivered: declaration-only extern class objects no longer synthesize
construction/destruction actions or unused LowIR global declarations;
automatic aggregate-class array elements now retain their canonical
synthesized constructor actions and lower directly into existing element
storage; reference-returning calls preserve the referent address for lvalue
consumers and load only for rvalue consumers; simple-assignment conversions
run before left-lvalue lowering; and trivial synthesized value-initialization
handles one-, two-, four-, and eight-byte objects without naming omitted
constructors.

Evidence (2026-09-03): the three packet fixtures that failed at the start of
CP10 (`200-extern-class-object-declaration`,
`200-function-reference-return-expression-type`, and
`200-reference-indexed-pointer-member-access`) pass exact comparison, as does
the adjacent `200-local-struct-array-init` regression fixture.  The final
`make test-pa16` result is 218/243 (25 failures, down from 30) with unchanged
coverage; the required through-pa15 report is 1139/1139, and the pa16 file
audit passes with five nonfatal warnings.  No fixtures or reference files
changed.

## Completed Checkpoint: CP11 — builtin, lookup, and common-reference boundaries

Delivered: the four packet builtins are now canonical function entities
created on demand, collected as declarations, and serialized with their
pointer access/alias metadata, including the `cppgm_builtin_*` object names.
Using-declarations import the complete declaration set instead of only the
latest binding, while a visible function wins over a same-name tag during
call classification.  Conditional derived/base lvalue arms retain their
common-base glvalue category and LowIR projects each selected address to the
base subobject before storing it.

Evidence (2026-09-03): the three builtin metadata fixtures, the public/private
base using-declaration fixture, the function/tag collision regression, and the
derived/base conditional-reference fixture pass exact comparison (6/6).
The final `make test-pa16` result is 223/243 (20 failures, down from the
25-failure start) with unchanged coverage; `make test-report-through-pa15`
is 1139/1139, and `perl scripts/cppgm_file_audit.pl --stage pa16 --paths
dev/src` passes with five nonfatal warnings.  No fixtures or reference files
changed.

## Completed Checkpoint: CP12 — aggregate leaf evaluation and constant global data

Delivered: local aggregate lowering now evaluates each supplied leaf before
forming its destination projection, preserving source-order side effects for
pointer, scalar, class, bit-field, and deferred member leaves.  Global class
definitions use a transactional recursive constant flattener for nested
arrays/classes, retaining explicit scalar zeros and ABI padding while leaving
dynamic aggregates on the existing startup path.  Repeated string literals in
one translation unit share a global data object keyed by decoded code-unit type
and bytes.

Evidence (2026-09-03): the focused aggregate/data comparison for
`100-global-aggregate-nested-array-initializer`,
`200-member-pointer-const-typedef-return`,
`200-nonliteral-field-condition-not-folded`, and
`300-namespace-aggregate-array-string-members` passed 4/4.  The required
`make test-pa16` result is 227/243 (16 failures, down from the 20-failure
start) with unchanged coverage; `make test-report-through-pa15` is 1139/1139;
and `perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src` passes
with five nonfatal warnings.  No fixtures or reference files changed.

## Completed Checkpoint: CP13 — direct call resolution and nested LowIR naming

Delivered: ordinary unqualified calls now retain the ordinary member-function
set instead of adding associated-namespace candidates, and parenthesized member
callees resolve through the same direct-member path as unparenthesized calls.
Nested class scope names are flattened to their innermost LowIR component while
ABI object names continue to use canonical qualified scope data.

Evidence (2026-09-03): the focused comparison for
`200-implicit-member-call-suppresses-adl`,
`200-nested-class-private-enclosing-access`, and
`200-parenthesized-member-call` passed 3/3.  The required `make test-pa16`
result is 229/243 (14 failures, down from the 16-failure start) with unchanged
coverage; `make test-report-through-pa15` is 1139/1139; and
`perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src` passes with
five nonfatal warnings.  No fixtures or reference files changed.

## Completed Checkpoint: CP14 — class member selection and lifetime initialization

Delivered: member access now owns object cv propagation while preserving
mutable and reference-member semantics, and lvalue-to-rvalue conversion drops
only top-level cv before overload ranking.  Class completion follows
non-static array elements and skips static fields.  Aggregate lowering keeps
real constructor work, elides empty user-provided nested calls, value-initializes
synthetic/defaulted no-argument objects with zero stores, and emits array
construction/unwind plus reverse member destruction with cleanup suffixes.

Evidence (2026-09-03): the focused packet comparison passed 4/5; the remaining
friend/defaulted-constructor fixture differs because its checked reference
omits the derived constructor's required base call.  The adjacent const-member
overload and mutable-member checks pass.  `make test-pa16` is 231/243 (12
failures, down from 14) with unchanged coverage; `make
test-report-through-pa15` is 1139/1139; and the pa16 file audit passes with
five warnings.  No fixtures or reference files changed.

## Completed Checkpoint: CP15 — empty-base layout and hidden-friend definitions

Delivered: complete-class layout now applies the empty-base optimization while
reserving a same-type member's address, fixing the callable-field layout and
its derived projections.  Unnamed namespaces own internal linkage through the
semantic scope path and retain their ABI namespace component without adding a
second local-name encoding.  Hidden-friend demand walks retain inline friends
referenced by another friend while internal hidden definitions remain emitted;
internal trivial special members receive their complete/base entries and
trivial-lifecycle metadata.  LowIR represents `nullptr_t` as its 64-bit value
word, rejects aggregate classes as accidental scalar conversions, preserves
narrow representable literals at the literal boundary, and reprojects the
final destination of aggregate bit-field read-modify-write stores.

Evidence (2026-09-03): the focused packet comparison passed 6/7; the only
remaining mismatch is the queued defaulted-constructor fixture whose checked
reference omits the derived constructor's required base call.  The fixed
callable-field, unnamed-namespace, hidden-friend, function-pointer,
`nullptr_t`, and bit-field fixtures all pass exact comparison.  The required
`make test-pa16` result is 240/243 (three failures, down from 234/243) with
unchanged coverage; `make test-report-through-pa15` is 1139/1139; and
`perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src` passes with
six nonfatal warnings.  No fixtures or reference files changed.

## Completed Checkpoint: CP16 — conversion diagnostics and temporary ownership

Delivered: conversion classification now owns viable non-explicit converting
constructors in copy-initialization contexts, with a recursion guard around
overload ranking.  Reference binding records temporary materialization and
top-level qualification, while the semantic initializer creates the selected
constructor action.  Derived-pointer comparisons and conditional common types
share a model-aware composite-pointer path, and LowIR reserves only marked
conversion temporaries inside declaration-owned class construction so ordinary
constructor, aggregate, placement-new, friend, and conditional slot order is
unchanged.

Evidence (2026-09-03): both queued diagnostics pass exact comparison, and the
13 focused recovery fixtures covering aggregates, placement new, references,
and hidden friends also pass.  The defaulted-constructor comparison remains
the sole mismatch: its checked reference omits the derived constructor's
standard-required base call, so the implementation preserves that call and
does not alter the fixture.  `make test-pa16` is 242/243 (one failure, down
from three) with unchanged coverage; `make test-report-through-pa15` is
1139/1139; and `perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src`
passes with six nonfatal warnings.  No fixtures or reference files changed.

## Completed Checkpoint: CP17 — defaulted-constructor emission boundary

Delivered: LowIR now proves declaration-owned defaulted construction is
side-effect free from the completed semantic constructor graph before omitting
an empty complete wrapper.  The proof rejects default arguments, default
member initializers, initialized scalar/reference members, non-empty bodies,
and recursive or unavailable constructors; results are cached per function
entity.  Semantic base initialization remains intact, while each required
non-trivial direct base keeps its ABI base-entry demand even when the complete
wrapper is elided.

Evidence (2026-09-03): the focused
`200-friend-derived-private-base-defaulted-constructor` comparison passes, as
do five adjacent defaulted, aggregate, explicit-base, and inherited-constructor
fixtures.  `make test-pa16` is 243/243 with unchanged coverage;
`make test-report-through-pa15` is 1139/1139; and
`perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src` passes with six
nonfatal warnings.  No fixtures or reference files changed.

## Active Checkpoint: CP18 — block-scope extern class declaration ownership

Goal: close the recorded block-scope `extern` class declaration boundary while
keeping storage ownership and lifetime decisions canonical.

### Implementation Packet

- Start with the recorded block-scope `extern` class declaration gap and trace
  its binding, type completeness, and declaration-only LowIR ownership through
  sema and lowering.
- Preserve the PA17 non-goals: block-scope `static` objects and by-value copy
  semantics.
- Require a focused declaration comparison, `make test-pa16`, the
  through-pa15 report, and the pa16 file audit; preserve unchanged coverage.
