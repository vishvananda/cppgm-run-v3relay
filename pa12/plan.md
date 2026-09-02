# PA12 Plan — cppgm++ --emit-semantics (call-semantics dump)

Grading: `cppgm++ --emit-semantics -o x.my x.t`; exit status must match
`x.ref.exit_status` (29 fixtures expect EXIT_FAILURE, 137 expect success and
a byte-exact dump).  No reference binary: the 166 `.ref` files under
`pa12/tests/{spec,general}` are the only format oracle, so every convention
below is fixture-pinned.  `cppgm.tests/course/pa12` is empty.  Harness: same
forked batch runner as PA11.  `pa12.gram` is identical to `pa11.gram`.
State at planning: every test reports EXIT_NOT_IMPLEMENTED because
`run_emit_semantics_mode` in `dev/cppgm++.cpp` is the PA10 stub; through-pa11
is 657/657 and the file audit passes.

## Stage Design

Data flow per translation unit (fresh state per file, argv order):
`PreprocEngine::RunSingleFile` → `Pa6TokenCollector::tokens` → `Pa10Parser`
(arena AST, dump unchanged) → `ScopeBuilder(tokens, arena, model,
tree).Build(root)` — one in-order pass that declares into the `SemaModel`
(PA11, unchanged output) **and** analyzes every initializer, condition,
expression statement and return as it is reached, appending typed nodes to a
`SemaTree` → `PrintSemanticsUnit`.  Because the pass is in source order,
unqualified lookup sees only earlier declarations (fixture
`300-namespace-function-body-later-anonymous-overload` depends on this); no
position stamps and no second pass.  Any exception → EXIT_FAILURE.

Owning boundaries (`dev/src/sema/` unless noted; every new `.cpp` is added to
`FRONTEND_OBJ_BASENAMES_cppgm++` in `dev/frontend_source_sets.mk`):

- `sema_tree.h/.cpp` (new) — `SemaId` (0 = null), `ValueCategory {VC_LVALUE,
  VC_PRVALUE, VC_XVALUE}`, `SemaKind` (one enumerator per dump line kind:
  translation-unit, namespace, type-alias, variable, function-declaration,
  function-definition, parameter, simple-declaration, compound,
  expression-statement, return, if, then, else, while, do, for, for-init,
  iteration, switch, case, default, break, continue, condition,
  condition-declaration, call, callee, id-expression, literal, unary,
  postfix, binary, assignment, conditional, subscript, member, cast, sizeof,
  braced-init-list, constructor-action), `SemaNode {kind, category, type,
  op (ETokenType of the operator or cast token; KW_AUTO = none), binding,
  function (FunctionEntityId), scope, first/last token span, has_value,
  value, first_child, last_child, next_sibling}`, `SemaTree {Make, Append,
  At, Root}`.  Intrusive child links, no per-node vectors or strings; every
  spelling is rendered at print time from typed identity.
- `scope_model.h/.cpp` (extended) — `Binding.scope` (owning scope, set in
  `AddBinding`), `Binding.function` (FunctionEntityId, 0 for non-functions),
  `Scope.unnamed_namespace` flag (replaces the `<unnamed>` string test),
  `FunctionEntity {scope, name, type (canonical 8.3.5p5-adjusted function
  type), defined}` with `CreateFunction`/`FunctionAt` (parameter names are
  not entity facts; they live on the definition's parameter nodes),
  `UsingDirective {nominated, apply_at}` replacing the bare `ScopeId` in
  `Scope.using_directives` (7.3.4p2: `apply_at` = nearest enclosing namespace
  scope of the directive's scope that also encloses `nominated`, computed
  once in `AddUsingDirective`; unqualified lookup carries the directives met
  on the way out and searches each when it reaches its `apply_at`; qualified
  lookup keeps using the directives stored on the nominated namespace),
  `LookupSet(scope, name, filter, std::vector<BindingId>&)` — the overload
  set: all bindings of the name in the first scope of the 3.4.1 walk that
  holds any (using the per-scope index past 8 bindings), plus the
  using-directive closure at that level, deduplicated by `Binding.function`;
  `LookupQualifiedSet` likewise for 3.4.3.2 (direct declarations first,
  directives only when none).  A non-function binding in the found set means
  "not a function" for call purposes (3.3.10 hiding).
- `type_table.h/.cpp` (extended) — `Decay(TypeId)` (array → pointer to
  element, function → pointer, reference → referent, then top-level cv
  removed for prvalues), `AdjustParameter(TypeId)` (8.3.5p5: array/function
  → pointer, then top-level cv removed), `Referent(TypeId)`, predicates
  `IsIntegral/IsArithmetic/IsScalar/IsPointer/IsNullPointerType`,
  `Promote(TypeId)` (4.5, enums → underlying promoted), `UsualArithmetic(a,
  b)` (5p9), `CompositePointer(a, b, ok)` (5.9p2 cv union), and — in CP4
  only — `TYPE_MEMBER_POINTER {class type, member type}` spelled
  `member-pointer of <class> to <member>`, and a `const` flag on function
  types spelled `function of (...) const returning T`.
- `conversions.h/.cpp` (new) — `ConversionRank {RANK_EXACT, RANK_PROMOTION,
  RANK_CONVERSION, RANK_ELLIPSIS, RANK_NONE}`, `ImplicitConversion {rank,
  kind (identity, lvalue-to-rvalue, array-to-pointer, function-to-pointer,
  integral-promotion, integral-conversion, floating-*, pointer,
  pointer-to-bool, null-to-pointer, null-to-nullptr_t, qualification,
  boolean), qualification (bool), reference (none / direct / temporary),
  rvalue_ref_to_rvalue, function_lvalue_to_lvalue_ref, to (TypeId)}`,
  `Classify(expr node, target type, context) → ImplicitConversion` (4.1–4.12
  plus 8.5.3 reference binding) and `Compare(a, b) → better/worse/equal`
  (13.3.3.2p3–4 plus the PA12 tie-break "direct reference binding beats
  temporary binding").  Pure functions over `TypeTable`; no lookup.
- `expr_sema.h/.cpp` (new) — `ExpressionAnalyzer` owning expression typing,
  value categories, integral constant folding (bottom-up `has_value/value`,
  signed 64-bit with overflow and division-by-zero producing *no value*;
  contexts that require a constant then fail), literal typing from
  `Pa6Token.lit_type/lit_value/lit_count`, id-expression resolution
  (`LookupSet` → variable/parameter/enumerator/function/type), operators
  (5.2–5.18), casts, sizeof/alignof, functional casts and value-initialization,
  `Initialize(node, target, context)` (copy-initialization for variables,
  condition declarations, returns, arguments: classification, literal
  retyping, temporary-cast wrapping, errors), and in CP2 call analysis
  (`overload.cpp`).  It **replaces** `ConstEvaluator`'s AST walkers and
  `ScopeBuilder::BuildExpressionType`: `ConstEvaluator::Evaluate` becomes a
  thin adapter (`Analyze` → `has_value ? value : throw`), `BuildDecltype` and
  `sizeof` typing read the analyzed node, and `ConstantOperandTypes` is
  deleted — one expression implementation (spec §1).  `BuildTypeId`,
  `BuildDecltype`, `BuildSpecifierType` become callable by the analyzer.
- `overload.cpp` (new, CP2) — candidate collection from `LookupSet`,
  viability (arity with variadic prefix, one `ImplicitConversion` per
  argument), best-viable selection (13.3.3: better on every argument and
  strictly better on one; none → ambiguous → error), target-directed
  selection of an overloaded function name for a pointer/reference-to-function
  target (13.4: the unique candidate whose canonical type equals the target's
  function type), indirect calls through pointers/references to functions
  (exact arity, each argument convertible), the built-in names
  `__builtin_abort` (`function of () returning void`) and
  `__builtin_constant_p` (folds to `literal prvalue int 0|1`).
- `stmt_builder.cpp` (new file, `ScopeBuilder` members) — statement walk
  with `StatementContext {scope, function entity, loop_depth, switch_depth}`:
  compound (own block scope), expression-statement, return (converted to the
  function's return type; a value in a `void` function or a missing value in
  a non-void one → error), if/while/switch/do/for with condition and
  condition-declaration (`T x = expr` copy-initialized; its scope is a block
  created for the statement so the name is visible in both substatements and
  in the loop body), for-init (own block scope), case (constant label
  converted to the promoted condition type; requires `switch_depth > 0`),
  default (`switch_depth > 0`), break (`loop_depth + switch_depth > 0`),
  continue (`loop_depth > 0`); an unbraced substatement that is a
  declaration gets its own implicit block scope (6.4p1).  Contextual bool
  conversion of conditions (4p4): scalar types only; scoped enumerations →
  error.  Block-scope declarations reuse `BuildNode` and then append the
  `simple-declaration` node; `RecordConstantValue` and variable
  initialization move onto the analyzer (`Initialize`).
- `scope_builder.cpp` / `type_builder.cpp` (extended) — every function
  declarator (`BuildSimpleDeclaration`, `BuildFunctionDefinition`, block
  scope `extern` declarations) resolves to a `FunctionEntity`: the canonical
  type is `TypeTable::Function(result, AdjustParameter(each), variadic)`;
  an existing entity of that name in the target scope with an equal
  canonical type is a redeclaration (differing return type → error, second
  definition → error), otherwise a new entity (overload).  The PA11 binding
  keeps the declared, unadjusted type so `--emit-types` is unchanged;
  parameters bind with their declared type and the analyzer reads
  `AdjustParameter` minus the cv stripping (array/function decay only) as
  the object type.  `union {…} u;` at block scope names its class
  `__local_type<N>`, a block-scope bare `enum {…};` names its enumeration
  `__anonymous_enum<N>` (N = 1-based per-unit counters of unnamed local
  types; namespace-scope rules from PA11 stay); the array bound of `T a[] =
  {…}` is the initializer count (8.5.1p4, `ApplySuffix` currently throws
  "incomplete array type").
- `semantics_dump.h/.cpp` (new) — `PrintSemanticsUnit(out, unit, tree,
  model, tokens)`: pre-order, two spaces per depth, `translation-unit` at
  column 0.  Renders: binding names, `FunctionEntity` qualified names (scope
  chain to global joining namespace and class names, skipping unnamed
  namespaces and non-namespace scopes: `A::B::f`, `outer::g`, `make`),
  `TypeTable::Spell`, operator text as `TokenTypeToStringMap[op] + ":" +
  token spelling` (C-style cast `OP_LPAREN:` with empty spelling,
  `KW_STATIC_CAST:static_cast`, `KW_TRUE:true`, `KW_NULLPTR:nullptr`;
  functional casts print no operator), id-expression spellings joined from
  the node's token span (`N::value`, `::f`, `wrap::g`), literal spellings
  from the token (`0xfeedL`, `1.0f`, `"x"`), synthesized literals in decimal.
- `dev/cppgm++.cpp::run_emit_semantics_mode` — mirrors `run_emit_types_mode`
  with a `SemaTree` per file.
- `parser/recog_token.h/.cpp` (CP1) — `emit_literal_array` keeps
  `lit_type` and a new `lit_count` (string literal → `array of <count>
  const <type>`, lvalue).  `parser/ast_parser*.cpp` (CP3) —
  `parse_postfix_root`: a functional cast may start with a run of
  simple-type-specifier keywords (`unsigned long(e)`) or a
  `decltype(...)` (`decltype(x)(1)`), the callee id-expression spanning all
  of them; `can_start_declaration`: a statement may start with `::`
  (`::n::S * (*p)(int, char *[]) = &make;`).  Both only accept inputs that
  fail to parse today, so PA10 dumps do not move.

Dump conventions (fixture-pinned; implicit conversions never print a node):

- Namespace scope: `type-alias N T`, `variable N T` (+ initializer child),
  `function-declaration Q T` (no children; every redeclaration prints),
  `function-definition Q T` then `parameter <name> <canonical parameter
  type>` (unnamed → `parameter  T`, two spaces) then `compound-statement`;
  `namespace-definition <name|<unnamed>>` with children.  Class, enum,
  template, using, alias-of-namespace, static_assert and linkage
  declarations print nothing (linkage blocks are transparent).  Member
  function definitions (CP4) print after all namespace-scope nodes with
  `parameter this pointer to <class>` first.
- Block scope: `simple-declaration` wrapping `variable`/`type-alias`/
  `function-declaration` nodes (a bare `enum {…};` prints an empty
  `simple-declaration`); `using U = int;` prints `type-alias` unwrapped.
- Statements: `expression-statement`, `return-statement`, `if-statement` /
  `condition` / `then` / `else`, `while-statement` / `condition` / body,
  `do-statement` / body / `condition`, `for-statement` / `for-init-statement`
  / `condition` / `iteration` / body, `switch-statement` / `condition` / body,
  `case-statement` label statement (chained cases nest as in the AST),
  `default-statement`, `break-statement`, `continue-statement`,
  `condition-declaration` / `variable` (+ initializer).
- Expressions: `<kind> <category> <type> [<extra>]`.  `id-expression lvalue
  <declared type, references print their referent> <source spelling>`;
  function names print their canonical function type; enumerators print as
  `literal prvalue <enum type> <decimal value>`.  `literal prvalue <type>
  <spelling>`; a null pointer constant `0` copy-initialized to a pointer
  prints `literal prvalue <unqualified target pointer> 0` (to `nullptr_t`:
  `literal prvalue nullptr_t 0`); a `constexpr` integral variable's literal
  initializer prints with the variable's cv-qualified type (`literal prvalue
  const int 1`); neither happens inside `?:` (`? 0 : p` keeps `int 0`).
  `unary-expression`, `postfix-expression`, `binary-expression`,
  `assignment-expression <lvalue> <lhs referent type> OP_..` carry the
  operator; `conditional-expression`, `subscript-expression` (children
  always array/pointer first, then index — `1[a]` is commuted), `sizeof-
  expression prvalue unsigned long int` (no children), `cast-expression
  prvalue <type> [<op>]` (cast to a reference type does not create a node:
  the operand is re-labelled with the reference type and lvalue/xvalue),
  `size_t()` value-initialization → `literal prvalue <type> 0`.
  `call-expression <category> <type>`: type is the return type as declared
  (reference returns print the reference type with lvalue/xvalue); first
  child is `callee <qualified name> <canonical type>` for a named function
  (resolved through using-declarations/directives/aliases to the entity)
  or the callee expression for pointer/reference callees; then arguments.
  A temporary created for a reference parameter through a promotion or
  conversion prints `cast-expression prvalue <cv referent>` around the
  argument (`converted(u)`); identity/qualification temporaries print
  nothing.  `braced-init-list lvalue <array type>` with element children.

Semantic rules (N3485): value categories 3.10/5.1.1p8 (names of
variables/parameters/functions lvalue, enumerators and literals prvalue,
string literals lvalue arrays); 5p9 usual arithmetic conversions with
4.5/4.7 promotions (enum → underlying promoted); 5.3.1 (`&` prvalue
pointer, `*` lvalue referent incl. function lvalues, `++/--` lvalue
scalar, `!` bool, `~`/`-`/`+` promoted); 5.2.6 postfix prvalue of the
decayed type; 5.7 pointer ± integral, pointer − pointer → `long int` for
cv-compatible pointees; 5.9/5.10 comparisons → bool over arithmetic,
enumerations, composite pointers, null pointer constants and `nullptr_t`
(pointer vs non-zero integer → error); 5.14/5.15 bool; 5.16 (both same-type
lvalues → lvalue; else prvalue with arithmetic/composite-pointer/null rules);
5.17 assignment (modifiable lvalue LHS, RHS implicitly convertible;
compound arithmetic operators need arithmetic/enum operands, `+=`/`-=`
also pointer LHS with integral RHS); 5.18 comma (right operand); 5.2.1
subscript (integral or unscoped enum index); 5.2.2 calls; 5.2.3/5.2.9/5.4
casts over the integral/enum/floating/pointer/bool/nullptr subset; 5.3.3
sizeof over expressions and type-ids (`sizeof(T)` when the id names a
type); 4.10 null pointer constant = integer *literal* whose value is 0
(`PA6_ZERO_FLAG`) or `nullptr` — an enumerator is not one; 4.4 recursive
qualification conversions with the "const at every intermediate level" rule
(`int**` → `const int* const*` ok, → `const int**` error); 4.10 object
pointer → cv `void*` only if cv is not dropped; 8.5.3 reference binding
(direct when reference-compatible with cv1 ≥ cv2 — array cv is element cv;
non-const lvalue references never bind rvalues or converted temporaries;
rvalue references never bind lvalues except function lvalues; reference-
related but less-qualified → error; otherwise const lvalue/rvalue references
bind a temporary through a standard conversion); 13.3.3.2 ranking with the
bullets exercised by fixtures (promotion > conversion; pointer→bool worst;
proper subsequence/qualification; ellipsis last; ranked prefix only before
`...`; rvalue reference to rvalue beats lvalue reference; lvalue reference
to function lvalue beats rvalue reference; less cv-qualified referent
wins); 13.3.3p1 best viable across all arguments; 13.4 target-directed
selection; 8.3.5p5 canonical signatures and 13.1/3.3.10 redeclaration
matching; 6.4–6.6 statement scopes and jump validity; 6.4.2 integral
constant case labels; 7.3.4p2 using-directive application point; 3.4.3.2p2
qualified lookup ignoring directives when the namespace declares the name.

## Failure Map

At CP1 start: 0/166; after CP1: 86/166.  The initial state below records
`NotImplementedException`.  Existing front-end behaviour on the inputs:
`--emit-ast` rejects 3 (`300-decltype-functional-cast`,
`300-scoped-enum-functional-cast-integral`, `300-local-extern-function-
declaration`; owner CP3 parser); `--emit-types` additionally rejects 12:
`200-function-pointer-array-deduced-bound` (deduced bound, CP1),
`200-local-using-directive-preserves-nearer-namespace-type` (7.3.4p2, CP3),
`300-qualified-direct-function-hides-using-directive` (decltype of a call,
CP2), the two `nullptr_t` spec fixtures (built-in type name, CP2), 4
member-pointer inputs (CP4), `300-most-vexing-local-function-member-call-bad`
(already rejected on `B(A);` — expected failure, stays rejected), and the
two namespace/ordinary-name conflict fixtures (already rejected — pass once
the driver is wired).

Groups by owner (counts are fixtures whose *first* blocking feature is
listed):

- CP1 procedural core without calls — 56 success fixtures (all spec/general
  inputs whose bodies contain no call expression, e.g. `100-local-arith`,
  `200-comma-bitwise-shift`, `200-loop-jumps`, `300-floating-*`,
  `300-pointer-*-conditional`, `300-subscript-*`, `300-unscoped-enum-
  integral-operators`, `200-constexpr-complete-object-cv`, `200-function-
  pointer-array-deduced-bound`, `100-floating-functional-cast`,
  `300-nested-functional-pointer-bool-casts`) and 15 failure fixtures
  (jump/case/default/return/condition/assignment/equality/redeclaration
  errors and the two namespace conflicts).  14 further failure fixtures
  whose input calls a function pass in CP1 only because calls are rejected;
  they count as proven only in CP2.
- CP2 calls and overload resolution — 82 fixtures (68 successes, 14
  failures): every remaining
  call-bearing success (direct, qualified, using-directive/declaration,
  alias, unnamed/inline namespace, function pointer/reference, deref,
  reference-returning, variadic, string-literal arguments, `__builtin_*`,
  overload exact/ranking/pointer/reference/volatile/ellipsis fixtures,
  target-directed `g` arguments and initializers, `decltype(::f(0))`,
  `nullptr_t` fixtures) plus the 14 call-related failure fixtures
  (`100-bad-no-match`, `300-bad-ambiguous-overload`, `300-overload-no-
  global-best-bad`, `300-nullptr-t-vs-long-overload-bad`, the three bad
  reference bindings, indirect-call arity/conversion, `200-bad-noncallable-
  variable`, `300-enumerator-is-not-null-pointer-constant-bad`, deep
  qualification and `void*` cv drops).
- CP3 front-end facts — 4: the three parser fixtures and the block-scope
  using-directive fixture.
- CP4 class-aware fixtures beyond the README slice — 9:
  `200-local-anonymous-union-variable`, `300-block-anonymous-union-injected-
  members` (storage variable, `constructor-action`, synthesized
  `__anonymous_union_type__F_L::…` and `__local_type1::__local_type1`
  definitions with `this`), `300-elaborated-local-struct-copy-init`,
  `300-reference-binding-pointee-const-pointer` (`A a;` →
  `constructor-action A::A` and the appended `A::A` definition;
  `member-expression lvalue <cv member type> OP_DOT:x`, injected members
  print the bare name), the three member-pointer alias fixtures,
  `300-static-cast-member-overload-prefers-nontemplate` (`&A::f` with
  `this`-first function type, non-template preferred), `300-static-cast-
  overloaded-function-template-argument` (template-id id-expression,
  instantiated `function-declaration` nodes with `parameter` children
  appended at the end in instantiation order).  Each is one or two fixtures;
  none is required by the README, so they close the stage last.

## Performance Risks

- Analysis is one in-order walk: O(AST) node visits, each expression node
  analyzed once and folded bottom-up; `sizeof`/`decltype`/case-label/
  `__builtin_constant_p` operands are analyzed once and their value read
  from the node — no second walk, no exception-driven retry.
- Lookup per name: O(scope depth) index probes plus the directive closure
  (each namespace once); the overload set of a name is the per-scope index
  vector, never a scan of the scope.  Overload resolution per call is
  O(candidates × arguments) classification plus the same for pairwise
  ranking against the current best; candidates are the bindings of one name.
- `SemaTree` is an append-only vector with intrusive links; the dump is
  linear in nodes; qualified names and type spellings render at print time.
- Function entities: matching a redeclaration scans the same-named bindings
  of one scope (index vector) comparing canonical `TypeId`s — O(overloads).
- Recursion depth follows AST depth (parser-bounded); statement contexts are
  passed by value, no worklists.  Deferred member bodies (CP4) are a
  per-class list processed once at the class end.
- Probes (medians of interleaved runs, immutable executables, compared with
  `--emit-ast` on the same input): 20000 expression statements in one body,
  20000 local declarations with initializers, 2000 overloads of one name
  with 2000 calls, 400-deep nested blocks and `if` chains, 5000-step
  constexpr chain (must stay linear after the evaluator replacement).

## Checkpoint Ledger

- CP1 semantic tree, driver, declarations, statements, expressions without
  calls, conversions for initialization/return/assignment, function
  entities, string-literal token facts — completed at 86/166 (56 packet
  success fixtures byte-exact, CP1 rejection fixtures accepted, and calls
  retained as the CP2 boundary).  `make test-report-through-pa11` is
  657/657; the file audit passes; the 10k/20k/40k probe remains linear and
  within the parser-only baseline.
- CP2 calls, overload sets, ranking, target-directed selection, indirect
  calls, built-ins, decltype over calls — completed at 153/166; focused call
  fixtures match, through-pa11 remains 657/657, the file audit passes, and
  the 10k/20k/40k semantics probe is linear and near parser-only cost.
- CP3 parser/front-end expression and statement boundaries — completed at
  157/166; the four packet fixtures and two using-directive regressions pass,
  `make test-report-through-pa11` is 657/657, and the file audit passes with
  its existing header-layout warning.  The nine class/member/constructor
  fixtures remain the CP4 boundary.
- CP4 class-aware fixtures (anonymous/local unions with constructor actions,
  member expressions, member pointers, `this`-first member functions,
  static_cast over overloaded member/template names, appended instantiation
  declarations) — completed at 166/166; all nine packet fixtures pass, the
  through-pa11 report is 657/657, and the file audit passes with its existing
  one header-layout warning.
- CP5 architecture audit and cleanup (`audit.md`): single expression
  implementation, no textual downgrades (operator/name/literal facts typed,
  presentation rendered on demand), bounded lookup and resolution, probe
  evidence; 166/166 and through-pa12 clean, file audit passing.

## Completed Checkpoint: CP1 — semantic tree, driver, declarations, statements, expressions without calls

Outcome: `--emit-semantics` now owns an append-only semantic tree and a
canonical scope/type model for declarations, statements, conversions,
constant propagation, function entities, and string-literal facts.  Calls
still throw `runtime_error("call expressions: CP2")`, preserving the next
semantic boundary.

Evidence: `make test-pa12` reports 86/166 (80 remaining failures, all in the
deferred call/class/front-end slices); the 56 CP1 success fixtures pass
byte-exact; `make test-report-through-pa11` reports 657/657; and
`perl scripts/cppgm_file_audit.pl --stage pa12 --paths dev/src` passes.

### Completed Implementation Packet

Files and symbols to create or change:

1. `dev/src/sema/sema_tree.h/.cpp` — `SemaId`, `ValueCategory`, `SemaKind`,
   `SemaNode`, `SemaTree` as in Stage Design (`Make(kind)` returns the id;
   `Append(parent, child)` links; nodes hold `type`, `category`, `op`,
   `binding`, `function`, `scope`, `first/last`, `has_value/value`).
2. `dev/src/sema/scope_model.h/.cpp` — `Binding.scope`, `Binding.function`,
   `Scope.unnamed_namespace`, `FunctionEntity`/`FunctionEntityId`/
   `CreateFunction`/`FunctionAt`, `LookupSet` (unqualified) and
   `LookupQualifiedSet` returning every binding of the name at the level
   found.  Keep `Lookup*` single-result APIs for PA11 callers.
3. `dev/src/sema/type_table.h/.cpp` — `Decay`, `AdjustParameter`,
   `Referent`, `IsIntegral/IsArithmetic/IsScalar/IsPointer`, `Promote`,
   `UsualArithmetic`, `CompositePointer`.  `Spell` unchanged.
4. `dev/src/sema/conversions.h/.cpp` — `ConversionRank`,
   `ImplicitConversion`, `Classify(TypeTable&, source type, source
   category, is_null_literal, is_function_lvalue, target type)`; CP1 uses
   viability (`rank != RANK_NONE`) and `kind` for literal retyping and
   temporary-cast wrapping; `Compare` is written now (pure function) and
   exercised in CP2.
5. `dev/src/sema/expr_sema.h/.cpp` — `ExpressionAnalyzer(tokens, arena,
   model, tree, builder)`: `Analyze(AstId, ScopeId) → SemaId` over
   `AST_LITERAL`, `AST_KEYWORD_LITERAL`, `AST_ID_EXPRESSION`/
   `AST_IDENTIFIER`, `AST_PARENTHESIZED_EXPRESSION` (transparent),
   `AST_UNARY_EXPRESSION`, `AST_POSTFIX_EXPRESSION`, `AST_BINARY_EXPRESSION`
   (text/op token at `node.first`; comma included), `AST_ASSIGNMENT_
   EXPRESSION`, `AST_CONDITIONAL_EXPRESSION`, `AST_SUBSCRIPT_EXPRESSION`,
   `AST_CAST_EXPRESSION` (children: type-id, operand; op = token at
   `node.first`, `OP_LPAREN` for C-style), `AST_SIZEOF_EXPRESSION`/
   `AST_TYPE_TRAIT_EXPRESSION` (operand is a type-id node or an expression;
   an id-expression resolving to a type name counts as a type), and
   `AST_CALL_EXPRESSION` only when the callee id-expression span starts with
   a fundamental type keyword (`IsFundamentalTypeKeyword`), `KW_DECLTYPE`,
   or resolves to a type binding — a functional cast (one argument) or
   value-initialization (`literal prvalue <type> 0`); any other call throws.
   `Initialize(SemaId, TypeId target, InitContext {variable, constexpr,
   condition, return_value, argument})` applies `Classify` and the literal
   retyping/temporary-cast rules.  `TryConstant`/`Value` for callers.
   Remove the walkers from `const_eval.cpp` (keep `Evaluate` as adapter,
   keep `Convert`), remove `BuildExpressionType` and `ConstantOperandTypes`.
6. `dev/src/sema/stmt_builder.cpp` — move and extend `BuildCompound`/
   `BuildStatement` (statement kinds listed in Stage Design, contexts,
   errors, implicit scopes); `BuildFunctionDefinition` creates the
   function-definition node, parameter nodes (canonical types from the
   entity, names from this declarator), and passes the entity's return type
   in the context.  `BuildSimpleDeclaration`/`BuildAlias`/`BuildNamespace`
   append their dump nodes (namespace scope: direct; block scope: under a
   `simple-declaration` node) and run `Initialize` on initializers
   (`AST_INITIALIZER` child is an expression, an `AST_PAREN_INITIALIZER`
   holding one expression, or an `AST_BRACED_INIT_LIST` → `braced-init-list`
   node with element children, each initialized to the element type;
   deduced bound = element count).  Function declarators go through
   `FunctionEntity` matching.  Unnamed local class/enum naming counters.
7. `dev/src/sema/semantics_dump.h/.cpp` — `PrintSemanticsUnit`.
8. `dev/cppgm++.cpp` — `run_emit_semantics_mode` builds `TypeTable`,
   `SemaModel`, `SemaTree`, `ScopeBuilder`, prints per unit; header via
   `PrintHeader`.
9. `dev/src/parser/recog_token.h/.cpp` — `Pa6Token.lit_count`;
   `emit_literal_array` stores `lit_type`, `lit_count`, `lit_scalar=false`.
10. `dev/frontend_source_sets.mk` — add `sema/sema_tree`, `sema/conversions`,
    `sema/expr_sema`, `sema/stmt_builder`, `sema/semantics_dump` to
    `FRONTEND_OBJ_BASENAMES_cppgm++`.

Fixture groups for CP1 (byte-exact): `spec/100-empty`, `100-function-decls`,
`100-local-arith`, `100-pointer-plus-integral`, `200-do-statement`,
`200-for-loop`, `200-sizeof-typeid`, `200-subscript-expression`,
`200-unary-logical-conditional`, `300-nullptr-pointer-conversion`;
`general/100-{assignment-expression, cast-to-void-expression,
floating-functional-cast, integer-zero-to-pointer-local-init,
integer-zero-to-pointer-return, nullptr-static-cast-pointer,
nullptr-to-pointer-return, pointer-equality, pointer-plus-assign,
pointer-postincrement, pointer-relational-compare,
qualified-return-function-pointer-declaration, scoped-enum-to-integral-cast}`,
`200-{bool-conditional-mixed-value-category, comma-bitwise-shift,
constexpr-complete-object-cv, enclosing-namespace-qualified-type-before-
later-shadow, function-pointer-array-deduced-bound,
if-substatement-sibling-declaration-scopes, local-alias-pointer-reference-
declaration, local-alias-postfix-cv-declaration, local-alias-statement,
local-direct-initialization, loop-jumps, sizeof-expression, while-loop}`,
`300-{deref-string-literal, enum-comparisons, floating-arithmetic-
comparisons, floating-conditional-common-type, floating-inc-dec,
floating-literal-classification, integral-compound-assign-unscoped-enum,
nested-functional-pointer-bool-casts, pointer-cv-conditional,
pointer-null-comparisons, pointer-nullptr-conditional,
pointer-plus-anonymous-enum, pointer-subtraction-cv-compatible,
qualified-namespace-value-lookup, qualified-using-directive-value-lookup,
static-cast-enum-to-enum, subscript-commuted-expression,
subscript-unscoped-enum-index, unnamed-namespace-definition,
unscoped-enum-integral-operators}`.  Rejections: `300-bad-{default-outside-
switch, duplicate-function-definition, pointer-integer-equality,
pointer-multiply-assign, scoped-enum-if-condition, void-return-expression}`,
`300-{binding-after-namespace-bad, binding-before-namespace-bad,
break-outside-loop-bad, compound-assignment-rhs-conversion-bad,
conflicting-function-return-bad, continue-outside-loop-bad,
nonconstant-case-label-bad}`, `spec/300-compound-assignment-lvalue-bad`,
and `300-most-vexing-local-function-member-call-bad` (already rejected by
the PA11 builder on `B(A);`).

Required spec facts: 3.10 and 5.1.1p8 value categories; 4.5–4.10 and 4.12
standard conversions; 4p4 contextual bool; 5p9 usual arithmetic
conversions; 5.2.1, 5.2.3 (`T()` value-initialization is a prvalue of T),
5.2.6, 5.2.9, 5.3.1, 5.3.2, 5.3.3 (`sizeof` yields `unsigned long`), 5.4,
5.5–5.18 operand and result rules; 5.19 integral constant expressions
(folding set: literals, enumerators, const integral variables with constant
initializers, the operators above, `sizeof`, casts to integral/enum);
6.4p1–p3, 6.4.2, 6.5.3, 6.6.1–6.6.3; 8.3.5p5 parameter adjustment; 8.5p14
copy-initialization; 8.5.1p4 deduced array bound; 8.5.3p5 reference
binding; 13.1/3.3.10 redeclaration and hiding; 7.1.5p9 constexpr objects
are const.

Commands:

- focused: `make -C dev cppgm++ && for t in pa12/tests/spec/100-local-arith
  pa12/tests/general/200-comma-bitwise-shift pa12/tests/general/200-loop-
  jumps pa12/tests/general/300-pointer-plus-anonymous-enum; do
  dev/cppgm++ --emit-semantics -o /tmp/x.my $t.t && diff /tmp/x.my $t.ref;
  done`; `make test-pa12` for the stage count.
- broad: `make test-report-through-pa11` (must stay 657/657 — PA11 dumps
  byte-identical after the evaluator and expression-typing replacement) and
  `perl scripts/cppgm_file_audit.pl --stage pa12 --paths dev/src`.
- performance probe: generate `/tmp/p.t` with `int f(){ int x=0; <20000×
  "x = x + 1;"> return x; }` and a second with 20000 `int vN = N;` lines,
  time `dev/cppgm++ --emit-semantics` against `--emit-ast` on the same
  files (5 interleaved runs, medians); both must be linear across 10k/20k/
  40k and within noise of the parser-only mode.

Known uncertainties:

- Literal retyping and temporary-cast wrapping are each pinned by one or
  two fixtures (`200-constexpr-complete-object-cv`, `300-reference-binding-
  ranking`); implement exactly the stated rules and no broader folding.
- Cast to a reference type re-labelling the operand (`300-array-xvalue-
  subscript`) is the only reference-cast fixture; keep `cast-expression`
  for every non-reference target.
- The current PA11 parse of `union {…} u;` at block scope names the class
  `u`; PA12 pins `__local_type1` (used in CP4).  Introduce the counter in
  CP1 only for the bare local `enum {…};` case that CP1 prints.
- Division by zero and overflow during folding produce no value rather than
  an error; PA11 fixtures that expect failure must still fail through
  "constant required" paths — verify with through-pa11.
- The dump for `for (;;)` (empty init/condition/iteration), `return;` and an
  empty `expression-statement` has no fixture; print the node with no
  children.

## Completed Checkpoint: CP2 — call expressions, overload sets, and conversion ranking

Outcome: `ExpressionAnalyzer::AnalyzeCall` now owns direct, qualified,
aliased, using-directed, indirect, built-in, variadic, and function-reference
calls. `overload.cpp` selects canonical function entities with bounded
candidate sets, per-argument viability, target-directed function selection,
and strict conversion dominance. Shared conversion logic now covers
qualification, array/function decay, references and temporaries, null-pointer
conversions, and reference-returning call results; variadic parameter packs,
`nullptr_t` lookup/comparison, qualifier precedence, and reference-array
subscripts use the same model.

Evidence: the four packet fixtures and focused reference/conversion regressions
pass; `make test-pa12` reports 153/166 (13 deferred failures, with coverage
unchanged); `make test-report-through-pa11` reports 657/657; the pa12 file
audit passes with its existing header-layout warning; and the five-run
10k/20k/40k probe remains linear (assignment semantics 0.24/0.47/0.90s,
declaration semantics 0.20/0.41/0.82s) and close to `--emit-ast`.

Completed implementation packet: `dev/src/sema/overload.{h,cpp}` and its
frontend source-set entry; `expr_sema.{h,cpp}` call selection, built-ins,
reference results, assignment/subscript handling, and target retargeting;
`conversions.cpp` standard/reference conversion classification;
`type_builder.cpp` variadic packs and `nullptr_t`; and `scope_model.cpp`
lexical qualifier precedence.

## Completed Checkpoint: CP3 — parser/front-end expression and statement boundaries

Outcome: functional-cast roots now retain complete fundamental and `decltype`
type-id spans; leading-global qualified declarations enter the statement
parser; local incomplete array parameters receive the required pointer
adjustment; and declaration semantic parents preserve default-statement tree
ownership without changing lexical lookup.  Qualified class type spellings
are retained through a keyword-aware type cache, while the existing
using-directive and enum presentation behavior remains unchanged.

Evidence: the four packet fixtures pass 4/4 and the two using-directive
regressions pass 2/2.  `make test-pa12` reports 157/166, reducing the
turn-start 13 failures to the nine named CP4 fixtures without changing test
coverage.  `make test-report-through-pa11` reports 657/657.
`perl scripts/cppgm_file_audit.pl --stage pa12 --paths dev/src` passes with
one pre-existing `recog_parser.h` implementation-body warning.

## Completed Checkpoint: CP4 — class/member/constructor semantic ownership

Outcome: class and anonymous-union ownership now drives synthesized storage
and constructor actions, cv-aware member expressions, member-pointer types and
addresses, `this`-first member function entities, class-qualified overload
sets, and selected template-id declarations.  Unused template instances and
class data members stay out of the semantic dump.

Evidence: the nine packet fixtures pass 9/9 and `make test-pa12` reports
166/166; `make test-report-through-pa11` reports 657/657; the pa12 file audit
passes with one pre-existing `recog_parser.h` implementation-body warning; and
the representative anonymous-union semantic probe completes in 0.00 s with
4684 KiB maximum resident memory.

## Active Checkpoint: CP5 — architecture audit and release hygiene

Goal: document the completed PA12 ownership boundaries and verify that the
final semantic implementation remains bounded, typed, and coverage-preserving.

Implementation boundary: review the final `dev/src/sema/` diff against the
stage design, record findings in `pa12/audit.md`, and retain the clean
through-pa12, file-audit, and representative-probe evidence.
