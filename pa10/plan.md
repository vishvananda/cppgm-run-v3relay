# PA10 Plan — cppgm++ --emit-ast (syntax tree dump)

State at planning: 0/157 pa10 fixtures pass (every test returns
EXIT_NOT_IMPLEMENTED from the driver stub); through-pa9 green; file audit
passes. Grading: `cppgm++ --emit-ast -o x.my x.t`; exit status must match
`x.ref.exit_status` (7 fixtures expect EXIT_FAILURE, 150 expect success and a
byte-exact `x.my` == `x.ref`). Stdout/stderr ignored. Harness batch timeout
10 s per test. No `cppgm++-ref` exists for PA10: the checked-in `.ref` files
are the only format oracle, so every dump convention below is fixture-pinned.

## Stage Design

Data flow per translation unit (fresh state per file, files in argv order):
`PreprocEngine::RunSingleFile` (as `dev/recog.cpp::DoRecog`, PA5 file-id
callback, build stamp) → `Pa6TokenCollector::tokens` (`>>` already split
into `PA6_RSHIFT_1/2` pieces; ignore the letter-category `flags`) →
`Pa10Parser` builds an arena AST with a scoped declaration table →
`AstPrinter` writes the dump. Any exception or parse failure → EXIT_FAILURE.

Owning boundaries (all new code under `dev/src/parser/`; PA6 files untouched):

- `ast_model.h/.cpp` — `AstKind` enum + `AstKindName()` table (the exact dump
  spellings, e.g. `simple-declaration`, `cast-expression`), `AstNode {kind;
  std::string text; std::vector<AstId> children; size_t first, last;}`,
  `AstArena` (append-only `vector<AstNode>`, id 0 = null), `AstPrinter`
  (`"<n> translation units"`, `start translation unit <k>` … `end translation
  unit`, root `translation-unit`, two spaces per depth, line = kind + (text
  empty ? "" : " " + text)). Later PAs consume `AstKind`/children, never text.
- `ast_scope.h/.cpp` — `SyntaxScopes`: stack of scopes, binding
  `name → {TYPE, TEMPLATE, NAMESPACE, VALUE}` with an undo log;
  `Mark()`/`Rollback(mark)` (rollback that undoes ≥1 binding clears the
  parser memo), `Lookup(name)` innermost-wins, `LookupScopePrefix(name)`
  ignores VALUE bindings (3.4.3p1, name before `::`).
- `ast_parser.h` + `ast_parser.cpp` (core, names, declarators, statements),
  `ast_parser_decl.cpp` (namespaces/classes/enums/templates),
  `ast_parser_expr.cpp` (expressions, type-ids, lambdas, new/delete) —
  `Pa10Parser(tokens, arena)`; every rule is `AstId parse_x()` returning 0 on
  failure after `restore(pos, brackets, scope mark)`. Keeps the PA6
  discipline verbatim: `pos_`, `brackets_` (PAREN/SQUARE/BRACE/ANGLE),
  `has_angle_boundary()` so `>` closes a template-argument list instead of
  being relational, `angle_refusal_`, `hard_failure_` for a committed
  template-id that cannot close, memo `(rule, pos, angle_refusal) → {end,
  node}` for simple-template-id, expression, constant-expression, type-id,
  template-argument, decl-specifier-seq, declarator, statement. Abandoned
  speculative nodes stay in the arena (bounded by parse work).
- `dev/cppgm++.cpp::run_emit_ast_mode` — replace the NotImplemented throw:
  collect `-o` and inputs (validation already exists), parse each file into
  its own arena/scopes, print, write outfile; exceptions → EXIT_FAILURE.
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_cppgm++ :=
  parser/ast_parser parser/ast_parser_decl parser/ast_parser_expr
  parser/ast_scope parser/ast_model parser/recog_token preproc_engine
  macro_replace ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables
  unicode`.

Name classification (replaces the PA6 mock letters; fixtures use real names):

- Binding commits happen only when a whole declaration is accepted:
  typedef/alias/class/enum names → TYPE; namespace and namespace-alias →
  NAMESPACE; the name declared by a `template-declaration` → TEMPLATE;
  variables, functions, parameters, enumerators, non-type template
  parameters → VALUE; type and template-template parameters → TYPE/TEMPLATE
  in a scope that wraps the templated declaration. Class name binds at `{`
  so members can name it. Scopes: TU, namespace body, class body, function
  (parameters + body), compound statement, lambda, template clause.
- `id` is a type-name when bound TYPE, or unbound and used where the grammar
  needs a type. 7.1.6.2p2: after a committing type-specifier the next
  identifier belongs to the declarator (PA6 `parse_decl_specifier_seq_impl`).
- `id <`: TEMPLATE-bound → committed simple-template-id (hard failure if the
  angle list cannot close); VALUE-bound → never a template-id
  (`__count < _Dt`); unbound → speculative template-id, plain backtrack to
  the `<` operator (`foo<int>(x)` vs `x < 1`).
- `id ::` root: `LookupScopePrefix`; unbound accepted (`N::f`, `alias::` with
  `int alias` shadowing still qualifies — fixture 300-namespace-alias…).
- Block item: identifier followed by `:` → labeled-statement; leading
  VALUE-bound identifier not followed by `::` → statement directly; otherwise
  try declaration (with scope mark) then statement (6.8: `int(a);` is a
  declaration; `function(&spawned_thread);` with a local VALUE `function` is
  a call — fixture 300-declaration-statement-ambiguity).
- Special members: in a class body, `member-function-specifier* [~]NAME (`
  where NAME equals the enclosing class's base identifier, or `operator`
  conversion/operator id, with no decl-specifier-seq → special-member-
  declaration/definition. At namespace scope, `nested-name-specifier [~]NAME`
  where NAME equals the last qualifier's base identifier (`Box<T>::Box`,
  `C<T>::~C`) or a conversion-function-id (`C::operator int`, fixture-added
  to the grammar's qualified form) → special-member-definition; a qualified
  `operator+` without return type must fail (200-qualified-nonconversion-…).
- Attributes are consumed and never printed: `__attribute__((…))`,
  `alignas(…)`, `[[…]]` wherever PA6 accepted attribute-specifiers.

Dump conventions (fixture-pinned; `Join` = token spellings concatenated with
one space only between two adjacent word tokens — identifiers/keywords):

- Leaves: `KW_X:spelling`, `OP_X:spelling`, `TT_IDENTIFIER:x`; `literal
  <source spelling>`; `keyword-literal KW_TRUE:true|KW_FALSE|KW_NULLPTR|
  KW_THIS`; the two `>>` pieces print `OP_RSHIFT:>>`; C-style cast prints
  `cast-expression OP_LPAREN:` (nothing after the colon).
- Names: `id-expression <Join>`, `identifier <Join>` (declarator-id, member
  name incl. `template get<U>`), `base-name`, `mem-initializer-id`,
  `type-name`, `target`, `ptr-operator C::*`, `placement (Join)`,
  `lambda-introducer [Join]`, `trailing-return-type <Join>` (function
  declarator only; bare in lambda-declarator). Unqualified operator-/
  conversion-function-id = `"operator" + Join(rest)` with no space
  (`operatorint`, `operatorVal<value_type>`, `operatornew[]`,
  `operator""_scale`, `operator->`); qualified forms are plain Join
  (`C::operator int`). A `typename` prefix is dropped in a decl-specifier
  (`decl-specifier alloc<Y>::type`) but kept inside template arguments.
- decl-specifier-seq children: keywords `decl-specifier KW_..`; lone
  identifier type `decl-specifier TT_IDENTIFIER:x`; qualified/template-id
  `decl-specifier <Join>`; `decl-specifier decltype(Join)` with the
  expression as child; nested `class-specifier [name]` / `enum-specifier
  [name]` subtrees (elaborated `enum kind` → `enum-specifier kind`, no
  children). type-specifier-seq (type-id contexts): `type-specifier KW_..`,
  `cv-qualifier KW_CONST:const`, `type-name <Join>`, `decltype-specifier
  decltype(Join)` + child; `type-id{type-specifier-seq, abstract-declarator?}`.
- Declarations: `simple-declaration{decl-specifier-seq, init-declarator-list
  {init-declarator{declarator, initializer{expr | braced-init-list |
  paren-initializer{args} | special-initializer default|delete}}}}`;
  `function-definition{decl-specifier-seq, declarator, compound-statement}`;
  a declaration that is only a class/enum definition + `;` prints the bare
  `class-specifier NAME?` / `enum-specifier NAME?`; `struct S;` →
  `class-forward-declaration S{class-key}`; `empty-declaration`;
  `alias-declaration NAME{type-id}`; `static-assert-declaration{expr,
  message "..."}`; `namespace-definition NAME|<unnamed>{inline?, decls}`;
  `namespace-alias-definition NAME{target}`; `using-directive{target}`;
  `using-declaration{target}`; `linkage-specification C|C++{decls}` (quotes
  stripped); `explicit-instantiation-declaration{decl}`.
- Declarator: `declarator{ptr-operator OP_STAR:*…, identifier | nested-
  declarator{declarator}, parameter-clause{parameter-declaration{decl-
  specifier-seq, declarator|abstract-declarator?, default-argument{
  initializer{e}}}…, parameter-pack ...}, array-suffix{e?}, cv-qualifier,
  function-qualifier noexcept | noexcept(Join){e}, virt-specifier
  TT_IDENTIFIER:override|final, trailing-return-type <Join>{type-id}}`.
- Classes: `class-specifier NAME{class-key KW_STRUCT:struct, base-clause{
  base-specifier{virtual KW_VIRTUAL:virtual?, access-specifier KW_PUBLIC:
  public?, base-name}}, members…}`; access labels are `access-specifier`
  siblings; `bit-field-declaration{decl-specifier-seq, bit-field-declarator{
  declarator?, e}}`; `special-member-declaration|definition <name>{
  member-specifiers{specifier KW_INLINE:inline | specifier explicit}?,
  declarator, ctor-initializer{mem-initializer{mem-initializer-id,
  paren-argument-list|braced-init-list}}?, compound-statement}`; enums:
  `enum-specifier NAME?{enum-key KW_CLASS:class?, type-id?, enumerator
  NAME{e?}…}`.
- Templates: `template-declaration{template-parameter-clause{template-
  parameter-list{…}}?, decl}` (explicit specialization: empty clause);
  `type-parameter{template-template-parameter, template-parameter-clause}?,
  parameter-key KW_CLASS:class, parameter-pack ...?, identifier?, default-
  template-argument{type-id}}`; `non-type-template-parameter{decl-specifier-
  seq, parameter-pack ...?, declarator?, default-template-argument{e}}`;
  partial specialization → `class-specifier NAME<Join>`.
- Statements: `compound-statement`, `expression-statement{e}`, `if-statement
  {condition, then{s}, else{s}?}`, `while-statement{condition, s}`,
  `do-statement{s, condition}`, `for-statement{for-init-statement{decl|e}?,
  condition?, iteration{e}?, s}`, `switch-statement{condition, s}`,
  `case-statement{e, s}`, `default-statement{s}`, `labeled-statement NAME{s}`,
  `goto-statement NAME`, `break-statement`, `continue-statement`,
  `return-statement{e?}`, `throw-statement{e?}`, `try-block{compound,
  handler{exception-declaration{ellipsis ... | decl-specifier-seq,
  declarator|abstract-declarator?}, compound}+}`; `condition{e |
  condition-declaration{decl-specifier-seq, declarator, initializer}}`.
- Expressions: `binary-expression OP{l,r}` (every binary incl. `.*`, `->*`,
  `&&`), `assignment-expression OP_ASS:={l,r}`, `conditional-expression
  {c,a,b}`, `unary-expression OP{e}`, `postfix-expression OP_INC:++{e}`,
  `call-expression{callee, argument-list{…}}` but a simple-type keyword
  functional cast is `call-expression{id-expression unsigned,
  paren-argument-list{…}}`, `subscript-expression{e,i}`, `member-expression
  OP_DOT:.|OP_ARROW:->{obj, identifier}`, `parenthesized-expression{e}`,
  `cast-expression KW_STATIC_CAST:static_cast{type-id, e}`, `sizeof-
  expression{type-id|e}`, `type-trait-expression KW_TYPEID|KW_ALIGNOF|
  KW_NOEXCEPT{type-id|e}`, `new-expression{global-scope?, placement (Join){
  paren-argument-list}?, type-id, initializer{paren-initializer|braced-init-
  list}?}`, `delete-expression{array-delete?, e}`, `lambda-expression{
  lambda-introducer, lambda-declarator{parameter-clause, lambda-specifier
  KW_MUTABLE:mutable?, noexcept-specification{e}?, trailing-return-type{
  type-id}?}?, compound-statement}`, `braced-init-list{…}`,
  `pack-expansion-expression{e}` for `e...` arguments.

## Failure Map

All 157 fail at the driver stub; the map is by the boundary whose completion
turns them.

- F0 driver + printer + empty TU: spec/100-empty (1).
- F1 declaration/statement/expression core with real-name spelling (Join,
  qualified ids, speculative template-ids, typedef/VALUE scopes): spec
  100-decl, 100-empty-decl, 100-main, 100-params, 100-nested-declarator,
  100-array-declarator, 100-storage-specifiers, 100-switch-try,
  300-declaration-statement-ambiguity, 300-template-id-less-expression
  (needs function-template binding: falls to CP3), 300-type-id-expression-
  contexts; general 100-block-items, 100-if-else, 100-while-assign,
  100-for-postfix, 100-inc, 100-operators-pm, 100-conditional-sizeof,
  100-rshift-piece-normalization, 100-c-style-cast-expression,
  100-typedef-style-unary-cast, 100-qualified-id-call, 100-decl-initializers,
  100-catch-declaration, 100-static-assert, 100-typedef-for-init-declaration,
  100-function-pointer-typedef-parameter, 100-lambda-cast,
  100-new-delete-traits, 100-structured-type-id, 100-function-traits,
  100-template-condition, 100-extern-cxx-linkage, 200-cast-parenthesized-*,
  200-condition-expression-call-chain-not-declaration, 200-conditional-
  simple-type-shift-return, 200-builtin-function-style-cast-expression,
  200-literal-operator-id, 200-allocation-array-operator-ids,
  200-parenthesized-new-type-vs-placement, 200-placement-new-*,
  200-lambda-declarator, 200-function-virt-and-noexcept-suffixes,
  200-empty-brace-scalar-init, 200-member-pointer-* (≈45).
- F2 namespaces, classes, enums: spec 100-namespace-forms,
  200-bit-field-declaration, 200-class-bases-and-ctor-init; general
  100-member-declarations, 100-special-member-definitions,
  100-typedef-struct-union, 100-typedef-anonymous-enum/union,
  100-scoped-enum-underlying-type, 200-elaborated-enum-member-declarators,
  200-enum-initializer-list, 200-extern-c-*, 200-global-qualified-pointer-
  conversion, 200-qualified-conversion-operator-definition,
  200-zero-width-bit-field-declaration, 300-local-typedef-shadows-…,
  300-namespace-alias-shadow-… (≈25).
- F3 templates: spec 100-template-class, 100-template-parameters,
  200-explicit-instantiation-declaration, 200-explicit-specialization-syntax,
  200-non-type-template-parameters, 200-qualified-special-member-definition;
  general 100-member-operator-name-call, 100-decltype-qualified-id-
  expression, 100-class-alignas-after-class-key, 200-dependent-*,
  200-partial-specialization-*, 200-attributed-*, 200-template-*,
  200-inline-namespace-template-visibility-base, 200-member-template-
  parameter-value-vs-template-name, 200-forward-unknown-nested-template-in-
  ctor-body, 200-using-*-imported-template-id-type (≈75).
- F4 negative fixtures (exit status only): 100-bad, 200-malformed-function-
  parameter-list, 200-malformed-template-parameter-clause, 200-class-
  definition-missing-semicolon-bad, 200-bad-condition-declaration (condition
  declaration requires an initializer), 200-unsupported-co-return-bad
  (identifier followed by a literal is neither declaration nor expression),
  200-qualified-nonconversion-operator-missing-return-type-bad (7). These
  pass as soon as the parser is strict; they must never regress.
- Quirks pinned by single fixtures (resolve in CP4, no special-casing of
  test names): `specifier explicit` is bare while other member-function-
  specifiers print `KW_X:x`; `literal TT_LITERAL:0` for the default of a
  declarator-less non-type parameter whose type is a keyword (`int = 0`),
  while `int N = 1` and `__enable_if_t<…> = 0` print `literal 1|0`.

## Performance Risks

- Declaration-then-statement retries nested in parentheses, argument lists
  and lambdas can multiply. Bound: memoize declarator/statement/expression/
  type-id at (rule, pos); PA6's committed template-id hard failure; skip the
  declaration attempt for a leading VALUE identifier.
- Speculative `id <` template-argument scans on unbound names: bounded by
  the template-argument memo and by angle-boundary closing; never rescan the
  same `<` twice in one context.
- Scope rollback clears the memo: only on failed declarations that bound
  names (class bodies inside a failed decl-specifier-seq attempt); acceptable.
- Arena growth is O(attempted parse work); nodes are small (enum, string,
  vector, two indices). Fixtures are ≤ 31 lines; target ≤ 20 ms per test.
- Probe: `time make -C pa10 test` (whole suite) plus a synthetic file with
  200 nested parentheses `((((x))))`, a 60-deep `a<b<c<…>>>` type-name, and
  a 500-statement function body; all must finish well under 1 s.

## Checkpoint Ledger

- CP1 — driver, AST model/printer, scope table, and the declaration/
  statement/expression core (F0 + F1) (COMPLETE). Evidence: `make test-pa10`
  reports 58/157, with all 7 negative fixtures passing and no successful
  fixture producing a mismatched dump; the turn-start failure count fell from
  157 to 99. `make test-report-through-pa9` reports 432/432. The pa10 file
  audit passes (one pre-existing `recog_parser.h` warning), and the 200-level
  parenthesis and 60-level angle probes each complete in 0.00s.
- CP2 — namespaces, linkage, using/alias forms, class-specifier with bases,
  members, access labels, bit-fields, special members, ctor-initializers,
  enums (F2) (COMPLETE). Evidence: the packet and Failure Map F2 fixture
  sweep passes 18/18; `make test-pa10` reports 94/157, reducing the current
  stage failures from 99 to 63 without removing coverage. The required
  `make test-report-through-pa9` reports 432/432, and the pa10 file audit
  passes with one pre-existing `recog_parser.h` warning. The representative
  timed suite invocation completes in 0.35s before the first known CP3
  fixture stops that non-keep-going harness.
- CP3 — template-declaration, parameter forms, TEMPLATE bindings,
  `typename`/`template` dependent names, explicit instantiation and
  specialization, partial specializations, qualified special-member
  definitions (F3). Target ≥ 150/157 (ACTIVE).
- CP4 — closure: remaining quirks, performance probes, audit warnings,
  `make test-report-through-pa10` green; record results in this file.

## Active Checkpoint — CP3: templates and dependent declarations

Goal: extend the 94/157 CP2 baseline through the template/dependent-name
fixtures without changing the completed F0–F2 dump contracts or scope
rollback behavior.

### Implementation Packet

Files and symbols:

- `dev/src/parser/ast_model.h/.cpp`: extend the existing kind table only when
  a CP3 fixture requires a template/dependent-declaration spelling.
- `dev/src/parser/ast_scope.h/.cpp`: preserve transactional `Push`/`Pop`,
  undo-log rollback, and the completed TYPE/NAMESPACE ownership while adding
  TEMPLATE and template-clause bindings.
- `dev/src/parser/ast_parser.h`, `ast_parser.cpp`, and
  `ast_parser_decl.cpp`: implement the template declaration, parameter,
  explicit instantiation/specialization, dependent-name, and qualified
  special-member productions named by the Failure Map; retain the completed
  namespace/class/enum/member productions.
- `dev/src/parser/ast_parser_expr.cpp`: only extend dependent/template-id
  expression ownership where a CP3 fixture requires it.
- `dev/frontend_source_sets.mk`: keep the PA10 parser/model/scope source set
  complete; do not add PA6/PA7 sources to the cppgm++ target.

Fixture groups: `spec/100-template-class`, `spec/100-template-parameters`,
`spec/200-explicit-instantiation-declaration`,
`spec/200-explicit-specialization-syntax`,
`spec/200-non-type-template-parameters`,
`spec/200-qualified-special-member-definition`, and the F3 template,
dependent-name, attribute, and imported-using fixtures listed in the Failure
Map. Preserve all F0–F2 fixtures and negatives.

Exit evidence: the pa10 stage pass count must increase without removing or
weakening any fixture; run `make test-pa10`, `make test-report-through-pa9`,
and the pa10 file audit after source is stable.
