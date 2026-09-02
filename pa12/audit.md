# PA12 Final Architecture Audit — cppgm++ --emit-semantics

## Scope and method

Traced representative call-semantics facts through their full ownership
paths: srcfile → PA5 `PreprocEngine::RunSingleFile` → `Pa6TokenCollector`
(typed kinds, decoded literal bits and string-literal counts) → `Pa10Parser`
(arena AST with token spans) → `ScopeBuilder` (one in-order walk that
declares into `SemaModel`, builds `TypeTable` types, and hands every
expression to the one `ExpressionAnalyzer`, which appends typed nodes to the
`SemaTree`) → `PrintSemanticsUnit` (renders names, operators and type
spellings on demand) → outfile in the `dev/cppgm++.cpp` envelope.  Facts
followed end to end: a call `g(1)` reached through a block-scope
using-directive (name span → `QualifiedName` → the 3.4.1 walk → overload set
→ 13.3 selection → `callee` node holding a `FunctionEntityId`, rendered from
the entity's scope chain); an array parameter `int a[3]` (declared type on
the binding for the PA11 dump, adjusted type on the function entity,
adjusted object type in the analyzer for `sizeof`, `decltype` and
initialization); a constant `const int K = 3; int arr[K];` (initializer
folded once, value recorded on the binding, read by the array bound through
a transient analysis); an overloaded function name passed as an argument
(`h(g)`: candidate entities collected once, matched per parameter type,
re-targeted at the initialization boundary); an injected anonymous-union
member (binding with `object_binding`, printed through its bindings); a
template-id `f<int>` (typed argument token range → instantiation → deferred
`function-declaration`); and a `constexpr` literal initializer retyped to
the object's cv-qualified type.

Compared the pre-cleanup and final executables (immutable copies) on all
391 checked-in `.t` inputs of pa6, pa7, pa8, pa10, pa11 and pa12 in
`--emit-ast`, `--emit-types` and `--emit-semantics` mode.  Measured scaling
with generated inputs (5k/10k/20k expression statements, local
declarations, function declarations, classes plus function declarations,
chained call definitions, constexpr chains and enumerators; 1000/2000
overloads of one name with as many calls; 100/200/400 arity-distinct
overloads; a 400-deep block chain) with interleaved runs and medians of five
on a quiet host, and profiled the one probe that stayed superlinear with
`perf`.

## Ownership trace (final)

- **Name facts**: `ReadQualifiedName(tokens, first, last, allow_template_id)`
  is the single reader of names.  Components are bare identifiers; a
  template-id is recorded as the token range of its argument list and is
  accepted only for expression names.  Declarations, type names and
  using-declarations reject it there, as the PA11 audit fixed.
- **Lookup facts**: `SemaModel::WalkUnqualified` is the one 3.4.1
  implementation.  It walks outward, carries the using-directives it passes,
  and at each namespace unions that namespace's own declarations, the
  members of its inline namespaces, and the namespaces nominated by the
  directives whose recorded application point (7.3.4p2, computed once in
  `AddUsingDirective`) is that namespace.  `LookupSet`, `LookupUnqualified`
  and `LookupTypeName` are views of that walk; `ResolveQualifier` is the one
  3.4.3 qualifier resolution shared by `LookupQualified` and
  `LookupQualifiedSet`; `CollectNamespace` is the one 3.4.3.2 namespace
  search.  Direct declarations come from the per-scope name index
  (`DirectBindings`); each scope is visited once per lookup, so results
  need no deduplication.
- **Entity facts**: `FunctionEntity` owns the canonical 8.3.5p5 signature,
  member facts and template parameter types; `Scope.class_entity` and
  `Scope.inline_namespaces` are the typed back-references lookup and
  membership need; `ClassForScope` looks only through template-parameter
  scopes.
- **Expression facts**: `ExpressionAnalyzer` is the one expression
  implementation for both dump modes.  Every initializer of a dumped
  variable, every const integral initializer, array bound, enumerator
  value, `static_assert` condition, `sizeof`/`decltype` operand and
  statement expression goes through it; it folds integral constants
  bottom-up (including 4.7 conversions for casts) and records values on
  bindings.  Analyses that are not part of the dump (array bounds,
  enumerators, `decltype` operands, `--emit-types` initializers) use
  `SemaTree::Mark/Truncate` so they leave no nodes behind.
- **Conversion and overload facts**: `Classify` and `Compare` are pure over
  `TypeTable`; `SelectBestOverload` deduplicates entities by sorting the
  candidate set, classifies each argument once per candidate, and selects
  the best viable function in two linear passes.
- **Presentation**: `SemaNode` holds only typed identity (kind, category,
  operator token, type, binding, function, scope, token span, folded value,
  intrusive links: 96 bytes, was 168).  The dump renders id-expressions from
  their span or, for synthesized nodes, from their binding; operators from
  their token or, when synthesized, from the operator enum; function names
  by streaming the entity's scope chain; unnamed namespaces from the flag.

## Findings and changes (this cleanup)

1. **Two expression implementations (material, removed)**: `--emit-types`
   evaluated constants through `ConstEvaluator` (its own literal decoding,
   name lookup, operators, casts, `sizeof`) and typed `decltype` operands
   through `ScopeBuilder::BuildExpressionType`, while `--emit-semantics`
   used `ExpressionAnalyzer`.  The modes disagreed: `decltype(r)` for
   `int& r` was `int` in one and `int&` in the other; `typedef
   decltype(nullptr) nullptr_t` printed `bool` under `--emit-types`; const
   integral values were never recorded under `--emit-semantics`, so three
   PA11 fixtures (`200-using-directive-values`, `200-using-declaration-
   values`, `200-const-int-static-assert`) failed in that mode; and
   `decltype(::f(0))` failed under `--emit-types`.  The evaluator, the
   operand-type interface and the second typing path are deleted; cast
   folding moved into the analyzer; both modes now agree on all 391 inputs
   except where the old mode was wrong.
2. **Block-scope using-directives unreachable (material, fixed)**: the
   overload-set lookup applied a block's directives only "at the block",
   where 7.3.4p2 never places them, so `using namespace X; g(1);` was
   rejected as an unknown name — a README-required feature with no fixture.
   The single-result lookup applied them at the block instead, ahead of
   enclosing declarations, and patched qualifier lookup with a lexical-first
   special case; `LookupQualifiedSet` walked the qualifier chain twice.
   One walk now implements 3.4.1 with 7.3.4p2 for both APIs: `g(1L)` with a
   global `g(long)` and `using namespace X` inside `N` selects `::g(long)`
   (was `X::g(int)`), and two non-function declarations reaching one level
   from different scopes are rejected as ambiguous (7.3.4p6).
3. **Class-membership scan (material, fixed)**: `ClassForScope` compared
   every class entity against every scope on the chain for every function
   declaration (20000 classes plus 20000 declarations: 6.1 s, ×3.4 per
   doubling), and its walk through block scopes made a block-scope `extern
   void loc(int);` inside a member function a member, so `loc(1)` failed.
   The class scope now carries its entity; 0.51 s, equal to parser-only.
4. **Redeclaration scan (material, fixed)**: `DeclareFunction` scanned
   every binding of the scope per declaration (20000 declarations 1.63 s);
   it reads the name's index vector: 0.39 s.
5. **Overload resolution (material, fixed)**: entity deduplication by
   linear `seen` scan and best-viable selection by pairwise dominance were
   quadratic in the candidate set per call (2000 overloads with 2000 calls:
   9.4 s, ×7 per doubling), and each candidate copied every argument
   record including its candidate vector.  Sorted deduplication, a linear
   tournament plus confirmation pass, and by-reference arguments: 1.2 s.
   The remaining cost is 13.3 itself — every call classifies every
   candidate of the name — which is the documented bound.
6. **Inline-namespace scan (fixed)**: both namespace searches iterated
   every child scope of a namespace to find inline namespaces; on 20000
   chained call definitions the global namespace has 40000 children and
   `perf` attributed 48 % of the run to it.  Namespaces keep an
   `inline_namespaces` list: 2.04 s → 0.69 s (parser-only 0.61 s).
7. **Textual downgrades (fixed)**: CP4 had re-encoded template-ids into the
   name component (`f<int>`), re-split them with `find('<')` and rescanned
   the tokens for `<`; template parameters were identified by name string
   in `std::map<std::string, TypeId>` and `vector<string>`.  The name now
   carries the argument token range and template parameters are identified
   by their `TypeId`.  `template<> struct S<int>` is rejected again instead
   of declaring a class named `S<int>`.
8. **Per-node strings (fixed)**: `SemaNode.operator_spelling` and
   `expression_name` existed for the synthesized constructor-action `&` and
   injected union members, and the dump string-compared `"<unnamed>"` next
   to the flag.  Rendered from the operator enum and the binding; peak RSS
   on 20000 expression statements 75 MB → 62 MB, equal to `--emit-ast`.
9. **Parameter object type (fixed)**: `int a[3]` as a parameter was
   analyzed as `array of 3 int` (`sizeof(a)` gave 12, `decltype(a) y = a`
   was rejected, `int* p = a` printed an array lvalue); 8.3.5p5 gives the
   parameter `int*`.  The binding keeps the declared type the PA11 dump
   pins; the analyzer adjusts.
10. **Hidden state and dead vocabulary (removed)**: `pending_array_bound_`
    (implicit parameter through a member, now `deduced_bound`), an
    `InitContext` with five flags of which one was read, the unused `AstId`
    of `IsZeroLiteral`, `IsNullptrLiteral`, `OverloadSelection.conversions`,
    the unused out-parameters of `SelectTargetFunction`, `(void)`-silenced
    locals in `conversions.cpp` and `type_table.cpp`, `ImplicitConversion::
    to`, `ClassEntity::anonymous_storage`, `FunctionEntity::is_constructor`,
    the `std::map<ScopeId, SemaId>` over a dense id domain (now a vector),
    the heap-allocated analyzer (now a member), self-assignments in
    `AnalyzeSizeof`, and the two duplicated declarations-only statement
    walks (now `BuildDeclarationsOnly`).
11. **5.19 parity**: a left shift of a negative value no longer folds
    (undefined behaviour is not a constant expression); `const double d =
    1.5;` and `const int x = f();` are no longer rejected under
    `--emit-types`, where the evaluator demanded an integer literal for any
    const object of fundamental type — only `constexpr` requires a constant.

Accepted at stage scale: `Binding.name` and `Scope.name` remain
`std::string`s (PA11 decision); `MarkTemplateInstanceUsed` and
`BuildTemplateInstance` search the instance list linearly (instances are
few); `FunctionCandidates` deduplicates by linear search over one name's
entities; the parser's `qualified_pointer_declaration` token pattern is a
narrow 6.8 disambiguation at the parser's own layer, pinned by the PA10
dumps.  Rejected: interning `Binding.name` (lookup already hashes once per
scope and the strings are the dump model).

## Behaviour on checked-in inputs

391 inputs, old versus final executable.  `--emit-ast`: identical on all.
`--emit-types`: 388 identical; `pa12/general/300-nullptr-equality` prints
`type-alias nullptr_t nullptr_t` (was `bool`, finding 1);
`300-qualified-direct-function-hides-using-directive` now succeeds
(finding 1); `pa10/spec/200-explicit-specialization-syntax` fails on the
template-id class name as the PA11 audit specified (finding 7).
`--emit-semantics`: 387 identical; the three PA11 inputs of finding 1 now
succeed; the same pa10 input is rejected.

## Performance evidence

Immutable executables, interleaved, medians of 5 on a quiet host, wall
seconds; the last column is the final executable's parser-only mode on the
same input.

| probe (--emit-semantics) | old | final | final --emit-ast |
| --- | --- | --- | --- |
| 5000 / 10000 / 20000 expression statements | 0.07 / 0.15 / 0.30 | 0.07 / 0.14 / 0.27 | 0.06 / 0.11 / 0.23 |
| 5000 / 10000 / 20000 local declarations | 0.06 / 0.12 / 0.25 | 0.06 / 0.11 / 0.24 | 0.05 / 0.11 / 0.23 |
| 5000 / 10000 / 20000 function declarations | 0.18 / 0.56 / 1.63 | 0.09 / 0.19 / 0.39 | 0.10 / 0.21 / 0.43 |
| 5000 / 10000 / 20000 classes + function declarations | 0.48 / 1.79 / 6.10 | 0.12 / 0.25 / 0.51 | 0.12 / 0.25 / 0.49 |
| 5000 / 10000 / 20000 chained call definitions | 0.26 / 0.75 / 2.04 | 0.17 / 0.35 / 0.69 | 0.15 / 0.30 / 0.61 |
| 5000 / 10000 / 20000 constexpr chain | 0.09 / 0.19 / 0.38 | 0.09 / 0.17 / 0.36 | 0.08 / 0.16 / 0.33 |
| 5000 / 10000 / 20000 chained enumerators | 0.05 / 0.10 / 0.20 | 0.05 / 0.10 / 0.22 | 0.05 / 0.10 / 0.20 |
| 1000 / 2000 overloads × as many calls | 1.32 / 9.42 | 0.33 / 1.23 | 0.04 / 0.08 |
| 100 / 200 / 400 arity-distinct overloads and calls | 0.03 / 0.15 / 0.62 | 0.03 / 0.13 / 0.54 | 0.03 / 0.13 / 0.54 |
| 400-deep block chain | 0.00 | 0.00 | 0.00 |
| `--emit-types`, 20000 function declarations | 1.54 | 0.40 | 0.43 |
| `--emit-types`, 20000 classes + declarations | 5.88 | 0.52 | 0.49 |

Peak RSS is unchanged except for the expression-statement probe (75 MB →
62 MB, finding 8); on every probe it equals `--emit-ast` on the same input.
The old executable grew ×2.9 per doubling on declarations, ×3.4 on classes
plus declarations, ×2.8 on chained calls and ×7.1 on the overload probe;
the final one grows ×2 on all but the overload probe, whose ×3.7 is the 13.3
candidate set (2000 candidates classified per call, 0.3 µs each).  The
semantic pass is within 0.1 s of the parser alone on every linear probe;
the remaining cost is tokenization, node construction and output (the PA10
audit's profile).  Outputs of old and final are byte-identical on every
probe.

## Conformance validation

- `make test-report-through-pa12`: 823/823 (657 through pa11 unchanged +
  166 pa12; the course pa12 set has 0 tests).  `make test-pa11`: 68/68;
  `make test-pa12`: 166/166.
- `perl scripts/cppgm_file_audit.pl --stage pa12 --paths dev/src`: passes
  with the pre-existing `recog_parser.h` header-layout warning.
- Standard-conformance probes (no fixture): block-scope directive call,
  namespace-scope directive versus global candidate, inner-namespace
  directive versus global object, local `extern` declaration inside a
  member function, array-parameter `sizeof`/`decltype`/initialization,
  `decltype` of a reference variable — all as 3.4.1, 7.3.4, 8.3.5p5 and
  7.1.6.2 specify, in both dump modes.

## Checkpoint ledger

| checkpoint | commit | outcome |
| --- | --- | --- |
| plan | `d5782429b` | stage design, dump conventions, failure map |
| CP1 semantic tree, declarations, statements, conversions | `b4ffb8820` | 86/166; through-pa11 657/657 |
| CP2 calls, overload sets, ranking, built-ins | `a1d3b816a` | 153/166 |
| CP3 parser boundaries, using-directive application point | `1071e5663` | 157/166 |
| CP4 class members, anonymous unions, member pointers, template-ids | `28c7fd605` | 166/166 |
| CP5 architecture audit and cleanup (this document) | final commit | 166/166; through-pa12 823/823; findings 1–11 |
