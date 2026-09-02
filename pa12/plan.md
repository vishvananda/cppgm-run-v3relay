# PA12 Plan — cppgm++ --emit-semantics (call-semantics dump)

Grading: `cppgm++ --emit-semantics -o x.my x.t`; exit status must match
`x.ref.exit_status` (29 fixtures expect EXIT_FAILURE, 137 expect success and
a byte-exact dump).  No reference binary: the 166 `.ref` files under
`pa12/tests/{spec,general}` are the only format oracle, so every convention
below is fixture-pinned.  `cppgm.tests/course/pa12` is empty.  Harness: the
same forked batch runner as PA11.  `pa12.gram` is identical to `pa11.gram`.
The final architecture review, findings, performance evidence and
validation are in `audit.md`.

## Stage Design (as built)

Data flow per translation unit (fresh state per file, argv order):
`PreprocEngine::RunSingleFile` → `Pa6TokenCollector::tokens` → `Pa10Parser`
(arena AST, dump unchanged) → `ScopeBuilder(tokens, arena, model,
tree).Build(root)` — one in-order pass that declares into the `SemaModel`
(PA11, unchanged output) and analyzes every initializer, condition,
expression statement and return as it is reached, appending typed nodes to
a `SemaTree` → `PrintSemanticsUnit`.  Because the pass is in source order,
unqualified lookup sees only earlier declarations (fixture
`300-namespace-function-body-later-anonymous-overload` depends on this); no
position stamps and no second pass.  Any exception → EXIT_FAILURE.  The
`--emit-types` builder is the same class without a dump tree: its constant
and `decltype` analyses run through the same analyzer over a scratch tree
and are released once read.

Owning boundaries (`dev/src/sema/`; every `.cpp` is listed under
`FRONTEND_OBJ_BASENAMES_cppgm++` in `dev/frontend_source_sets.mk`):

- `sema_tree.h/.cpp` — `SemaId` (0 = null), `ValueCategory`, `SemaKind`
  (one enumerator per dump line kind), `SemaNode {kind, category, op
  (ETokenType of the operator or cast token; KW_AUTO = none), has_value,
  type, binding, function, scope, first/last token span, value, intrusive
  child links}`, `SemaTree {Make, Append, At, Root, Mark, Truncate}`.  No
  per-node strings or vectors; every spelling is rendered at print time.
- `qualified_name.h/.cpp` — `QualifiedName {global, components,
  template_id, template_first/last}` read once from a token span;
  template-ids only for expression names.
- `scope_model.h/.cpp` — scopes (bindings and children in declaration
  order, `class_entity`, `inline_namespaces`, `using_directives {nominated,
  apply_at}` with the 7.3.4p2 application point computed once), bindings
  (`name, kind, type, scope, namespace_scope, function, object_binding,
  const value`), class/enum/function entities (`FunctionEntity {scope, name,
  canonical type, member facts, template parameter types, defined}`).  One
  3.4.1 walk (`WalkUnqualified`) behind `LookupSet`, `LookupUnqualified`
  and `LookupTypeName`; one qualifier resolution behind `LookupQualified`
  and `LookupQualifiedSet`; `DirectBindings` from the per-scope index.
- `type_table.h/.cpp` — interned derived types; `Decay`, `AdjustParameter`,
  `Referent`, `IsIntegral/IsArithmetic/IsScalar/IsPointer/
  IsNullPointerType`, `Promote`, `UsualArithmetic`, `CompositePointer`;
  `TYPE_MEMBER_POINTER` and const member-function types.
- `conversions.h/.cpp` — `ImplicitConversion {rank, kind, qualification,
  reference binding, tie-break facts}`, `Classify` (4.1–4.12 plus 8.5.3)
  and `Compare` (13.3.3.2), pure over `TypeTable`.
- `overload.h/.cpp` — `SelectBestOverload` (13.3.3: sorted entity set,
  per-argument classification, linear best-viable selection),
  `SelectTargetFunction` (13.4), `CallableFunctionType`.
- `expr_sema.h/.cpp` — `ExpressionAnalyzer`: the one expression
  implementation (typing, value categories, bottom-up integral folding with
  4.7 conversions, literals from `Pa6Token` facts, names through the lookup
  sets, operators 5.2–5.18, casts, `sizeof`/`alignof`, functional casts and
  value-initialization, calls and built-ins, `Initialize` for the
  copy-initialization boundary, `DeclaredType` for `decltype`).
- `scope_builder.h/.cpp`, `type_builder.cpp`, `stmt_builder.cpp` —
  declarations, function entities (8.3.5p5 canonical signatures, 13.1
  redeclaration matching), class/enum/anonymous-union ownership and
  constructor actions, template instances, specifier/declarator types
  (`deduced_bound` for `T a[] = {…}`), statement scopes and jump validity.
- `semantics_dump.h/.cpp` — `PrintSemanticsUnit`: pre-order, two spaces per
  depth; names from spans or bindings, operators from tokens or the
  operator enum, function names streamed from the entity's scope chain.
- `dev/cppgm++.cpp::run_emit_semantics_mode` — mirrors
  `run_emit_types_mode` with a `SemaTree` per file.
- `parser/recog_token.h/.cpp` — `Pa6Token.lit_count` for string literals.
  `parser/ast_parser*.cpp` — functional casts spanning a full fundamental
  type-id or `decltype(...)`; statements starting with `::`.

Dump conventions (fixture-pinned; implicit conversions never print a node):

- Namespace scope: `type-alias N T`, `variable N T` (+ initializer child),
  `function-declaration Q T` (no children; every redeclaration prints),
  `function-definition Q T` then `parameter <name> <canonical parameter
  type>` (unnamed → `parameter  T`) then `compound-statement`;
  `namespace-definition <name|<unnamed>>` with children.  Class, enum,
  template, using, alias-of-namespace, static_assert and linkage
  declarations print nothing (linkage blocks are transparent).  Member
  function definitions print after all namespace-scope nodes with
  `parameter this pointer to <class>` first; used template instances print
  last as `function-declaration` nodes with `parameter` children.
- Block scope: `simple-declaration` wrapping `variable`/`type-alias`/
  `function-declaration` nodes (a bare `enum {…};` prints an empty
  `simple-declaration`); `using U = int;` prints `type-alias` unwrapped;
  `union {…} u;` names its class `__local_type<N>`, a bare local
  `enum {…};` its enumeration `__anonymous_enum<N>`; a bare local anonymous
  union prints its synthesized storage variable with a
  `constructor-action`.
- Statements: `expression-statement`, `return-statement`, `if-statement` /
  `condition` / `then` / `else`, `while-statement` / `condition` / body,
  `do-statement` / body / `condition`, `for-statement` / `for-init-statement`
  / `condition` / `iteration` / body, `switch-statement` / `condition` / body,
  `case-statement` label statement, `default-statement`, `break-statement`,
  `continue-statement`, `condition-declaration` / `variable` (+ initializer).
- Expressions: `<kind> <category> <type> [<extra>]`.  `id-expression lvalue
  <declared type; references print their referent; parameters their
  adjusted type> <source spelling>`; function names print their canonical
  function type; enumerators print as `literal prvalue <enum type> <decimal
  value>`.  `literal prvalue <type> <spelling>`; a null pointer constant `0`
  copy-initialized to a pointer prints the unqualified target pointer type
  (to `nullptr_t`: `nullptr_t`); a `constexpr` integral variable's literal
  initializer prints the variable's cv-qualified type; neither happens
  inside `?:`.  `unary-expression`, `postfix-expression`,
  `binary-expression`, `assignment-expression <lvalue> <lhs referent type>
  OP_..` carry `<TOKEN>:<spelling>`; `conditional-expression`,
  `subscript-expression` (array/pointer child first — `1[a]` is commuted),
  `sizeof-expression prvalue unsigned long int` (no children),
  `cast-expression prvalue <type> [<op>]` (C-style `OP_LPAREN:`; a cast to a
  reference type re-labels the operand instead), `size_t()` → `literal
  prvalue <type> 0`, `member-expression lvalue <cv member type> OP_DOT:x`
  (injected members print the bare name).  `call-expression <category>
  <type>` (return type as declared; reference returns lvalue/xvalue) with
  `callee <qualified name> <canonical type>` or the callee expression, then
  the arguments; a temporary bound to a reference parameter through a
  promotion or conversion prints `cast-expression prvalue <cv referent>`
  around the argument.  `braced-init-list lvalue <array type>` with element
  children.

Semantic rules (N3485): 3.10/5.1.1p8 value categories; 5p9 with 4.5/4.7
promotions; 5.3.1, 5.2.6, 5.7, 5.9/5.10, 5.14–5.18, 5.2.1, 5.2.2,
5.2.3/5.2.9/5.4, 5.3.3 operand and result rules; 5.19 folding (literals,
enumerators, const integral objects, operators, `sizeof`, casts; undefined
behaviour yields no value); 4.10 null pointer constants (zero literal or
`nullptr`, never an enumerator); 4.4 recursive qualification conversions;
8.5.3 reference binding; 13.3.3.2 ranking; 13.3.3p1 best viable; 13.4
target-directed selection; 8.3.5p5 signatures and 13.1/3.3.10
redeclaration; 3.4.1 with 7.3.4p2/p4/p6 directive application; 3.4.3.2p2
qualified lookup ignoring directives when the namespace declares the name;
6.4–6.6 statement scopes and jump validity; 6.4.2 integral case labels;
7.1.6.2p4 `decltype`.

## Failure Map (at planning)

At CP1 start every fixture reported EXIT_NOT_IMPLEMENTED; `--emit-ast`
rejected 3 inputs (functional casts spanning `decltype`/scoped-enum
type-ids, a local `extern` function declaration) and `--emit-types` a
further 12 (deduced array bound, block-scope directive application,
`decltype` of a call, `nullptr_t`, member pointers, and two expected
rejections).  Groups by first blocking feature: 56 success and 15 failure
fixtures without calls (CP1); 68 success and 14 failure fixtures needing
calls, overload sets and ranking (CP2); 4 parser/front-end fixtures (CP3);
9 class-aware fixtures — anonymous/local unions with constructor actions,
member expressions, member pointers, `this`-first member functions,
static_cast over overloaded member/template names, appended instantiation
declarations (CP4).

## Performance Design

One in-order walk: O(AST) node visits, each expression analyzed once and
folded bottom-up; transient analyses are released.  Lookup per name:
O(scope depth) index probes plus each nominated namespace once.  Overload
resolution per call: O(candidates × arguments) classification and O(viable)
selection; the candidate set is one name's index vector.  Redeclaration
matching: O(same-name bindings).  Class membership and inline-namespace
membership are O(1) typed back-references.  `SemaTree` is an append-only
vector with intrusive links; the dump is linear in nodes; qualified names
and type spellings render at print time.  Evidence: `audit.md`.

## Checkpoint Ledger

- CP1 (`b4ffb8820`) semantic tree, driver, declarations, statements,
  expressions without calls, conversions, function entities,
  string-literal token facts — 86/166; through-pa11 657/657; file audit
  passing.
- CP2 (`a1d3b816a`) calls, overload sets, ranking, target-directed
  selection, indirect calls, built-ins, `decltype` over calls — 153/166.
- CP3 (`1071e5663`) parser/front-end expression and statement boundaries,
  using-directive application point, incomplete array parameters — 157/166.
- CP4 (`28c7fd605`) class-aware fixtures: anonymous/local unions with
  constructor actions, member expressions, member pointers, `this`-first
  member functions, template-id declarations — 166/166.
- CP5 (final commit) architecture audit and cleanup: one expression
  implementation for both modes, one lookup implementation with the
  7.3.4p2 application point, typed template-ids and template parameters,
  string-free semantic nodes, linear declaration/membership/overload paths
  — 166/166, through-pa12 823/823, file audit passing; findings, changes
  and evidence in `audit.md`.
