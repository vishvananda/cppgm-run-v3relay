# PA16 Plan — cppgm++ --emit-lowir with the basic object model

Final state (2026-09-03): every checkpoint is complete, the stage passes
243/243 and `make test-report-through-pa16` passes 1382/1382.  This file is
the compact record of the stage design, the ownership rules that hold at the
end, the performance rules and probes, and the commands; `audit.md` holds
the reviews, findings, performance evidence, validation and the checkpoint
ledger.

Grading: `cppgm++ --emit-lowir -O0 -o x.my x.t`; exit status must match
`x.ref.exit_status` (19 fixtures expect EXIT_FAILURE, 224 expect EXIT_SUCCESS
plus a relaxed LowIR match).  243 fixtures: `tests/general` (228) and
`tests/spec` (15); no reference binary.  `pa16.gram` is byte-identical to
`pa15.gram`, so every syntax gap was a parser gap, not a grammar change.
The harness links `dev/src/test_runner.cpp` and runs many inputs in one
process: no mutable static state, and no reference into a growable table may
be assumed to survive an insertion unless the table guarantees it (see
Ownership rules).  The PA11 `--emit-types` / PA12 `--emit-semantics` dumps
are order- and binding-sensitive, so class-model changes must not add
printed bindings.

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
`^labels`, `pass=`, `arity=`, `role=`, instruction order.  A probe is
validated the same way: copy its `.my` to `.ref` and run
`scripts/compare_results.pl ref my <dir>`; this catches undefined call
targets and malformed IR that a diff would not.

## Stage design

Data flow is unchanged from PA15: `dev/cppgm++.cpp` `run_emit_lowir_mode`
→ `Pa10Parser` → `ScopeBuilder(tokens, arena, model, tree)` →
`ProgramLowering::AddUnit` per unit → `Finish` → `serialize_lowir_program`.
PA16 adds no layer: the class model lives in the PA11 `SemaModel`, class
expressions in the PA12 `ExpressionAnalyzer`, and object lowering in the
PA15 `Lowerer`.

1. **Syntax** (`dev/src/parser/`).  Qualified class heads with `alignas`,
   member `alignas`, `X::operator!` declarator-ids, out-of-class special
   members with nested qualifiers, `p->~T()` / `a.~A()` member expressions
   whose identifier carries the `~`, `#pragma pack` state surfaced through
   the canonical token stream (`Pa6Token::pack_alignment`).  The PA10 AST
   dump is unchanged.

2. **Class model and layout** (`sema/scope_model.*`, `scope_builder.cpp`,
   `class_completion.cpp`, `type_table.*`).  `ClassEntity` is the single
   record of class facts: direct base (entity, access, offset), fields in
   declaration order (`ClassField`: binding, type, offset, bit offset/width,
   access, default-member-initializer `AstId`, mutable, anonymous-member
   class), size, alignment, `requested_alignment`, `pack_alignment`,
   constructors, destructor, inheriting-constructor base, hidden friends,
   `friend_of`, and the `aggregate`, `empty`, `trivial_default_constructor`
   and `trivial_destructor` flags.  `CompleteClassLayout` runs at the closing
   brace and is the one layout service (base at 0, empty bases at 0 with a
   same-type member reserving their size, natural alignment, bit-field
   units, zero-width realignment, anonymous members, `alignas` raises only,
   pack caps, size rounded up) and publishes `TypeTable::SetClassLayout` so
   `SizeOf`/`AlignOf` answer in O(1).  A member binding reaches its record
   through `Binding::field_index` / `SemaModel::FieldFor`.
   `class_completion.cpp` owns 9.2p2: every definition in a class body is
   pushed on a pending stack and completion pops that range in three passes
   (mem-initializers, bodies, default member initializers).
   `EnsureDefaultConstructor` / `EnsureDestructor` synthesize subobject
   special members recursively, and completion ensures the ones a
   user-written constructor or destructor needs (12.6.2p8, 12.4p8), so
   lowering never asks for an entity sema did not create.
   `ScopeBuilder::BuildFunctionBody` is the one owner of a body's jump
   context (labels, gotos, switch entries, initialized locals).
   `SemaModel::WalkUnqualified` searches a class level through
   `CollectClassMember` (own scope, then bases, 3.4.1p8);
   `InjectedClassName` turns a constructor binding met by a type lookup into
   the class binding.  Friendship is indexed on the befriended entity
   (`ClassEntity::friend_of`, `FunctionEntity::friend_of`, recorded by
   `RecordFriend`), so `ContextCanAccess` asks only the classes that name
   the context; `IsAccessible` / `IsBaseAccessible` are the access owners.
   A qualified class head (`struct B::D : B {}`) is named by its last
   component under `ResolveQualifierScope(prefix)`, so every consumer reads
   one typed scope chain.  `SemaModel::InUnnamedNamespace(scope)` is the one
   owner of the unnamed-namespace fact behind scope-given internal linkage
   and the `_GLOBAL__N_1` ABI component.  A block-scope `extern` variable
   declaration is linked by `LinkBlockScopeExtern` (3.5p6-7) to the visible
   namespace entity through `Binding::redeclared_binding`, or declares a new
   member of the innermost enclosing namespace; `Binding::extern_declaration`
   tells every layer that no lifetime starts.  Static data members keep
   `Binding::static_member` and `thread_local_storage` on the class
   declaration; an out-of-class definition recovers them.

3. **Expression semantics** (`sema/expr_sema*.cpp`, `overload.cpp`,
   `conversions.cpp`).  `this`, implicit members, `.`/`->` through
   `LookupMember`, `MemberAccessType` (object cv onto the member, mutable
   and reference members excepted), static members through objects or
   `X::m`.  Every named call resolves through `ResolveNamedCallee` and
   `FinishCall`; `BuildResolvedCall` is the operator-call tail; the implicit
   object argument is bound by `BindImplicitObject` (viability only, 11.2p5
   checks the member in its naming class).  A member expression naming a
   pointer-to-function data member is `CallResolution::indirect_callee`,
   with or without parentheses; ordinary lookup finding a member function
   suppresses ADL (3.4.2p3).  Operator overloading: member `operatorX` of
   the left class ∪ unqualified lookup ∪ ADL (`SemaModel` owns associated
   sets, hidden-friend visibility and source-point rules), one
   candidate-specific ranking (`SelectBestOverloadCandidates`), built-in
   fallback, a non-member operator needs a class or enum operand.
   Constructor selection is `ScopeBuilder::ResolveConstructor` (public) over
   `ConstructorCandidates` (`overload.cpp`: declared, not deleted, not
   explicit for copy-initialization), the same candidate owner that a
   user-defined conversion sequence uses: `Classify(model, …)` selects a
   converting constructor once (`SelectConvertingConstructor`, the source
   argument marked `OverloadArgument::standard_conversions_only` per
   13.3.3.1p4), records it in `ImplicitConversion::conversion_function`,
   and `Initialize` builds that constructor action without a second
   selection.  Placement `new` is `AnalyzeNew`; braced aggregates are
   `expr_sema_aggregate.cpp` (`EnsureAggregateConstructor` reuses a
   synthesized constructor of the same type).  Copy-list initialization
   excludes explicit constructors and diagnoses narrowing.  The four
   supported `__builtin_*` calls are a typed `BuiltinFunctionKind`.
   Derived-pointer composite types go through the model-aware
   `CompositePointer`.  Pseudo-destructor targets are compared by canonical
   type.

4. **Lowering** (`lower/*`).  `LowInfoOf(TYPE_CLASS)` → `obj<SxA>` slots;
   `nullptr_t` is the 64-bit value word.  Every object projection is formed
   by `LoadThis`, `ProjectField` and `ProjectArrayElement`, every lifetime
   call by `EmitVoidCall`; aggregate paths carry field indices.  Symbols:
   `@Class__method`, nested `@Outer__Inner__f`, constructors `@X__X`,
   destructors `@X___X`, base entries `@…__base_entry`; `object=` comes
   from the PA14 encoder over `NamespacePieces` / `QualifiedTypeName` (one
   scope walk), `ABI_TERMINAL_SPECIAL` C1/C2/D1/D2, unnamed namespaces as
   `_GLOBAL__N_1` without the local-name `L` encoding.  Helper emission is
   demand-driven by (function entity, complete|base variant) on a
   program-wide worklist; the thread-local, startup and shutdown bodies are
   demand roots built before the walk; an emitted C1/D1 publishes its
   C2/D2 alias after the walk, base-owned entities first.  A hidden friend's
   referenced inline friends are retained by the same bounded
   body-reference walk as ordinary inline callees; internal-linkage
   definitions are always emitted.  Lifetime: `LowerVariable` emits the
   declaration address, the recorded constructor action or
   `ZeroInitializeObject` for value-initialization, and registers the live
   object; an `extern_declaration` binding gets no slot and no lifetime and
   its uses reach the global through `CanonicalBinding`.  Destruction is
   emitted at block end and before every exit from the live-object stack;
   every guarded subobject sequence (return cleanups, destructor suffixes,
   member-array construction unwinds) shares `Lowerer::kInlineCleanupLimit`
   (32): below it the fixture shape, at it one linked chain.  A declaration-
   owned `= default` constructor is omitted only when
   `ConstructorHasNoWork` proves the whole plan empty (cached per entity);
   required base entries keep their demand.  Namespace-scope objects: class
   globals fold to constant data (`ConstantGlobalAggregate`,
   `FoldConstructorAction`) only when every omitted subobject is trivially
   constructible; otherwise `@__cppgm_init` aggregate-initializes or
   constructs in place, `@__cppgm_fini` destroys in reverse; `thread_local`
   objects get the wrapper, guard and guarded initializer family;
   declaration-only objects are declared in `BuildDeclarations` after the
   bodies have run (`GlobalSymbol::referenced`).  String literals are
   pooled per unit by `(code-unit type, bytes)`.  A non-empty class passed
   by value is copied with `copyobj` on both sides; the empty class keeps
   the bare `$argobj__N` slot the fixture pins.  Conversion temporaries are
   reserved up front only inside class-typed declaration initializers, so
   ordinary slot order is unchanged.

### Ownership rules that hold at the end

- One owner per fact: a fact classified in sema is read, never rebuilt,
  in lowering; a spelling is never joined and split.
- `TypeTable::nodes_`, `SemaTree::nodes_` and the five `SemaModel` entity
  tables are `std::deque`: a reference returned by `At`, `ScopeAt`,
  `BindingAt`, `ClassAt`, `EnumAt` or `FunctionAt` stays valid while later
  entries are added.  Builders and the lowerer hold such references across
  work that interns types or creates entities; a vector here is a
  use-after-free that only shows under a different heap layout.
- No mutable static state: recursion guards are typed arguments
  (`OverloadArgument::standard_conversions_only`), caches are per-unit
  members cleared in `Lowerer::Run`.
- Layout is computed once per class; member lookup walks the base chain
  once per name with `DirectBindings`; never scan a scope's binding vector.
- Deferred bodies are queued once and analyzed at completion; demand walks
  are bounded by definitions × referenced functions.

## Performance rules and probes

- `SizeOf` never walks members per query; ADL visits each associated
  namespace once per call; destruction at exits reads the live-object
  stack; helper emission is once per (entity, variant); guarded sequences
  are linear through `kInlineCleanupLimit`.
- Probes (generated inputs; each timing must grow at most ~2.2× per
  doubling; compare executables interleaved, medians of five):

```sh
perl -e '$n=shift; print "struct S {"; print "int f$_; int g$_() { return f$_; }\n" for 1..$n; print "}; int main(){ S s; return s.g1(); }\n"' 2000 > /tmp/wide.t
perl -e '$n=shift; print "struct C0 { int v; };\n"; print "struct C$_ : C".($_-1)." { };\n" for 1..$n; print "int main(){ C$n c; return c.v; }\n"' 300 > /tmp/deep.t
perl -e '$n=shift; print "int g; struct D { ~D(){ g = g + 1; } };\nint main(){"; print "D d$_; if (g == $_) return $_;\n" for 1..$n; print "return 0; }\n"' 400 > /tmp/exits.t
perl -e '$n=shift; print "int g; struct E { E() { g = g + 1; } ~E() { g = g - 1; } };\nstruct H { E items[$n]; };\nint main(){ H h; return g; }\n"' 1000 > /tmp/array.t
perl -e '$n=shift; print "int g; struct E { ~E() { g = g + 1; } };\nstruct H {"; print "E m$_;\n" for 1..$n; print "};\nint main(){ H h; return g; }\n"' 1000 > /tmp/members.t
for f in wide deep exits array members; do /usr/bin/time -f "$f %es %MKB" ./dev/cppgm++ --emit-lowir -O0 -o /dev/null /tmp/$f.t; done
```

## Commands

```sh
make -C dev cppgm++
cd pa16 && T=tests/general/100-member-methods.t && \
  CPPGM_APP_ARGS="--emit-lowir -O0" scripts/run_all_tests.pl ../dev/cppgm++ my $T && \
  CPPGM_APP_ARGS="--emit-lowir -O0" scripts/compare_results.pl ref my $T; cd ..
make test-pa16                       # 243/243
make test-pa11 test-pa12             # after any scope_model/scope_builder change
make test-report-through-pa16        # 1382/1382
perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src
cd pa16 && for t in tests/general/*.t tests/spec/*.t; do valgrind -q --error-exitcode=99 \
  ../dev/cppgm++ --emit-lowir -O0 -o /tmp/vg.my $t >/dev/null 2>&1 || echo "VG $t"; done
```

A harness failure that does not reproduce standalone is a memory error:
run the fixture under valgrind before reading code.

## Known leftovers (recorded, not fixture-pinned)

- Block-scope `static` objects are lowered as automatic slots re-initialized
  on every call (pre-PA16 behaviour).
- Omitted aggregate subobjects are default-constructed by lowering through
  `DefaultConstructor` rather than by a sema-recorded action; the aggregate
  list carries only the written clauses.
- A call expression looks its callee name up three times
  (`TryFunctionalCast`, `TryCallableObjectExpression`,
  `ResolveNamedCallee`), each bounded; `AnalyzeSizeof` and
  `AnalyzeAmbiguousParameter` retry from a caught exception once per
  operand.
- `TypeScopeForDeclaration` looks the decl-specifier of a qualified
  out-of-class member definition up in the member's class (11p7 would look
  in the enclosing scope and check access as the member).
- `BuildVariable` recognises copy-list-initialization by the `=` token
  because the parser does not keep it in `AST_INITIALIZER`.
- `ZeroInitializeObject` keeps two spellings by size; the bit-field branch
  of `LowerAggregateObjectInitializer` duplicates its value computation to
  order the encode before the projection.
- A block-scope `extern` that matches no visible entity creates a visible
  namespace binding (3.5p7 says the name is not introduced there).
- `AstArena` stays a vector: the parser completes before sema and sema never
  appends to it.

## Stage handoff

PA17 adds copy/move and by-value class transfer on the `copyobj` and
constructor-action paths; PA18 adds virtual dispatch on the class model's
base chain; PA19 adds template-backed overload participation on the
candidate-specific ranking.  Nothing in PA16 pre-empts those layers.
