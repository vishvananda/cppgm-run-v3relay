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
   queued and analyzed when the outermost class completes.  `SemaModel::
   LookupMember(entity, name, filter, result)` is the one base-chain lookup
   (own scope, then base; derived names hide base names; using-declarations
   re-expose) used by member access, implicit-this names, qualified names and
   inherited typedefs.  Access checking (`CheckAccess(member, naming class,
   context scope)`: 11.2 with friends and protected-through-derived) is one
   function called from member access, qualified names, base conversions
   and type lookups; it lands with the initialization checkpoint.

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
   `eh_try` / `eh_cleanup` / `eh_end` / `resume` instructions.

## Failure map (218 failing, 25 passing; 19 fixtures expect EXIT_FAILURE)

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
| CP2 constructors, destructors, lifetime | user/implicit/inheriting ctors and dtors, mem-initializers, default member initializers, subobject plans, demand-driven C1/C2/D1/D2 with aliases, local and array lifetime at every exit, namespace-scope init/fini, thread_local family, EH cleanup shapes, serializer additions | the `unsupported pa11 declaration` group and the lifetime fixtures pass; expected ≥ 160/243 |
| CP3 operator overloading and ADL | member/non-member/hidden-friend operators, ADL sets and source-point rules, built-in fallback, functors, subscript/call/increment operators, chained `<<`, enum operators, literal operator | the operator group (20) passes; expected ≥ 205/243 |
| CP4 initialization forms and access control | aggregate/brace-elision/value-init of class objects (locals, globals, nested arrays, string members), copy-init through converting ctors with `explicit` rejection, narrowing rejection, placement new, anonymous struct/union members, bit-field access lowering (7 `400-*` fixtures), access checks with the watch-list verified | 243/243 and through-pa16 clean |
| CP5 audit and cleanup | architecture audit (`audit.md`), performance evidence, dead-path removal | through-pa16 clean; probe timings recorded |

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

## Active Checkpoint: CP2 — constructors, destructors, lifetime

Goal: lower explicit and synthesized construction/destruction consistently
for class subobjects, arrays, locals, globals, thread-local objects, and all
control-flow exits while preserving CP1's canonical class layout and member
lookup. A constructor or destructor must either produce the planned helper
calls and cleanup edges or be rejected cleanly; it must never silently emit
an uninitialized or partially destroyed object.

### Implementation Packet

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
