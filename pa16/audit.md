# PA16 Checkpoint Review — cppgm++ --emit-lowir with the basic object model

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
| review 1 (this section) | review commit | 157/243; through-pa15 1139/1139; findings 1–9 |
