# PA16 Checkpoint Review — cppgm++ --emit-lowir with the basic object model

## Review 3 (2026-09-03): CP7–CP10

Scope: the four implementation checkpoints since review 2 (`8d71a7b5d`
qualified and deferred member types, `5cedae657` enclosing special-member
contexts, `95f01acef` member initialization and layout, `3ea514d42`
aggregate and reference conversion boundaries), read in full against the
README boundary, the plan's ownership and performance rules, spec.md's
one-owner and bounded-work requirements, and the fixtures they cite.
Method: the review-2 executable was rebuilt in a worktree and its pa16
failing set diffed against the turn-start set (58 → 25, a strict subset:
no regression hidden by the count); representative facts were traced from
their owner to the emitted text (the reclassified
`sizeof(derived::select(index()))` call, a qualified nested constructor
definition with its C1/C2 variants and alias, an explicit destructor call,
`alignas` on a class head, a member and a forward declaration, a reference
member read, a value-initialized trivial member, a reference-returning call
used as lvalue and rvalue, an in-place aggregate array element, and the
empty-class by-value parameter fixture); generated probes covered a `goto`
around a local class, a twelve-byte value-initialized member, a non-empty
class passed by value, and block-scope statics; the plan's scaling probes
were timed on the review build.

### Findings and changes

1. **Value-initialized members wider than eight bytes were left
   uninitialized (material, fixed).**  CP10 replaced the eight-byte-only
   zero store in `LowerMemberInitializer` with a 1/2/4/8 switch and then
   returned for every trivial default constructor, so `Q() : p() {}` with a
   twelve-byte `P` emitted a field projection and no store (before CP10 the
   same input failed cleanly).  `Lowerer::ZeroInitializeObject` now owns
   8.5p5 zero-initialization of one object: scalar widths keep the fixtures'
   single integer store, wider objects are zeroed per base, member and
   element with one store per bit-field allocation unit, and a synthesized
   non-trivial default constructor is zeroed before its call (8.5p7).
2. **By-value class arguments lost their value (material, fixed).**  CP8's
   `LowerClassArgument` materialized `$argobj__N` for the empty-class
   fixture and passed it without a copy, and `BuildFunctionVariant` stopped
   storing every class parameter, so a non-empty trivially copyable class
   arrived uninitialized on both sides.  The pre-CP8 spelling (`load
   obj<8x4>` / `store obj<8x4>`) was itself invalid LowIR: the validator's
   `load`/`store` take word types, and object copies are `copyobj`.  The
   class model now records `ClassEntity::empty` with the layout (no
   non-static data member in the class or any base); the empty case keeps
   the fixture shape, every other class parameter object is copied with
   `copyobj` (caller: source pointer → parameter object; callee: object
   value → slot) through `EmitCopyObject`.  The serializer learned
   `copyobj` and `zeroinit`, which the parser, the in-process validator and
   the CY86 backend already accepted.  Pass-by-value semantics stay PA17's
   layer; this is the trivial copy the IR defines, validated by the
   harness on the probe.
3. **Function-body jump state was cleared, not saved, around nested
   bodies (material, fixed).**  `labels_`, `gotos_`, `initialized_locals_`
   and `jump_sequence_` were reset by three copies of the same
   goto-resolution loop (`BuildFunctionDefinition`, `CompleteClassMembers`,
   and CP8's `BuildSpecialMember`, which also cleared them at declaration
   time).  A `goto` placed before a local class with any member definition
   lost its record ("label without a semantic ordinal"), and an initialized
   local declared before such a class was forgotten by the 6.7p3 check.
   `ScopeBuilder::BuildFunctionBody` is the one owner: it sets the
   enclosing body's state aside (including `switch_entries_`), builds and
   resolves the body, and restores it; the three sites call it.  The
   goto-past-initialization probe now fails with "jump bypasses variable
   initialization" and the goto-around-local-class probe compiles.
4. **A second call-resolution path (fixed).**  CP7's `AnalyzeNamedCall`
   re-implemented named-callee lookup, implicit-object detection, overload
   selection and call construction for the reclassified ambiguous forms.
   `ResolveNamedCallee` (shared with `ResolveCallCallee`) and `FinishCall`
   (the tail of `AnalyzeCall`, which now takes analyzed arguments) are the
   single path, so those forms get the same template, builtin, arity and
   default-argument treatment as ordinary calls.
5. **Alias ordering through a name set (fixed).**  CP8 sorted
   `object_aliases` after the emission walk with a `std::set<std::string>`
   of base-required symbol names.  Aliases are collected with their
   `FunctionEntityId` and published in two linear passes (base-owned
   first): typed identity, no sort, no string set; output unchanged.
6. **Duplicated serializer metadata writer (fixed).**
   `append_function_metadata` copied the bodies of
   `append_boundary_metadata` and `append_symbol_metadata`; the three now
   share `append_boundary_items` / `append_symbol_items`.  Output unchanged
   (through-pa15 1139/1139).
7. **Pseudo-destructor name compared by spelling (fixed).**  `p->~T()` on
   a class required `T` to equal the class scope's printed name.  The name
   is now looked up as a type in the object's class and then in the context
   (3.4.5p3) and compared by canonical type, so a typedef spelling names
   the class and a different class is still rejected.
8. **Dead branch (fixed).**  `CollectClassMember` kept an unreachable
   derived-hides-base return behind CP7's early return.
9. **Verified and kept.**  CP7's reclassification of parameter-shaped
   expressions (`FindAmbiguousDirectInitializer`,
   `AnalyzeAmbiguousParameter` / `AnalyzeAmbiguousTypeId`,
   `TryBuildAmbiguousReferenceDeclaration`) is bounded (per-declarator
   lookups, one retry per `sizeof` operand) and lives in sema because the
   parser's disambiguator cannot see inherited names; injected-class-name
   materialization and using-declaration hiding in `CollectClassMember`
   are per lookup and bounded by overload-set sizes.  CP8's out-of-class
   special members mark `base_required` so one definition owns both
   Itanium variants; `QualifiedTypeName` renders the ABI type name once per
   symbol at the PA14 adapter boundary (`AbiType::name` is that adapter's
   input); the incomplete-class return declaration is `-> void` as the
   fixture pins.  CP9's `alignas` clauses are canonicalized per declaration
   by `AlignmentSpecifiers` and validated in `CompleteClassLayout`, with
   `pack_alignment` and `requested_alignment` kept as separate facts; a
   reference member expression denotes its referent.  CP10's extern class
   declarations start no lifetime and are declared only when referenced;
   the reference-return load sits at the rvalue boundary; aggregate class
   array elements construct in place.
10. **Kept deliberately, recorded here.**  Block-scope `static` objects
    (scalar and class alike) are lowered as automatic slots re-initialized
    on every call: pre-PA16 behaviour with no fixture, and the CP10 array
    element path inherits it.  `AnalyzeSizeof` and
    `AnalyzeAmbiguousParameter` retry the expression reading from a caught
    exception (one per operand).  `BuildResolvedCall` remains the
    operator-call tail beside `FinishCall`.  The bit-field branch of
    `LowerAggregateObjectInitializer` duplicates its value computation to
    order the encode before the projection.

### Ownership after review

| fact | owner | consumers |
| --- | --- | --- |
| empty class (no non-static member through any base) | `ClassEntity::empty`, computed in `CompleteClassLayout` | `LowerClassArgument`, `BuildFunctionVariant` |
| zero-initialization of one object (8.5p5) | `Lowerer::ZeroInitializeObject` | `LowerMemberInitializer` |
| trivial object copy | `Lowerer::EmitCopyObject` → `copyobj` | by-value class parameters, both sides |
| a function body's jump context (labels, gotos, switch entries, initialized locals) | `ScopeBuilder::BuildFunctionBody` | `BuildFunctionDefinition`, `BuildSpecialMember`, `CompleteClassMembers` |
| named callee resolution and call completion | `ResolveNamedCallee`, `FinishCall` | `AnalyzeCall`, `AnalyzeNamedCall` (ambiguous forms) |
| object alias publication order | the emission walk (`pending_aliases`) | serializer |
| pseudo-destructor target | type lookup in the class, then the context; canonical type equality | `AnalyzeMember` |
| `copyobj` / `zeroinit` text | `lowir_serialize.cpp` beside the parser's `parse_bulk` | — |

### Validation

- `make test-pa16`: 218/243 (25 failures), the same set as the turn start
  and a strict subset of the review-2 set; no fixture that passed at
  review 2 or at the turn start fails.
- `make test-report-through-pa15`: 1139/1139.
- `perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src`: passes
  with the five pre-existing warnings.
- Probes: the twelve-byte value-initialized member and the eight-byte
  by-value class pass the harness's sanity validation and relaxed compare
  against their own output; the goto probes behave as in finding 3.
- Remaining failures by first error: `unknown name in expression` ×3 (the
  metadata-emission trio), `initializer conversion is not viable` ×2
  (`300-using-declaration-public-private-base-member`,
  `spec/200-conditional-derived-base-lvalue-reference`), `incompatible
  pointer comparison` (`spec/200-const-reference-binds-derived-pointer-
  prvalue`), `no unique viable function overload`
  (`200-implicit-member-call-suppresses-adl`), `no viable constructor`
  (`200-string-literal-does-not-convert-to-mutable-void-pointer`), `a
  static or function member lvalue` (`200-parenthesized-member-call`), one
  sanity failure (`200-nested-class-private-enclosing-access`: indirect
  call without a signature), and 15 LowIR shape mismatches (nested and
  namespace aggregate data, bit-field increments, callable field, hidden
  friend definitions, synthesized array member lifecycle, `nullptr_t`
  operator, value-initialized functional cast, const subobject call,
  member-pointer typedef return, non-literal field condition,
  function-pointer parameter shadowing, defaulted constructor through a
  friend).

### Performance evidence

Review build, wall seconds and peak RSS; the plan's probes.

| probe | N | N×2 |
| --- | --- | --- |
| wide (fields + methods) 2000 | 0.06 s / 17.9 MB | 0.12 s / 30.8 MB |
| deep (inheritance) 300 | 0.00 s / 6.2 MB | 0.01 s / 7.2 MB |
| exits (locals with destructors + returns) 400 | 0.02 s / 10.1 MB | 0.04 s / 15.1 MB |

Every probe doubles at most in time and memory per doubling.

## Review 2 (2026-09-02): CP4b.1–CP6

Scope: the four implementation checkpoints since review 1 (`25437f10b`
placement new, `5f224e380` copy-list and access, `041f12f07` static member
storage, `32ff02d5d` static and TLS storage), read in full against the
README boundary, the plan's ownership and performance rules, and the
fixtures they cite.  Method: the review-1 executable was rebuilt in a
worktree and its pa16 failing set diffed against the turn-start set rather
than compared by count; representative facts were traced from their owner
to the emitted text (a placement-new allocation and its synthesized
aggregate constructor, a protected member reached from a friend, a
using-declaration through a private base, a static member read, write and
address, a thread-local class member's wrapper and guarded initializer, a
constant static member folded without storage); the plan's probes and a
new protected-access probe were timed on the turn-start and review builds.

### Findings and changes

1. **CP4b.2 regressed a review-1 fixture (material, fixed).**  The count
   went 86 → 63 failures across the four checkpoints, but
   `300-private-base-using-method-call` passed at review 1 and failed at
   the turn start with "inaccessible base-class conversion": `d.f()` with
   `class Derived : private Base { public: using Base::f; }` converted the
   implicit object argument through `Initialize`, whose new base-path
   access check rejected the private base.  11.2p5 checks the member in
   the naming class, and the using-declaration makes `f` a public member of
   `Derived`; the object's conversion to the member's class is part of that
   access, not a separate one.  `BindImplicitObject` now binds the implicit
   object argument at both member-call sites (`BuildResolvedCall`,
   `AnalyzeCall`), checking only that the conversion is viable.  User
   expressions still go through `Initialize` and its check.
2. **Protected access scanned every class in the program (fixed).**
   `ContextCanAccess` implemented the friend-of-derived rule as a loop over
   `classes_` (all of them) with `IsDerivedFrom` and a linear `std::find`
   through each candidate's friend list, on every protected member use:
   protected uses × classes.  The friend relation was also stored on the
   granting class, the wrong side for a query that starts from the context.
   `ClassEntity::friend_of` and `FunctionEntity::friend_of` now hold the
   classes whose friend declarations name the entity (one owner,
   `ScopeBuilder::RecordFriend`); the check walks the context's enclosing
   classes and its function and asks each granting class whether it is the
   owner or, for protected access, derived from it.  Cost is bounded by
   the friend declarations naming the context.  `IsNestedClassOf`,
   `IsFriendClass` and `IsFriendFunction` are gone; nesting is covered
   because `ContextClasses` already lists every enclosing class.  The
   probe (N classes, N friend functions of a derived class each reading a
   protected base member) shows the old scan was cheap in practice: 8000
   0.70 s on both builds, 16000 1.48 s before and 1.44 s after; it is gone
   rather than deferred.
3. **Unqualified lookup did not search base classes; a fallback did
   (ownership, fixed).**  `WalkUnqualified` looked at a class scope's direct
   bindings only, so CP4b.2 bolted a fallback onto `AnalyzeName`: when the
   whole walk found nothing, search the innermost class's member graph.  A
   name that exists both in a base and in an enclosing namespace therefore
   resolved to the namespace (`200-inherited-member-call-hides-outer-type`:
   `f()` in `D::g` picked `enum class f` over `B::f`), and types were never
   covered.  The walk now owns 3.4.1p8: at a class level it calls
   `CollectClassMember` (own scope, then bases, with hiding) for both the
   value and the type walks.  A constructor binding found by a type lookup
   stands for the injected-class-name through `InjectedClassName`, which
   replaces the innermost-class special case in `LookupTypeName` and also
   covers a class named inside its own member bodies and a base named in a
   derived class.  The `AnalyzeName` fallback is removed.  This fixed the
   regression's neighbours as well: `200-inherited-base-typedefs-in-
   derived-members`, `200-out-of-line-member-inherited-typedef-body` and
   `300-using-base-same-signature-derived-preferred`, three of CP7's
   planned fixtures.
4. **Declaration-only constants were dropped after the fact through a
   shared name set (fixed).**  CP6 collected every declaration-only global
   into `declare global` lines up front, recorded uses in a
   `ProgramLowering::referenced_globals_` string set from a `const`
   `GlobalFor`, kept an unread `mutable GlobalSymbol::referenced` beside
   it, and ran `DropUnreferencedConstantDeclarations` over the program's
   declaration list to delete the unused constants again.  Declaration-only
   objects are now declared where declaration-only functions are:
   `BuildDeclarations`, after the unit's bodies have run, using the
   per-unit `GlobalSymbol::referenced` that a non-const `GlobalFor` sets.
   A constant static member with an in-class initializer is declared only
   when odr-used; a thread-local one is always declared so its wrapper's
   `tls_for` target exists.  `BuildGlobalDeclaration` and `GlobalMetadata`
   own the declaration and metadata shape.  Output on the suite is
   unchanged.
5. **Dead state (fixed).**  `ScopeBuilder::current_class_` was saved, set
   and restored around every class body and never read.  The class-element
   aggregate path of `BuildGlobalArrayDefinition` wrote
   `DynamicInitializer::byte_offset` beside the `aggregate_path` that
   `BuildGlobalInitializers` actually consumes.
6. **Verified and kept.**  Placement new (`AnalyzeNew`, `LowerNew`): the
   size operand is a synthesized `int` literal converted by the selected
   `operator new` parameter, placement arguments join the allocation
   overload set, a class object is constructed at the returned pointer, and
   a braced initializer of an aggregate becomes a synthesized constructor
   whose body stores each parameter to its field; the fixture pins exactly
   that constructor (`_ZN5tableC1Ej`), so the entity is the oracle's shape
   and not an invention.  `EnsureAggregateConstructor` reuses an existing
   synthesized constructor of the same type.  Copy-list initialization:
   explicit constructors are excluded only for copy forms, narrowing is
   diagnosed on floating→integral, wider→narrower floating, and
   non-constant or out-of-range integral→floating conversions; both are
   pinned by `-bad` fixtures.  Static members: the out-of-class definition
   recovers `Binding::static_member` and `thread_local_storage` from the
   class declaration by name and compatible type, the in-class declaration
   is the canonical global symbol, and `LowerLValue` / `GlobalAddress` read
   it through `GlobalFor`.  Thread-local storage: the wrapper declaration,
   guard object and guarded initializer are emitted once per TLS object
   with a definition; the `tls_for` target, the missing type on a
   thread-local class declaration and the `local_static_ctor_*` labels are
   the fixtures' spellings.
7. **Kept deliberately, recorded here.**  `TypeScopeForDeclaration` looks
   the decl-specifier of a qualified out-of-class member definition up in
   the member's class scope so a private nested type passes the access
   check; `BuildFunctionDefinition` has done the same for return types
   since CP1.  The standard looks the specifier up in the enclosing scope
   and only checks access as the member (11p7), so an unqualified class
   member name in that position is over-accepted; no fixture pins the
   rejection.  The qualifier is resolved twice per declaration (once for
   the type scope, once per declarator).  `TryBuildRuntimeClassAggregate`
   applies its "zero object plus a store per leaf" rule to class static
   arrays only; the only fixture with a dynamic aggregate leaf is a static
   member, so the namespace-scope branch that mixes constant items with
   startup stores stays as the pre-existing PA15 rule until a fixture
   decides.  `BuildVariable` recognises copy-list-initialization by the
   `=` token after the declarator because the parser does not keep the
   `=` in `AST_INITIALIZER`.

### Ownership after review

| fact | owner | consumers |
| --- | --- | --- |
| unqualified name in a member: class, bases, then enclosing scopes (3.4.1p8) | `SemaModel::WalkUnqualified` via `CollectClassMember` | `LookupSet`, `LookupUnqualified`, `LookupTypeName`, `LookupCallSet` |
| injected-class-name seen through a constructor binding | `SemaModel::InjectedClassName` | the type walk |
| friendship | `ClassEntity::friend_of`, `FunctionEntity::friend_of` (`ScopeBuilder::RecordFriend`) | `ContextCanAccess` |
| member and base accessibility | `SemaModel::IsAccessible`, `IsBaseAccessible` | name lookup filters, casts, `Initialize`, `LookupType`, using-declarations |
| implicit object argument binding | `ExpressionAnalyzer::BindImplicitObject` | `BuildResolvedCall`, `AnalyzeCall` |
| explicit constructors, copy-list exclusion, narrowing | `FunctionEntity::explicit_constructor`, `ResolveConstructor(copy_initialization)`, `IsNarrowingListInitialization` | `BuildVariable`, `BuildConstructorTemporary`, aggregate clauses |
| placement allocation and construction | `AnalyzeNew` → `SEMA_NEW_EXPRESSION`; `EnsureAggregateConstructor` | `LowerNew` |
| static member storage and TLS bits | `Binding::static_member`, `thread_local_storage` (class declaration; definition recovers them) | `CollectSymbols` → `GlobalSymbol` |
| declaration-only object symbols | `Lowerer::BuildDeclarations` after uses (`GlobalSymbol::referenced`) | serializer |
| TLS wrapper, guard, guarded initializer | `AddThreadLocalWrapperDeclaration`, `AddThreadLocalInitializer`, `BuildThreadLocalInitializers` | — |

### Validation

- `make test-pa16`: 185/243 (58 failures; 63 at the turn start, 86 at
  review 1).  The turn-start failing set is a strict superset of the
  current one; the review-1 regression passes; no fixture that passed at
  review 1 or at the turn start fails.
- `make test-report-through-pa15`: 1139/1139; `make test-report-through-
  pa16`: 1324/1382 with every failure in pa16.
- `perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src`: passes
  with five nonfatal warnings (four header-weight, one nesting depth in
  `BuildGlobalDefinitions`).
- Remaining failures by first error: 23 LowIR shape mismatches (bit-field
  reads and increments, alignas layouts, ADL source point, nested aggregate
  data, callable field, synthesized array lifecycle, nullptr_t operator,
  value-initialized functional cast), 7 parse gaps (qualified class heads
  and constructors, `alignas` positions, pseudo-destructor and explicit
  destructor calls), 4 scalar conversions, 3 `unknown name` in the
  metadata-emission trio, 3 `no viable constructor`, 3 initializer
  conversions, 2 trailing return types, 2 `sizeof` of an incomplete type,
  4 unknown type names (qualified injected-class-name, decltype of a
  nested type, local-class inherited call, static using-declaration), and
  singletons (out-of-class private nested return type, ADL suppression,
  aliased base mem-initializer, derived pointer comparison, under-aligned
  `-bad`, parenthesized member call, function reference return).

### Performance evidence

Review build, wall seconds and peak RSS; the plan's probes and the
protected-access probe (`prot N`: N classes, N friend functions of a
derived class each reading a protected base member).

| probe | N | N×2 |
| --- | --- | --- |
| wide (fields + methods) 2000 | 0.06 s / 17.7 MB | 0.13 s / 31.1 MB |
| deep (inheritance) 300 | 0.00 s / 6.2 MB | 0.01 s / 6.8 MB |
| exits (locals with destructors + returns) 400 | 0.02 s / 9.8 MB | 0.04 s / 15.1 MB |
| prot 8000 | 0.70 s / 140.3 MB | 1.44 s / 274.0 MB |

Every probe doubles in time and memory per doubling; the turn-start build
measured prot 8000/16000 at 0.70/1.48 s.

## CP6 (2026-09-02): canonical thread-local and static storage

The semantic `Binding::thread_local_storage` bit is now the canonical storage
fact for a static data member.  It is captured on both class-scope and
qualified out-of-class declarations, carried through redeclaration matching,
and transferred into `GlobalSymbol`.  The lowerer therefore emits one
consistent `storage=thread_local` global family, including the ABI object
wrapper, guard wrapper, guarded initializer function, and `tls_for` metadata.
TLS class construction and scalar initialization remain demand-driven and
bounded by the collected global symbols; ordinary globals keep the existing
program startup path.

The same ownership path also removes declaration-only constant static
objects only after lowering has recorded an actual `GlobalFor` use, which
keeps `100-static-member-object-access` as a local constant while preserving
address and lvalue users.  Qualified member declarations build their type in
the resolved class scope, so private nested types remain valid in permitted
out-of-class definitions.  LowIR serialization now preserves TLS storage
metadata on declarations and definitions.  No fixtures or reference files
were changed.

Evidence (2026-09-02): the five packet fixtures
`100-static-member-object-access`, `200-static-thread-local-member`,
`200-static-thread-local-member-object-call`,
`300-static-member-definition-private-nested-type`, and
`300-thread-local-synthetic-symbol-family-isolation` pass 5/5.  The final
`make test-pa16` result is 180/243 (63 failures, down from 68) with unchanged
coverage; `make test-report-through-pa15` is 1139/1139, and
`make test-report-through-pa16` is 1319/1382.  The pa16 file audit passes with
five nonfatal warnings.  The specified probe measured wide 0.06s/18348KB
and doubled 0.13s/29668KB; deep 0.00s/6372KB and doubled 0.01s/6924KB (the
baseline timing is below wall-clock resolution); exits 0.02s/10196KB and
doubled 0.04s/15528KB.

## CP5 (2026-09-02): static-member identity and aggregate startup stores

The implementation now keeps the static/non-static distinction on the
canonical semantic binding and function entity across declaration forms.  A
qualified out-of-class data definition recovers the prior class declaration's
`Binding::static_member` fact before field creation; class static objects are
not added to `ClassEntity::fields`, and their external definitions are the
only definitions collected for global storage.  Qualified lookup delegates to
the class member graph, so an inherited static function resolves to its
declaring class.  Member-function redeclaration matching compares the source
member signature and skips a prior function with the opposite static mode,
which keeps an implicit-object parameter from colliding with an explicit
pointer parameter.

LowIR symbol collection now admits class-scope static variables, while
`LowerLValue` and `GlobalAddress` use the canonical global symbol for static
member reads, writes, and addresses.  Aggregate dynamic initializers carry a
root type and index path; `BuildGlobalInitializers` projects that path through
the existing array/field helpers.  Class static arrays with a dynamic leaf
are emitted as one zero-initialized object and source-ordered startup stores,
so constant sibling fields do not accidentally bypass the runtime aggregate
state.  The declaration/redeclaration matching helpers live in
`scope_builder_members.cpp`, keeping `scope_builder.cpp` below the audit
limit without changing ownership semantics.

Validation: the five packet fixtures pass 5/5; `make test-pa16` reports
175/243 (68 failures, down from 74) with unchanged coverage;
`make test-report-through-pa15` reports 1139/1139; and
`make test-report-through-pa16` reports 1314/1382, with the remaining 68
failures confined to pa16.  The pa16 file audit passes with four pre-existing
header-weight warnings.  No fixtures or reference files were changed.  The
next focused ownership boundary is thread-local storage and the remaining
static-member lvalue/address cases, listed in `pa16/plan.md` as CP6.

## Review 1 (2026-09-02): CP1–CP4a

Scope: the four implementation checkpoints since the plan (`b0de88827`,
`30d1737df`, `e74637d5b`, `14487c1aa`), read against the README boundary,
`lowir.md`, and the plan's ownership and performance rules.  Method: each
checkpoint's diff was read in full; representative facts were traced from
their sema owner to the emitted text (a member access, a mem-initializer, a
synthesized constructor and destructor, a hidden friend, a class array at
namespace scope, a bit-field store); the CP3 executable was rebuilt in a
worktree so the CP4a failing set could be diffed against it rather than
counted; the plan's probes and two new ones (N classes with one method
each; a constructor chain of depth D) were timed on the review build.

### Findings and changes

1. **CP4a traded coverage (material, fixed).**  The count went 101 → 94
   failures, but eight fixtures that passed at CP3 failed at CP4a: six
   hidden-friend cases (`300-hidden-friend-definition-adl-call`,
   `300-friend-function-definition-skip`, `300-hidden-friend-operator-
   nullptr-compare`, `300-nested-enum-hidden-friend-bitmask-adl`,
   `300-prvalue-derived-base-friend-operator`, `spec/300-operator-lookup-
   ordinary-adl-union`), `100-defaulted-constructor-default-member-
   initializer`, and `200-global-class-array-init`.  CP4a deferred every
   function defined in a class body to class completion but the completion
   pass only built bodies whose `member_class` was the completing class, so
   a friend defined in the class never got a body ("a function without a
   body"), and a `= default` constructor (body 0) never got its empty
   compound.  Completion now processes every definition deferred while the
   body was open, and a bodyless definition gets the empty compound the
   pre-CP4a path produced.
2. **Braced elements of a non-aggregate class array rejected (fixed).**
   CP4a's aggregate rule in `AnalyzeBraced` threw "braced initializer
   requires an aggregate" for `Entry table[] = {{"a", 1}, {"b", 2}}`, which
   CP3 folded into constant data by positional field assignment (ignoring
   the constructor).  The array path now routes each clause through the one
   aggregate-clause rule, so such an element is a constructor action; the
   global-data builder folds an action whose constructor has an empty body
   and stores one parameter or constant per field (`FoldConstructorAction`,
   the shape the fixture pins, constructor definition included) and
   constructs any other element in place at startup.
3. **Implicit special members were not synthesized recursively (fixed).**
   `struct X { ~X(); }; struct M { X x; }; struct O { M m; }; O o;` emitted an
   `O::~O` whose body destroyed nothing: `EnsureDestructor(O)` never
   synthesized `M::~M`.  The constructor side aborted lowering ("a member
   without a default constructor") for the same shape with a user
   constructor on `X`.  A user-declared destructor or constructor on `O`
   had the same gap.  Sema now owns the rule: `EnsureDestructor` and
   `EnsureDefaultConstructor` synthesize their subobjects' special members
   first, and class completion ensures the subobject constructors a
   user-written constructor default-initializes (12.6.2p8; mem-initializers
   and default member initializers excluded) and the subobject destructors a
   user-written destructor runs (12.4p8).  Emission stays demand-driven.
4. **Destructor triviality computed in three places (fixed).**  Sema's
   `HasNontrivialDestructor`, lowering's `NeedsDestructor` and
   `HasSubobjectDestructors` were the same recursive walk over bases and
   fields, re-run on every query (every scope exit, every global, every
   subobject).  `ClassEntity::trivial_destructor` is now fixed with the
   layout beside `trivial_default_constructor`; both layers read it.
5. **Layout facts duplicated on `Binding` and looked up by scanning (fixed).**
   `Binding::bit_field`/`bit_width` copied `ClassField::bit_width`, and
   every member access, mem-initializer and aggregate leaf found its
   `ClassField` by scanning the class's field vector.  `Binding::field_index`
   and `SemaModel::FieldFor` make the record the single owner and the lookup
   O(1); aggregate paths carry field indices instead of byte offsets.
6. **Unbounded completion work (fixed).**  Each constructor's initializer
   and default-member passes scanned the unit's whole deferred-node list to
   find its function node, and each class scanned the whole deferred-body
   list (quadratic in classes; the list was never emptied).  Constructor
   resolution scanned every binding of the class scope against the plan's
   "never scan a scope's binding vector" rule.  The pending list is now a
   stack bounded by the open class bodies (watermark at body start, popped at
   completion, entries carry their function node and scope), and constructor
   candidates come from the name-indexed `DirectBindings` under the class
   name.  The checkpoint-named `scope_builder_cp4.cpp` is
   `sema/class_completion.cpp`, the module that owns 9.2p2 completion.
7. **Lowering duplication (fixed).**  Six inline copies of decay / mul /
   index element addressing, eight of load-`this` / index-field, two
   aggregate-member leaf walkers, and `LowerAggregateInitializer`, which was
   reachable only from itself.  `LoadThis`, `ProjectField`,
   `ProjectArrayElement`, `MemberLeafDestination` and `EmitVoidCall` form
   every projection and lifetime call now; the file audit's duplicate-block
   warning is gone and the lowering sources are 420 lines shorter with
   byte-identical output on the suite.
8. **Transient and dead state (fixed).**  Three maps keyed by the same
   temporary node (`temporary_slots_`, `temporary_addresses_`,
   `constructed_temporaries_`) are one `TemporaryObject`; the bit-field unit
   set was keyed by formatted strings and never reset between subobjects of
   one class type, so the second `B` in `struct A { B x; B y; }` read-modify-
   wrote uninitialized storage; it is keyed by `(scope, offset)` and reset
   on entering each class subobject.  `ClassEntity::constructor` was written
   in three places and never read.  `ResolveConstructorForExpression` only
   forwarded to the private `ResolveConstructor`; the latter is the public
   API.  `Pa6TokenCollector::append_token` copied every token twice.
9. **Kept deliberately, recorded here.**  The shared return-cleanup chain
   switches on at 32 return statements (`BuildFunctionVariant`); below it
   the fixture-pinned inline form is emitted, above it the chain, so output
   shape has a cliff that no fixture crosses.  `Pa6Token::pack_alignment`
   costs 8 bytes on every token of every stage for a fact only class heads
   read.  `LookupTypeName` special-cases the injected-class-name because the
   class scope binds constructors under the class name and holds no type
   binding for itself.  `IsDerivedFrom` and `FindBasePath` keep a visited
   vector (O(depth²)) for a single-inheritance graph that cannot cycle.
   CP2's reversed replay of deferred definitions in `EmitDeferredSemantics`
   was checked with a forward-order build at CP4a: identical 149/243 and
   identical failing set, so it was not load-bearing; the replay is back in
   declaration order and the `NameSymbols` comment no longer explains a
   reversal.

### Ownership after review

| fact | owner | consumers |
| --- | --- | --- |
| direct base, fields (offset, bit width, access, default initializer), size, alignment, `aggregate`, `trivial_default_constructor`, `trivial_destructor` | `CompleteClassLayout` into `ClassEntity` (`scope_builder.cpp`) | member lookup, initialization, lowering |
| a member binding's layout record | `Binding::field_index` → `SemaModel::FieldFor` | bit-field access, mem-initializers, aggregate leaves, global folding |
| special-member entities, mem-initializer targets, default member initializers, subobject constructor/destructor demand | `BuildSpecialMember`, `EnsureDefaultConstructor`, `EnsureDestructor`, `class_completion.cpp` | lowering reads `default_member_initializers` and `default_semantic_arguments`; never synthesizes |
| constructor selection | `ScopeBuilder::ResolveConstructor` (public) | declarations, temporaries, functional casts, aggregate clauses |
| 9.2p2 completion order: mem-initializers, bodies (members and hidden friends), default member initializers | `CompleteClassMembers` over the bounded pending stack | — |
| object projections and lifetime calls | `LoadThis`, `ProjectField`, `ProjectArrayElement`, `EmitVoidCall` (`lowir_expr.cpp`, `lowir_function.cpp`) | member access, constructors, destructors, aggregates, globals |
| class array elements at namespace scope | `FoldConstructorAction` / `DynamicInitializer::constructor_action` (`lowir_symbols.cpp`) | `BuildGlobalDefinitions`, `BuildGlobalInitializers` |

### Validation

- `make test-pa16`: 157/243 (86 failures; 94 at the review start, 101 at
  CP3).  No fixture that passed at CP3 or at CP4a fails; the eight CP4a
  regressions pass.  The turn-start failing set is a strict superset of the
  current one.
- `make test-report-through-pa15`: 1139/1139.
- `perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src`: passes
  with the four pre-existing header-weight warnings; the
  `lowir_program.cpp` duplicate-block warning is resolved.
- Remaining failures by first error: 24 LowIR shape mismatches (bit-field
  reads and increments, alignas layouts, thread-local symbol family, ADL
  source point, synthesized array member lifecycle, static member
  aggregates), 7 EXIT_FAILURE fixtures accepted (access control and
  narrowing), 7 parse gaps (qualified class heads, `alignas` positions,
  pseudo-destructor calls), 6 `unknown name`, 5 `initializer conversion`,
  4 static-member lvalues, 4 scalar conversions, 3 implicit-`this` outside a
  member, 9 unknown type names through inherited typedefs and nested
  names, and singletons.

### Performance evidence

Review build, wall seconds and peak RSS; the plan's three probes and two
review probes (`many N`: N classes each with one method; `chain D`: a
constructor and destructor chain of depth D).

| probe | N | N×2 |
| --- | --- | --- |
| wide (fields + methods) 2000 | 0.06 s / 18.1 MB | 0.13 s / 31.0 MB |
| deep (inheritance) 300 | 0.00 s / 6.7 MB | 0.01 s / 7.9 MB |
| exits (locals with destructors + returns) 400 | 0.02 s / 10.0 MB | 0.04 s / 15.4 MB |
| many (classes) 8000 | 0.35 s / 85.9 MB | 0.71 s / 167.3 MB |
| chain (constructors) 300 | 0.04 s / 14.6 MB | 0.08 s / 24.4 MB |

Every probe doubles in time and memory per doubling.  Before the review
`many` also measured linear at 8000 because the quadratic scan was 64M
cheap comparisons (~0.05 s); it is gone rather than deferred.

## Checkpoint ledger

| checkpoint | commit | outcome |
| --- | --- | --- |
| plan | `9d3952444` | stage design, failure map |
| CP1 class model, layout, members, methods | `b0de88827` | 55/243 |
| CP2 constructors, destructors, lifetime | `30d1737df` | 97/243 |
| CP3 operator overloading and ADL | `e74637d5b` | 142/243 |
| CP4a aggregates and bit-fields | `14487c1aa` | 149/243; regressed 8 CP3 fixtures while fixing 15 |
| review 1 | `925a9d275` | 157/243; through-pa15 1139/1139; findings 1–9 |
| CP4b.1 placement new | `25437f10b` | 159/243 |
| CP4b.2 copy-list and access | `5f224e380` | 169/243; regressed `300-private-base-using-method-call` while fixing 11 |
| CP5 static-member identity | `041f12f07` | 175/243 |
| CP6 thread-local and static storage | `32ff02d5d` | 180/243 |
| review 2 | `42b656245` | 185/243; through-pa15 1139/1139; findings 1–7 |
| CP7 qualified and deferred member type contexts | `8d71a7b5d` | 192/243 |
| CP8 enclosing-scope destruction and incomplete types | `5cedae657` | 200/243 |
| CP9 member initialization and layout paths | `95f01acef` | 213/243 |
| CP10 conversion and call-lowering paths | `3ea514d42` | 218/243 |
| review 3 (this section) | review commit | 218/243, same failing set; through-pa15 1139/1139; findings 1–10 |
