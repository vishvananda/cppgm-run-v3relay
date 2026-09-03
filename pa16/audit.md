# PA16 Architecture Review — cppgm++ --emit-lowir with the basic object model

Final consolidated record (2026-09-03).  The stage design and the ownership
rules that hold at the end are in `plan.md`; this file records the final
review, the findings ledger of the four checkpoint reviews, the performance
evidence, the validation, and the checkpoint ledger.

## Final review (2026-09-03): CP15–CP17 and cleanup

Scope: the three implementation checkpoints since review 4 (`e2ab801bc`
empty bases and hidden-friend lowering, `16de2c1c0` conversion and pointer
binding ownership, `5cae1b7c9` defaulted-constructor elision), read in full
against the README boundary, the plan's ownership and performance rules,
and spec.md's one-owner, typed-vocabulary, bounded-work and measurement
requirements; then the whole stage as it stands.  Method: the turn-start
failure (`300-member-function-pointer-field-call`, `invalid type id` under
the harness but a clean pass standalone) was reproduced under valgrind
instead of by reading code; every fixture in the suite was then run under
valgrind; representative facts were traced from their typed owner to the
emitted text (a function-pointer field callee with and without parentheses
and through `.` and `->`, a converting constructor in a call argument, a
return, a reference local, a mem-initializer and a nested class parameter,
an unnamed-namespace class with constructor, destructor, method, hidden
friend, helper and global, a `= default` constructor over a user-provided
empty base and over a working member, and a block-scope `extern` object in
its found, later-defined, undefined, namespaced-class and no-linkage
forms); every probe output was validated through the harness's LowIR
validator and relaxed self-compare; the pre-cleanup executable
(`5cae1b7c9`, rebuilt in a worktree) and the final one were timed
interleaved on the plan's five probes, medians of five.

### Findings and changes

1. **Use-after-free through the growable entity tables (material, fixed).**
   `Lowerer::LowerCall` held `const TypeNode& type` across the lowering of
   the arguments and the callee; lowering a reference argument interned a
   pointer type (`AddressValue` → `TypeTable::Pointer`), the node vector
   reallocated, and the call's result type was read from freed memory.
   Standalone the freed block still held the old bytes; in the harness's
   single-process batch the heap differed and the garbage `TypeId` threw
   `invalid type id`.  The suite-wide valgrind sweep found the same class in
   `ScopeBuilder::DeclareFunction` (a `Binding&` across `AddBinding`,
   `100-out-of-class-methods` and `100-qualified-const-method-definition`),
   passing only by luck.  Sixty-eight `const TypeNode&` sites and many
   entity references hold such a reference across work that adds entries;
   that is the natural way to write this code, so the tables now guarantee
   it: `TypeTable::nodes_`, `SemaTree::nodes_` and the five `SemaModel`
   entity tables are `std::deque` (push_back and resize never move
   elements; O(1) indexing).  Representation-only: every suite output is
   unchanged (1382/1382), the valgrind sweep of all 243 fixtures is clean,
   timing is identical on every probe and peak RSS is lower on the wide
   probe (no doubling over-allocation).  The parser's `AstArena` keeps its
   vector: parsing completes before sema and sema never appends to it.
2. **Block-scope `extern` declarations were miscompiled (material,
   fixed; review 4 finding 10, the plan's CP18).**  `extern T t;` and
   `extern int n;` inside a function each received a fresh automatic slot
   (so every use read uninitialized storage) and a class object received a
   destructor call at scope exit.  Sema owns 3.5p6-7 now:
   `ScopeBuilder::LinkBlockScopeExtern` links the block binding through
   `Binding::redeclared_binding` to the variable a visible declaration in an
   enclosing block or the innermost enclosing namespace declares (a prior
   declaration without linkage is rejected), or declares a new member of
   that namespace.  Lowering reads the one fact: `CollectSymbols` treats an
   `extern_declaration` binding as its canonical global, `CollectSlots`
   gives it no slot, `LowerVariable` starts no lifetime, and every use
   reaches the global through `CanonicalBinding` as before.  Probes: the
   found object (`addr @t`, `load i32 @n`), objects defined later in the
   unit, an undefined one (`declare global @n3`), a namespaced class object
   read through a member call, and `int n = 1; { extern int n; }` rejected.
3. **Mutable static state in the conversion layer (fixed).**  CP16 guarded
   user-defined-conversion recursion with a `static thread_local` depth
   counter and an RAII guard.  The plan forbids mutable static state (the
   harness runs many inputs in one process), and 13.3.3.1p4 is a
   per-argument rule: `OverloadArgument::standard_conversions_only` marks
   the source argument of a constructor considered as a conversion, and
   `Classify(model, …, allow_user_defined)` honours it.  The
   two-user-conversion chain is still rejected.
4. **Constructor candidates collected in two layers, and the selected
   converting constructor selected twice (fixed).**  CP16's
   `HasConvertingConstructor` re-implemented `ResolveConstructor`'s candidate
   filter, and `Initialize` then re-ran `ResolveConstructor` for the
   constructor `Classify` had already chosen.  `ConstructorCandidates`
   (`overload.cpp`: declared, not deleted, not explicit for
   copy-initialization) is the one owner for both;
   `ImplicitConversion::conversion_function` records the selection and
   `BuildConstructorTemporary` takes it, so a conversion selects its
   constructor once.
5. **Unnamed-namespace fact walked by three copies (fixed).**  CP15 added
   `ScopeBuilder::HasInternalLinkage`, lowering's free `InUnnamedNamespace`,
   and a third inline scope walk in `QualifiedTypeName` beside
   `NamespacePieces`.  `SemaModel::InUnnamedNamespace` is the owner (five
   linkage sites, `GlobalObjectName`), and `QualifiedTypeName` reads
   `NamespacePieces`.  Five statement pairs CP15 had joined on one line and
   two dropped blank lines are back in the file's style.
6. **CP17's proof copied its recursion set per subobject (fixed).**
   `ConstructorHasNoWork` copied `active` into a fresh set for every base
   and member recursion; the set is passed through, each frame inserting and
   erasing its own entity.  The per-entity cache is unchanged.
7. **Verified and kept.**  CP15: empty bases at offset 0 with a same-type
   member reserving their size (`CompleteClassLayout`; the per-field base
   loop is O(fields) under single inheritance); hidden-friend demand
   (bounded by definitions × referenced functions) and always-emitted
   internal definitions; trivial special members carry `unwind=no` and
   `trivial_lifecycle`; `nullptr_t` as the 64-bit value word; a
   representable narrow literal retagged at the literal boundary
   (`FitsSmallIntegerLiteral`, computed values still convert); aggregates
   without constructors are not converting targets; the unnamed-namespace
   probe names every entity `_ZN12_GLOBAL__N_1…` with `binding=internal` and
   no local-name `L` encoding, and the constructor/destructor pairs keep
   their base entries.  CP16: converting-constructor sequences in a call
   argument, a return, a reference local (destroyed at scope exit), a
   mem-initializer and a nested class parameter validate through the
   harness; conversion temporaries are reserved up front only inside
   class-typed declaration initializers (`CollectSlots`) and otherwise
   created at first lowering (`LowerConstructorTemporary`), so ordinary
   slot order is unchanged; `CompositePointer(model, …)` is the one
   derived-pointer composite path for comparisons and conditionals.  CP17:
   the no-work proof is memoized per function entity
   (`constructor_no_work_cache_`, cleared per unit) and bounded by the
   class's bases and fields; an elided complete wrapper keeps every required
   base entry (`MarkElidedConstructorBaseVariants`); the probe elides
   `Derived() = default` over `Base() {}` and an empty defaulted member while
   keeping `_ZN4BaseC2Ev`, and a defaulted holder of a working member keeps
   its call chain.
8. **Kept deliberately, recorded in `plan.md` under Known leftovers.**
   Block-scope `static` objects as automatic slots; omitted aggregate
   subobjects constructed by lowering through `DefaultConstructor`; three
   callee-name lookups per call and the exception-driven retries;
   `TypeScopeForDeclaration` over-acceptance; `=` token detection in
   `BuildVariable`; two zeroing spellings; the duplicated bit-field value
   computation; the visible namespace binding of an unmatched block-scope
   `extern`; the `AstArena` vector.

### Ownership after review

| fact | owner | consumers |
| --- | --- | --- |
| reference stability of types, semantic nodes and entities | `std::deque` storage in `TypeTable`, `SemaTree`, `SemaModel` | every holder of an `At` / `…At` reference |
| a block-scope `extern` variable's entity (3.5p6-7) | `ScopeBuilder::LinkBlockScopeExtern` → `Binding::redeclared_binding`, `Binding::extern_declaration` | `CanonicalBinding` → `GlobalFor`; `CollectSymbols`, `CollectSlots`, `LowerVariable`, `BuildDeclarations` |
| standard-only argument (13.3.3.1p4) | `OverloadArgument::standard_conversions_only`, set by `SelectConvertingConstructor` | `SelectBestOverload`, `SelectBestOverloadCandidates` → `Classify(allow_user_defined)` |
| constructor candidate set for an initialization form | `ConstructorCandidates` | `ResolveConstructor`, `SelectConvertingConstructor` |
| the converting constructor a conversion selected | `ImplicitConversion::conversion_function` | `Initialize` → `BuildConstructorTemporary(selected)` |
| scope inside an unnamed namespace | `SemaModel::InUnnamedNamespace` | linkage in `BuildSimpleDeclaration`, `DeclareFunction`, synthesized special members; `GlobalObjectName` |
| enclosing namespace/class path of a symbol | `Lowerer::NamespacePieces` | `QualifiedTypeName`, `GlobalObjectName`, function names |

### Performance evidence

Executables: `head` = `5cae1b7c9` rebuilt in a worktree; `new` = this
commit.  Wall seconds and peak RSS, interleaved runs, medians of five.

| probe | head (s / MB) | new (s / MB) |
| --- | --- | --- |
| wide (fields + methods) 2000 | 0.06 / 18.1 | 0.06 / 16.4 |
| wide 4000 | 0.13 / 31.2 | 0.13 / 28.5 |
| deep (inheritance) 300 | 0.00 / 6.2 | 0.00 / 6.2 |
| deep 600 | 0.01 / 7.1 | 0.01 / 7.0 |
| exits (locals with destructors + returns) 400 | 0.02 / 9.9 | 0.02 / 10.1 |
| exits 800 | 0.04 / 15.1 | 0.04 / 15.2 |
| array (member array, ctor + dtor) 1000 | 0.07 / 31.6 | 0.07 / 31.9 |
| array 2000 | 0.14 / 58.2 | 0.14 / 58.3 |
| members (dtor each) 1000 | 0.04 / 17.2 | 0.04 / 17.3 |
| members 2000 | 0.07 / 31.6 | 0.07 / 31.6 |

Every probe doubles at most in time and memory per doubling on both
executables; the deque tables cost no measurable time and save the vector's
over-allocation on the widest input.

### Validation

- `make test-report-through-pa16`: 1382/1382 (the turn started at
  1381/1382 with `300-member-function-pointer-field-call` failing under the
  harness only).
- `make test-pa16`: 243/243; `make test-pa11`: 68/68; `make test-pa12`:
  166/166 (no dump changed).
- `perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src`: passes
  with six nonfatal warnings (four header-weight, one nesting depth in
  `BuildGlobalDefinitions`, `scope_builder.h` at the header-weight
  threshold).
- Valgrind over all 243 fixtures: no error on the final build (three
  fixtures reported invalid reads on the turn-start build).
- Thirteen generated probes (six converting-constructor contexts, the
  unnamed-namespace class, the defaulted-constructor pair, the four
  function-pointer field call forms, four block-scope `extern` forms)
  validate through the harness; the two ill-formed probes (a second
  user-defined conversion, an `extern` after a no-linkage declaration) are
  rejected with EXIT_FAILURE.
- No fixture or reference file changed.

## Findings ledger of reviews 1–4

Each review rebuilt the previous review's executable in a worktree and
diffed the failing set instead of counting it; each traced representative
facts to the emitted text and timed the plan's probes.  What each found and
where the fact lives now:

- **Review 1 (`925a9d275`, CP1–CP4a).**  Eight CP3-passing fixtures had
  regressed behind a falling count (friend bodies and `= default`
  constructors deferred but never completed): completion processes every
  definition deferred while a body was open.  Braced elements of a
  non-aggregate class array are constructor actions folded by
  `FoldConstructorAction`.  Implicit special members are synthesized
  recursively (`EnsureDestructor`, `EnsureDefaultConstructor`, 12.6.2p8 and
  12.4p8 in completion).  `ClassEntity::trivial_destructor` is fixed with
  the layout (three walkers removed).  `Binding::field_index` /
  `SemaModel::FieldFor` replace layout copies on bindings and field scans.
  Completion is a pending stack bounded by open bodies (two quadratic scans
  removed; `class_completion.cpp`).  `LoadThis`, `ProjectField`,
  `ProjectArrayElement`, `MemberLeafDestination`, `EmitVoidCall` replace
  sixteen inline copies.  Temporary state unified in `TemporaryObject`;
  bit-field unit tracking keyed by `(scope, offset)`.
- **Review 2 (`42b656245`, CP4b.1–CP6).**  `300-private-base-using-method-
  call` had regressed: `BindImplicitObject` binds the implicit object at
  both member-call sites, checking viability only (11.2p5).  Protected
  access no longer scans every class: friendship is indexed on the
  befriended entity (`friend_of`, `RecordFriend`) and `ContextCanAccess`
  asks only the classes naming the context.  `WalkUnqualified` owns
  3.4.1p8 through `CollectClassMember`; `InjectedClassName` replaces the
  innermost-class special case and the expression-level fallback.
  Declaration-only globals are declared in `BuildDeclarations` after uses
  (`GlobalSymbol::referenced`) instead of collected and deleted through a
  string set.  Dead `current_class_` and `byte_offset` state removed.
- **Review 3 (`a0f917238`, CP7–CP10).**  No hidden regression.
  `ZeroInitializeObject` owns 8.5p5 (members wider than eight bytes were
  left uninitialized).  `ClassEntity::empty` plus `copyobj` through
  `EmitCopyObject` (a non-empty class passed by value arrived
  uninitialized; the old `load obj` spelling was invalid IR).
  `BuildFunctionBody` owns a body's jump context (three copies cleared the
  enclosing body's labels and gotos).  `AnalyzeNamedCall` folded into
  `ResolveNamedCallee` / `FinishCall`.  Alias ordering by typed identity;
  serializer metadata writers shared; pseudo-destructor targets compared by
  canonical type; a dead branch removed.
- **Review 4 (`a267faf68`, CP11–CP14).**  Three regressions behind the
  count: the function-pointer field callee (`CallResolution::
  indirect_callee`) and the temporary-constructor demand
  (`ImplicitValueInitialization` is the one predicate).  A qualified class
  head was mangled with a doubled path (`_ZN1B1B1D1fEv`, hidden by the
  relaxed compare): sema names it by its last component under the prefix
  scope.  Destructor-suffix and array-construction unwinds were quadratic
  (a 1000-element member array took 13.95 s and 4.3 GB): every guarded
  sequence shares `kInlineCleanupLimit` and forms one linked chain at it.
  Constant folding zeroed omitted subobjects that needed construction and a
  class aggregate with a dynamic leaf got no startup code: an omitted
  subobject folds only when trivially constructible, otherwise
  `DynamicInitializer::aggregate_object` / `default_construction` at
  startup.  Startup, thread-local and shutdown bodies are demand roots built
  before the emission walk.  Builtins are a typed `BuiltinFunctionKind`;
  the default-constructor branch below sema in `LowerVariable` and the
  rendered string-pool key are gone.

## Checkpoint ledger

| checkpoint | commit | scope | outcome |
| --- | --- | --- | --- |
| plan | `9d3952444` | stage design, failure map | 25/243 at start |
| CP1 class model, layout, members, methods | `b0de88827` | class-scope declarations, layout, `this`, member access, static members, method symbols | 55/243 |
| CP2 constructors, destructors, lifetime | `30d1737df` | special members, mem-initializers, C1/C2/D1/D2 demand, exits, init/fini, TLS | 97/243 |
| CP3 operator overloading and ADL | `e74637d5b` | member/non-member/hidden-friend operators, ADL, built-in fallback, functors | 142/243 |
| CP4a aggregates and bit-fields | `14487c1aa` | aggregates, brace elision, pack state, anonymous members, bit-field lowering | 149/243; regressed 8 CP3 fixtures |
| review 1 | `925a9d275` | findings above | 157/243; through-pa15 1139/1139 |
| CP4b.1 placement new | `25437f10b` | allocation overloads, construction at the returned pointer | 159/243 |
| CP4b.2 copy-list and access | `5f224e380` | explicit constructors, narrowing, access completion | 169/243; regressed 1 fixture |
| CP5 static-member identity | `041f12f07` | static declaration identity, inherited qualified statics, aggregate startup stores | 175/243 |
| CP6 thread-local and static storage | `32ff02d5d` | canonical storage facts, TLS family, use-sensitive declarations | 180/243 |
| review 2 | `42b656245` | findings above | 185/243; through-pa16 1324/1382 |
| CP7 qualified and deferred member types | `8d71a7b5d` | trailing returns, injected names, ambiguous initializers | 192/243 |
| CP8 enclosing-scope destruction and incomplete types | `5cedae657` | qualified special members, explicit and pseudo-destructors, reference globals | 200/243 |
| CP9 member initialization and layout paths | `95f01acef` | aliased bases, reference members, `alignas`, bit-field aggregate stores | 213/243 |
| CP10 conversion and call-lowering paths | `3ea514d42` | extern class declarations, in-place array elements, reference returns | 218/243 |
| review 3 | `a0f917238` | findings above | 218/243, same set; through-pa15 1139/1139 |
| CP11 builtin, lookup and common-reference boundaries | `0ec81b58e` | builtin declarations, using-declaration sets, conditional glvalues | 223/243 |
| CP12 aggregate leaf evaluation and constant global data | `0a4d0ae66` | leaf-before-projection, transactional folding, string pooling | 227/243 |
| CP13 direct call resolution and nested LowIR naming | `b6f7a72cc` | ADL suppression, parenthesized member calls, nested names | 229/243; regressed 1 fixture |
| CP14 class member selection and lifetime initialization | `c6a5f196f` | member cv propagation, array completion, value-initialization | 231/243; regressed 2 fixtures |
| review 4 | `a267faf68` | findings above | 234/243; through-pa15 1139/1139 |
| CP15 empty-base layout and hidden-friend definitions | `e2ab801bc` | empty bases, unnamed namespaces, hidden-friend demand, `nullptr_t`, literals, bit-field stores | 240/243 |
| CP16 conversion diagnostics and temporary ownership | `16de2c1c0` | converting constructors, derived-pointer composites, conversion temporaries | 242/243 |
| CP17 defaulted-constructor emission boundary | `5cae1b7c9` | no-work proof, elided wrappers with retained base entries | 243/243; through-pa16 1381/1382 under the harness |
| final review and cleanup (this commit) | — | findings 1–6 above | 243/243; through-pa16 1382/1382; valgrind clean; probes linear |
