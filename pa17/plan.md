# PA17 Plan — cppgm++ --emit-lowir with value semantics

State at planning (2026-09-03, commit `e4bcf16eb`): 42/228 fixtures pass
(`tests/general` 199, `tests/spec` 29); `make test-report-through-pa16`
passes 1382/1382; file audit clean.  121 fixtures fail on exit status
(112 expect success and we throw, 9 expect failure and we succeed), 65
fail the relaxed LowIR compare.  PA17 adds no grammar (`pa17.gram` is the
shared source grammar); every "parse failed" below is a `Pa10Parser` gap.

Grading: `cppgm++ --emit-lowir -O0 -o x.my x.t`; exit status must match
`x.ref.exit_status`, and a passing fixture must match `x.ref` under the
relaxed compare (`scripts/compare_results_common.pl`, rules recorded in
`pa16/plan.md`: metadata such as `unwind`, `binding`, `object` and
`projection` is dropped, function `@names` are paired by `object=` mangling
and replaced by placeholders, everything else is exact: `$slots`,
`%temps`, `^labels`, `pass=`, instruction order).  No reference binary.
The harness runs many inputs in one process: no mutable static state, and
only `std::deque`-backed tables (`TypeTable`, `SemaTree`, the `SemaModel`
entity tables) keep references valid across insertion.  PA11 `--emit-types`
and PA12 `--emit-semantics` dumps print every binding of a scope and every
deferred semantic node, so every new synthesized entity must be created on
demand (first odr-use), never at class completion.

## Stage design

Data flow is unchanged from PA16: `dev/cppgm++.cpp` `run_emit_lowir_mode` →
`Pa10Parser` → `ScopeBuilder` (+ `ExpressionAnalyzer`) → `ProgramLowering::
AddUnit` → `Finish` → `serialize_lowir_program`.  PA17 adds no layer; it adds
facts to the PA11 class model, selections to the PA12 expression layer, and
one value-boundary owner to the PA15 lowerer.

1. **Class value facts** (`sema/scope_model.h`, `scope_builder.cpp`
   `CompleteClassLayout`, `scope_builder_special.cpp`).  `ClassEntity` gains
   the special-member record: `copy_constructor`, `move_constructor`,
   `copy_assignment`, `move_assignment` (`FunctionEntityId`, 0 until
   declared), the `user_declared_*` facts read at declaration (12.8p2-3,
   p17, p19: a non-template constructor whose first parameter is `cv X&` /
   `cv X&&` with the rest defaulted; `operator=` with one parameter of type
   `X`, `cv X&` / `cv X&&`), and `trivially_copyable` (3.9p9: no
   user-provided copy/move constructor or assignment, at least one not
   deleted, trivial destructor, every base and non-static member trivially
   copyable; reference members and empty classes qualify).  Triviality is
   fixed at the closing brace; the deletion state of an implicit member
   (12.8p11, p23) is decided when it is declared, memoized on the entity.
   `FunctionEntity` gains `copy_constructor` / `move_constructor` /
   `copy_assignment` / `move_assignment` flags and `SpecialMemberKind`
   gains `SPECIAL_MEMBER_CONVERSION`; `TypeNode` (TYPE_FUNCTION) gains the
   ref-qualifier as part of the interned function key (8.3.5p6).
   Implicit copy/move members are declared by `ScopeBuilder::
   EnsureCopyMoveMembers(entity)` on the first demand (a constructor
   resolution with a class-typed argument, a class assignment, a defaulted
   member's subobject), in the canonical order default, copy, move, so the
   `__ovN` ordinal in `ClassEntity::constructors` is stable regardless of
   demand order; bodies (detached `SEMA_FUNCTION_DEFINITION` nodes, the
   `EnsureDefaultConstructor` pattern) are built only at odr-use.

2. **Selection** (`sema/expr_sema.cpp`, `overload.cpp`, `conversions.cpp`,
   `scope_builder.cpp`).  `ResolveConstructor` / `ConstructorCandidates` see
   the implicit members and keep deleted constructors as candidates that
   diagnose when selected (8.4.3p2).  Copy-initialization from a class
   glvalue selects the copy constructor, from an xvalue the move
   constructor; `return local;` performs the 12.8p32 two-stage selection
   (rvalue first).  A class prvalue used to initialize an object of the same
   type is constructed in place (12.8p31: no constructor-action wrapper).
   Class assignment is an operator call on the (implicit or user)
   `operator=` through `TryOperatorCall`.  A conversion function is a
   `SPECIAL_MEMBER_CONVERSION` member entity that `Classify` considers as a
   user-defined conversion (13.3.1.5/1.6, 13.3.3.1.2) alongside converting
   constructors, including contextual `bool` and `explicit` in direct
   contexts.  Ref-qualified members bind the implicit object by value
   category (13.3.1p4); `&`/`&&` viability is decided in
   `BindImplicitObject`.  Allocation expressions resolve the usual
   allocation/deallocation functions through the implicit global
   declarations (3.7.4p2) or the class members (12.5), and a
   `SEMA_DELETE_EXPRESSION` carries the destructor and deallocation
   selections.

3. **Value boundary** (`lower/lowir_types.cpp`, `lowir_symbols.cpp`,
   `lowir_function.cpp`, `lowir_expr.cpp`, `lowir_program.cpp`).  One owner,
   `Lowerer::ClassBoundary(TypeId)`, reads `trivially_copyable` and
   `SizeOf` and answers the parameter mode and result mode; every consumer
   (`BuildFunctionDeclaration`, `BuildFunctionVariant` entry copies,
   `LowerCall`, `LowerReturn`, `LowerVariable`, `LowerConstructorTemporary`)
   reads it and never re-derives it.  Fixture-pinned rules:
   - trivially copyable parameter of any size: direct `obj<SxA>`; callee
     entry `%t1 = addr $p` then `copyobj SxA %p, %t1`; caller copies the
     source into `$argobj__N` (`%a = addr $argobj__N`, `%s = addr src`,
     `copyobj SxA %s, %a`) and passes `$argobj__N`; an empty class keeps
     the bare slot (PA16 rule);
   - other class parameter: `ptr [pass=by_address]`; the callee declares
     `slot $p : obj<SxA>` but uses `%p` as the object address; the caller
     constructs `$arg__N` with the selected copy/move constructor and passes
     its address;
   - trivially copyable result of at most 8 bytes: `-> obj<SxA>`; callee
     builds `$retobj__N` (`%r = addr $retobj__N`, `%s = addr src`,
     `copyobj`, `return obj<SxA> $retobj__N`); caller takes the destination
     address before the call, then `copyobj SxA %call, dst`, or
     `$discard__N` when discarded;
   - every other class result: `%ret : ptr [pass=indirect_result]` first
     parameter and `-> void`, on declarations too; the callee constructs into
     `%ret` (`return local;` on an eligible top-level named local lowers the
     local in `%ret` with no slot; otherwise the selected copy/move
     constructor, or `copyobj` for a large trivially copyable class); the
     caller passes the variable slot, `$arg__N`, or `$discard__N`.
   Trivially copyable copy/move *construction* is `copyobj` at the site;
   copy/move *assignment* is always a helper call (`@X__operator_`,
   `@X__operator___ov2`) whose synthesized body copies the leading
   trivially copyable storage prefix with one `copyobj` and then runs the
   remaining base/member assignments in order (12.8p28); synthesized
   copy/move constructors have the same prefix-plus-members shape with
   `__base_entry` calls for bases (12.8p15).  A user-provided destructor
   whose body is provably empty is not called at scope exit or for
   temporaries (its definition is still emitted when it is an out-of-class
   or otherwise referenced definition); an explicit `p->~T()` always calls
   a destructor entity, synthesized for a trivial class if needed.
   Temporaries: `$tmpobj__N` for a class prvalue used as an object
   expression, `$arg__N` for argument, destination and conversion
   temporaries; a full expression records its live temporaries so calls made
   while one with a non-trivial destructor is alive are wrapped in the
   PA16 `eh_try ^call_unwind_dispatch_N … eh_end` region shape and the
   temporaries are destroyed at the end of the full expression, or at the
   end of the binding reference's scope (12.2p5).

## Failure map (186 failing fixtures)

| # | bucket | owner | count | proves progress when |
| --- | --- | --- | --- | --- |
| A | by-value parameter and result ABI, `$argobj`/`$retobj`/`$discard`/`$arg` shapes, indirect result, return-slot reuse, xvalue/const-xvalue return copy selection | lowering 3 + selection 2 | ~38 | 200-pass-by-value-lvalue, 200-return-by-value-init, 300-local-return-slot-reuse, 300-trivial-copy-value-transfer-storage-copy, 100-xvalue-pass-by-value-uses-move-ctor, 300-discarded-large-class-call-void-cast, 300-conditional-class-cv-glvalue-copy pass |
| B | implicit copy/move constructors and assignments: synthesis, deletion rules, trivial-prefix `copyobj`, deleted candidates, access of elided copies ("no viable constructor", "function the unit never declares", "unknown type name: operator=") | facts 1 + selection 2 + lowering 3 | ~32 | 200-implicit-copy-constructor, 200-implicit-copy-assignment, 300-leading-trivial-prefix-storage-copy, spec/100-defaulted-move-nontrivial-subobject, spec/200-moveonly-defaulted-move-assignment, spec/100-implicit-copy-assignment-deleted-const-member-bad, 200-deleted-constructor-selected-over-viable-bad pass |
| C | temporaries and lifetimes: full-expression destruction, `eh_try` regions, reference extension, condition declarations, conditional/short-circuit branches, empty-destructor elision, `tmpobj`/`arg` naming ("non-scalar lvalue") | lowering 3 | ~26 | 200-constructor-argument-temporary-dtor, 200-base-initializer-temporary-full-expression, 400-short-circuit-left-temporary-dtor, 400-local-reference-extends-class-temporary-lifetime, 300-shadowed-local-cleanup-rebind-on-early-return, 200-prvalue-field-access-temporary pass |
| D | declarations and syntax: ref-qualifiers (13 fixtures), out-of-class `operator=` and qualified operator calls, out-of-class `= default`, braced-init-list call arguments, `static_cast<Class>` / C-style class casts, `&static_cast<const T&>(x)` | parser + facts 1 + selection 2 | ~25 | 200-ref-qualified-member-function-lvalue, spec/200-ref-qualified-mixed-overload-bad, 200-qualified-operator-function-id, spec/200-out-of-class-defaulted-special-members, 200-parenthesized-braced-aggregate-constructor-arg, 200-static-cast-explicit-constructor pass |
| E | conversion functions ("special member name does not match its class", 25 fixtures, plus `operator char const*()` parse gaps) and built-in operator candidates over them | parser + facts 1 + selection 2 | ~32 | 400-c-style-cast-explicit-bool-conversion, 400-cv-conversion-operator-overloads, 400-class-pointer-conversion-builtin-eq, 400-user-defined-conversion-second-rank pass |
| F | `new`/`delete`: implicit global allocation functions, class-specific and `::` forms, array new with construction loops, delete with destructor, nothrow null skip | selection 2 + lowering 3 | ~20 | 300-class-new-expression-default-constructor, 300-class-specific-new-delete-selection, 400-delete-array-expression, 400-nontrivial-class-array-new-delete, 300-nothrow-new-null-skips-constructor pass |
| G | unions (anonymous member id-expressions, layout ambiguity, explicit initializer over default, multiple default initializers), delegating constructors, `switch_end` block, value-init of class array elements, bool local storage, using-declared base overload sets | facts 1 + selection 2 + lowering 3 | ~13 | 300-delegating-constructor-basic, 300-anonymous-union-member-id-expression, spec/300-union-multiple-default-member-initializers-bad, 300-class-array-missing-value-init pass |

Counts overlap (a fixture may need two buckets); the sum exceeds 186.

## Performance risks

- `ClassBoundary` and `trivially_copyable` are O(1) per query: computed once
  per class at the closing brace, never by walking members at a call site.
- Synthesized copy/move bodies are one pass over bases and fields in
  layout order; the trivial prefix is the longest run of trivially copyable
  storage from offset 0, found in that same pass.
- Implicit special members are declared per class on first demand and
  their bodies built once (memoized entity ids); no pass over all classes.
- Temporary bookkeeping is a per-full-expression stack; an `eh_try` region
  per call lists only the currently live temporaries, so text is linear in
  calls × live temporaries; sequences at or above `kInlineCleanupLimit`
  must form one linked chain like the PA16 unwinds.
- Array `new` of a constant bound must not unroll beyond the fixture shape;
  a dynamic bound lowers to a loop.
- Return-slot reuse and two-stage return selection read the return
  statements once per function (`CountReturnStatements` already exists).
- Probes (generated inputs; each timing must grow at most ~2.2× per
  doubling of `n`; medians of five):

```sh
perl -e '$n=shift; print "struct S { int f0;"; print " int f$_;" for 1..$n; print " };\nint g(S s) { return s.f0; }\nS h(S s) { return s; }\nint main(){ S s; s.f0 = 1; S t = h(s); return g(t); }\n"' 4000 > /tmp/wideval.t
perl -e '$n=shift; print "struct E { int v; E() : v(0) {} E(const E& o) : v(o.v) {} };\nstruct H {"; print " E m$_;" for 1..$n; print " };\nint main(){ H a; H b = a; b = a; return b.m1.v; }\n"' 1000 > /tmp/members-copy.t
perl -e '$n=shift; print "int g; struct T { T(int){ g++; } ~T(){ g--; } int v; };\nint f(const T&, const T&) { return g; }\nint main(){ int r = 0;"; print " r += f(T($_), T($_));\n" for 1..$n; print "return r; }\n"' 500 > /tmp/temps.t
perl -e '$n=shift; print "struct V { int v; V(int x) : v(x) {} V(const V& o) : v(o.v) {} };\n"; print "V f$_(V a) { return a; }\n" for 1..$n; print "int main(){ V x(1);"; print " x = f$_(x);" for 1..$n; print " return x.v; }\n"' 1000 > /tmp/byaddr.t
for f in wideval members-copy temps byaddr; do /usr/bin/time -f "$f %es %MKB" ./dev/cppgm++ --emit-lowir -O0 -o /dev/null /tmp/$f.t; done
```

## Commands

```sh
make -C dev cppgm++
cd pa17 && T=tests/general/200-pass-by-value-lvalue.t && \
  CPPGM_APP_ARGS="--emit-lowir -O0" scripts/run_all_tests.pl ../dev/cppgm++ my $T && \
  CPPGM_APP_ARGS="--emit-lowir -O0" scripts/compare_results.pl ref my $T; cd ..
make test-pa17                       # stage suite
make test-pa11 test-pa12             # after any scope_model/scope_builder/expr_sema change
make test-report-through-pa16        # 1382/1382 must hold
perl scripts/cppgm_file_audit.pl --stage pa17 --paths dev/src
# regression diff: the failing set before and after a change (counts hide regressions)
grep -o 'pa17/tests/[^:]*\.t' /home/vishvananda/work/.ralph/v3relay-claude-fable-5-xhigh/last-test.log | sort > /tmp/before.txt
make test-pa17 2>&1 | grep -o 'pa17/tests/[^:]*\.t' | sort > /tmp/after.txt; comm -13 /tmp/before.txt /tmp/after.txt
```

A probe's LowIR is validated by copying its `.my` to `.ref` and running
`compare_results.pl`; a harness failure that does not reproduce standalone
is a memory error (valgrind first).

## Checkpoint ledger

| checkpoint | commit | scope | outcome |
| --- | --- | --- | --- |
| plan | this commit | stage design, failure map | 42/228 at start |
| CP1 value boundary and trivial value transfer | this checkpoint | bucket A: `ClassBoundary`, `trivially_copyable`, copy/move classification, two-stage return selection, `$argobj`/`$retobj`/`$discard`/`$arg` shapes, indirect result and return-slot reuse | 73/228; PA1-PA16 1382/1382; audit passed; scaling probes 2.0×; coverage unchanged |
| CP2 implicit copy/move members | this checkpoint | bucket B: `EnsureCopyMoveMembers`, deletion rules, synthesized bodies with trivial prefix, deleted candidates, elided-copy access | 106/228; PA1-PA16 1382/1382; PA11 68/68; PA12 166/166; audit passed; focused matrix green; scaling 2.0×/2.1×; coverage unchanged |
| review 1 | — | ownership, PA11/PA12 dumps, valgrind, probes | — |
| CP3 temporaries and lifetimes | this checkpoint | bucket C: canonical temporary ownership, full-expression destruction, `eh_try` cleanup regions, reference extension, condition declarations, empty-destructor elision, conversion-operator condition calls, and slot naming | 113/228 from 106/228; five packet fixtures 5/5; PA1-PA16 1382/1382; full-through 1495/1610; audit passed; coverage unchanged |
| CP4a ref-qualified calls and class-construction boundaries | this checkpoint | bucket D slice: ref-qualified overload ownership, out-of-class defaulted members, braced call arguments, and class/C-style cast construction | 121/228 from 113/228; eight new passes; named fixtures 5/5; PA1-PA16 1382/1382; audit passed; coverage unchanged |
| CP4b remaining operators and class-cast edges | — | bucket D: qualified operator ownership, reference-target casts, and remaining implicit-object ranking | continue from 121/228 |
| CP5 conversion functions | — | bucket E: parser, `SPECIAL_MEMBER_CONVERSION`, `Classify`, built-in operator candidates | target ≥ 185/228 |
| review 2 | — | as review 1 | — |
| CP6 allocation and deallocation | — | bucket F | target ≥ 205/228 |
| CP7 unions, delegating constructors, leftovers | — | bucket G | 228/228; through-pa17 clean |
| final review | — | ledger closed, probes linear, valgrind clean | — |

## Completed Checkpoint: CP1 value boundary and trivial value transfer

Outcome: one owner now supplies the class value boundary, its triviality
facts, and by-value copy/move selection.  The stabilized stage gate reached
73/228 from 42/228 with no coverage reduction; `make test-report-through-pa16`
and the file audit pass.  The remaining bucket-A and implicit-member gaps
continue in CP2.

### Implementation packet

**Sema facts (`dev/src/sema/scope_model.h`, `scope_model.cpp`,
`scope_builder.cpp`, `scope_builder_special.cpp`, `scope_builder_members.cpp`).**
- `FunctionEntity`: add `bool copy_constructor, move_constructor,
  copy_assignment, move_assignment;` set in `BuildSpecialMember` (12.8p2-3:
  first parameter `cv X&` / `cv X&&`, all others defaulted) and where
  `operator=` members are declared (12.8p17/p19; find the member-function
  declaration path in `scope_builder_members.cpp`).
- `ClassEntity`: add `FunctionEntityId copy_constructor, move_constructor,
  copy_assignment, move_assignment;` (user-declared ones recorded at
  declaration; a later `EnsureCopyMoveMembers` fills the implicit ones in
  CP2) and `bool trivially_copyable;` computed in `CompleteClassLayout`
  next to `trivial_destructor`: false if any `user_declared` copy/move
  member is user-provided (declared, not `defaulted`, not `deleted`) or the
  destructor is user-provided, or any base or class-typed member (array
  element) is not trivially copyable; note PA16's `trivial_destructor`
  treats a user-written destructor as non-trivial, which is right here.
- `ResolveConstructor`: when the class declares constructors, keep the
  existing path; add the 12.8p32 entry point `SelectReturnConstructor` used
  by the return statement builder (`stmt_builder.cpp` return handling): try
  the argument as an xvalue first, accept only a constructor whose first
  parameter is an rvalue reference to the class, else retry as an lvalue.
  Record the selected constructor on the return statement's
  `SEMA_CONSTRUCTOR_ACTION` (the node already exists for
  copy-initialization from an lvalue; `BuildConstructorTemporary` builds
  it); a class prvalue returned or used to initialize an object of the same
  unqualified type carries no action (12.8p31).

**Lowering (`dev/src/lower/lowir_lowering.h`, `lowir_types.cpp`,
`lowir_symbols.cpp`, `lowir_function.cpp`, `lowir_expr.cpp`,
`lowir_program.cpp`).**
- `lowir_types.cpp`: add `enum ClassBoundaryMode { CBM_DIRECT_OBJECT,
  CBM_BY_ADDRESS, CBM_INDIRECT_RESULT }` and `ClassBoundaryMode
  Lowerer::ClassBoundary(TypeId, bool result) const`: parameter → direct
  when `trivially_copyable` else by address; result → direct when
  `trivially_copyable` and `SizeOf <= 8`, else indirect result.
- `BuildFunctionDeclaration` (`lowir_symbols.cpp`): for a class result in
  indirect mode prepend `%ret : ptr` with `PPM_INDIRECT_RESULT` and return
  `void`; for a by-address parameter use `ptr` with `PPM_BY_ADDRESS`.
  Declarations of functions the unit only calls must take the same shape
  (fixture 300-discarded-large-class-call-void-cast).
- `BuildFunctionVariant` (`lowir_function.cpp` entry copies): direct object
  parameter → `%t = addr $p; copyobj SxA %p, %t` (replace the current
  `copyobj %p, $p`); by-address parameter → declare the slot, emit no copy,
  and make `SlotFor`/`LowerLValue` of that parameter binding resolve to the
  `%p` address (a per-function map from binding to parameter operand).
- `LowerCall` (`lowir_expr.cpp` ~2080) and `LowerClassArgument`: direct
  object argument keeps the `$argobj__N` copy; by-address argument builds
  `$arg__N`, constructs it with the action's constructor (the sema
  `SEMA_CONSTRUCTOR_ACTION` around the argument) and passes the address.
  Indirect-result calls: allocate the destination first (variable slot when
  the call initializes a variable, `$arg__N` otherwise, `$discard__N` for
  a discarded call), pass it as the first argument, and return the
  destination as an lvalue `Value`; direct-object results return the call
  temp and the consumer emits `copyobj SxA %t, dst` (`LowerVariable` for
  `T v = f();`, `LowerDiscard` into `$discard__N`).
- `LowerReturn` (`lowir_program.cpp:2192`): direct-object result → build
  `$retobj__N` and `return obj<SxA> $retobj__N`; indirect result → construct
  into `%ret` (call the selected constructor with `%ret` first, or
  `copyobj` for a trivially copyable large class) and `return void`.
  Return-slot reuse: when the function has exactly one return statement
  naming a top-level automatic of the return type, `AddSourceSlot` skips
  that binding and `SlotFor` yields `%ret` (fixtures 300-local-return-slot-
  reuse and spec/100-implicit-move-assignment-moveonly-member pin the
  shape; 300-local-default-ctor-return-copy pins the non-reuse shape).
- `LowerVariable` (`lowir_program.cpp`): `T v = lvalue;` of a trivially
  copyable class → `%d = addr $v; %s = addr src; copyobj` (replace the
  `load obj`/`store obj` scalar path, fixture 200-implicit-copy-constructor);
  member access on a class rvalue call result reaches the materialized
  destination instead of `Unsupported("a non-scalar lvalue")`
  (`lowir_expr.cpp:1174`, fixtures 200-prvalue-field-access-function-return,
  300-function-pointer-class-return-call).
- `LowerConstructorTemporary`: name the slot `tmpobj` when the temporary is
  an object expression (member access, method call) and `arg` otherwise;
  the current trivial-default-constructor axis is wrong (fixture
  200-prvalue-field-access-temporary).

**Fixture groups to drive by (run each before the broad suite).**
- direct object parameters: general/200-pass-by-value-lvalue,
  200-associated-namespace-adl-function-call, 200-hidden-friend-adl,
  200-nonmember-operator-minus, 200-parenthesized-function-name-suppresses-adl,
  400-nonclass-by-value-converting-ctor-argument;
- direct object results: 200-return-by-value-init, 200-pass-return-forwarding,
  200-adl-ignores-using-declaration-candidate, 300-discarded-class-call,
  200-default-member-class-call-init-target, 200-copy-lookup-skips-zero-param-same-name-function;
- by-address and indirect results: 100-xvalue-pass-by-value-uses-move-ctor,
  200-default-class-value-argument-uses-copy-constructor,
  300-direct-object-parameter-passthrough-base-copy,
  300-trivial-copy-value-transfer-storage-copy,
  300-discarded-large-class-call-void-cast, 300-local-return-slot-reuse,
  300-local-prvalue-init-elides-move, 300-local-default-ctor-return-copy,
  300-return-member-copy-ctor, 300-return-const-ref-parameter-copy-not-move,
  300-xvalue-call-init-uses-move-ctor, 100-xvalue-static-cast-return-uses-move-ctor,
  300-const-xvalue-return-uses-copy-constructor, spec/100-implicit-move-assignment-moveonly-member
  (needs CP2 for the assignment helper; its `@make` shape belongs here);
- prvalue member access: 200-prvalue-field-access-function-return,
  200-prvalue-field-access-temporary, 200-prvalue-method-call-temporary,
  300-function-pointer-class-return-call.

**Required spec facts.**  3.9p9 trivially copyable; 12.8p2-3 copy/move
constructor forms; 12.8p12 trivial copy/move constructor; 12.8p31 elision
of a temporary or a returned local; 12.8p32 two-stage constructor selection
for `return local;`; 5.2.2p4 by-value class parameters are
copy-initialized from the argument; 6.6.3p2 return copy-initializes the
result; 12.2p3 a temporary of a class with a non-trivial constructor is
constructed (destruction is CP3); `pa13/lowir.md` "Object ABI Conventions"
and `copyobj` (`direct` object types, `by_address`, `indirect_result` first
parameter with `void` return).

**Commands.**  Focused: build, then the per-fixture `run_all_tests.pl` /
`compare_results.pl` pair from the Commands section over the four fixture
groups.  Broad: `make test-pa17`, the `comm -13` regression diff, `make
test-pa11 test-pa12` (new entity fields must not change a dump), `make
test-report-through-pa16`, the file audit.  Performance probe: `wideval`
and `byaddr` from the Performance section at n and 2n; both must stay
within ~2.2× and the `wideval` output must contain exactly one `copyobj`
per transfer.

**Known uncertainties.**
- The direct-result size threshold is pinned only for 1, 4 and 8 bytes
  (direct) and 24 bytes (indirect); `lowir.md`'s example returns a 16-byte
  pair indirectly, so use `<= 8` unless a fixture shows otherwise.
- Which local qualifies for `%ret` reuse: compare 300-local-return-slot-
  reuse (reused) with 300-local-default-ctor-return-copy (copied) before
  fixing the predicate; the plan's guess is "single return naming a
  top-level local of the return type".
- Whether a by-address parameter's `slot $p` is always declared even when
  unused (100-xvalue-pass-by-value-uses-move-ctor says yes; check a fixture
  whose by-address parameter is never read).
- Resolved: 300-refmember-copy-constructor-binds-storage returns a class
  with one reference member as a direct `obj<8x8>`, so a reference member
  keeps the class trivially copyable; its synthesized aggregate constructor
  is `@RefWrap__RefWrap__ov4`, which pins that the implicit default, copy
  and move constructors reserve ordinals 1-3 of `ClassEntity::constructors`
  even when never declared or used.  CP1 must reserve those positions (or
  compute the ordinal from a canonical order) before the aggregate
  constructor is appended, or the LowIR name pairing changes.

## Completed Checkpoint: CP2 implicit copy/move members

Outcome: on-demand copy/move constructors and assignments now own their
canonical entities, deletion state, overload selection, and synthesized
prefix-plus-subobject LowIR bodies.  Direct and indirect class boundaries
remain owned by the CP1 query; same-class trivial direct initialization uses
the existing `copyobj` boundary, and temporary context is resolved through a
single cached semantic-parent map.  The final gate reached 106/228 from
73/228 with no coverage reduction; the focused CP2/prvalue matrix passed,
PA11 and PA12 remained 68/68 and 166/166, prior PAs remained 1382/1382, and
the file audit passed.  The representative probes measured 2.0× `wideval`
and 2.1× `byaddr` growth at doubling, with five `copyobj` transfers for five
modeled `wideval` transfers.

## Active Checkpoint: CP4b remaining operators and class-cast edges

Goal: continue bucket D from 121/228 with PA1-PA16 clean, closing the
remaining qualified-operator, reference-target cast, and implicit-object
ranking gaps without disturbing the completed class-construction boundary.

Scope: out-of-class qualified operator/conversion ownership, reference-target
class casts, and ref-qualified implicit-object ranking.  Focus next on
`300-ref-qualified-member-call-implicit-object-rank`,
`400-out-of-class-qualified-conversion-operator-result`, and
`400-static-cast-class-reference-temporary`.
