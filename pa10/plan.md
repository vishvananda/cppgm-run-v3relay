# PA10 Plan — cppgm++ --emit-ast (syntax tree dump)

Grading: `cppgm++ --emit-ast -o x.my x.t`; exit status must match
`x.ref.exit_status` (7 fixtures expect EXIT_FAILURE, 150 expect success and
a byte-exact dump).  No `cppgm++-ref` exists: the checked-in `.ref` files
are the only format oracle, so every dump convention below is
fixture-pinned.  Harness budget 10 s per test.  Final review, findings,
performance evidence and validation are in `audit.md`.

## Stage Design

Data flow per translation unit (fresh state per file, files in argv order):
`PreprocEngine::RunSingleFile` (host services from `dev/src/preproc_host`)
→ `Pa6TokenCollector::tokens` (`>>` already split into two pieces) →
`Pa10Parser` builds an arena AST with a scoped declaration table →
`PrintUnit` writes the dump.  Any exception or parse failure → EXIT_FAILURE.

Owning boundaries (all under `dev/src/parser/`; PA6 files untouched):

- `ast_model.h/.cpp` — `AstKind` + `AstKindName()` (the exact dump
  spellings), `AstNode {kind; text; children; first; last}` where
  `[first, last)` is the token range the text was rendered from (zero for
  structural nodes), `AstArena` (append-only, id 0 = null), printer (two
  spaces per depth, line = kind + optional text).  Later PAs consume kind,
  children and token spans; they never split text.
- `ast_scope.h/.cpp` — `SyntaxScopes`: scope stack binding
  `name → {TYPE, TEMPLATE, NAMESPACE, VALUE}` with an undo log;
  `Mark()`/`Rollback(mark)` reports whether a binding was undone;
  `Lookup` innermost-wins; `LookupScopePrefix` skips VALUE (3.4.3p1).
- `ast_parser.h` + `ast_parser.cpp` (primitives, memo, node construction,
  typed name facts, declarations, statements), `ast_parser_decl.cpp`
  (namespaces, classes, enums, templates, specifiers, declarators, names),
  `ast_parser_expr.cpp` (expressions, type traits, new/delete, lambdas).
  Every rule returns a node or 0 after `restore(mark)`; `restore` resets
  position and bracket stack, rolls back bindings and drops the memo when a
  binding was undone.  `parse_declaration` dispatches on the leading
  keyword, tries the qualified special-member definition, then
  `parse_specified_declaration`, which parses the decl-specifier-seq and
  the first declarator once and continues as function body, bit-field
  (member context) or init-declarator-list.  `parse_assignment_expression`
  and `parse_simple_template_id` are memoized on (rule, position,
  `has_angle_boundary()`), failures included; both are net-zero on the
  bracket stack and the undo log.  Brackets PAREN/SQUARE/BRACE/ANGLE make
  `>` close a template-argument list only at an angle boundary.
- `dev/cppgm++.cpp::run_emit_ast_mode` — `-o` and inputs, one arena and
  parser per file, header/unit framing, exceptions → EXIT_FAILURE.
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_cppgm++` lists
  `preproc_host`, the five parser units, `parser/recog_token` and the
  PA1–PA5 pipeline.

Name classification (replaces the PA6 mock letters; fixtures use real names):

- Bindings are made by the owning rule: class and enum names before their
  body, namespace names before their body, template parameters at the
  parameter, typedef/alias/class/enum names → TYPE, namespace and alias →
  NAMESPACE, the name a `template-declaration` declares → TEMPLATE (last
  identifier outside `<...>`, none for operator/destructor names), other
  declarators, enumerators and non-type parameters → VALUE when the
  declaration is accepted.  Only a single identifier token declares a name;
  qualified names and template-ids redeclare entities living elsewhere.
- Parameters bind VALUE in a pushed scope covering function bodies,
  special-member mem-initializers and bodies, and lambda bodies.  Scopes:
  TU, namespace body, class body, function, compound statement, lambda,
  template parameter clause.
- `id` is a type-name when bound TYPE, or unbound and used where a type is
  needed.  7.1.6.2p2: after a committing type-specifier the next identifier
  belongs to the declarator.
- `id <`: VALUE-bound → never a template-id (`__count < _Dt`); otherwise a
  speculative template-id that falls back to the `<` operator, and in an
  expression is re-read as a comparison when an identifier or literal
  follows the closing `>` (`a < b > c`).
- Block item: identifier followed by `:` → labeled-statement; an unknown
  or namespace name whose expression contains a call → expression
  statement; a VALUE-bound name not followed by `::` → statement; otherwise
  declaration first, then statement (6.8: `int(a);` is a declaration;
  `function(&spawned_thread);` with a local VALUE `function` is a call).
- Special members: in a class body, `member-function-specifier* [~]NAME (`
  where NAME is the enclosing class's base identifier, or `operator ...`
  with no decl-specifier-seq.  At namespace scope, `nested-name-specifier
  [~]NAME` where NAME equals the last qualifier's base identifier
  (`Box<T>::Box`, `C<T>::~C`) or `C::operator <type>` (a type must follow
  `operator`; `C::operator+` without a return type fails).
- Attributes (`__attribute__((…))`, `alignas(…)`, `[[…]]`) are consumed
  and never printed.

Dump conventions (fixture-pinned; `Join` = token spellings with one space
only between two adjacent word tokens):

- Leaves: `KW_X:spelling`, `OP_X:spelling`, `TT_IDENTIFIER:x`; `literal
  <spelling>`; `keyword-literal KW_TRUE:true|KW_FALSE|KW_NULLPTR|KW_THIS`;
  `>>` pieces print `OP_RSHIFT:>>`; C-style cast prints `cast-expression
  OP_LPAREN:`.
- Names: `id-expression <Join>`, `identifier <Join>` (declarator-id, member
  name incl. `template get<U>`), `base-name`, `mem-initializer-id`,
  `type-name`, `target`, `ptr-operator C::*`, `placement (Join)`,
  `lambda-introducer [Join]`, `trailing-return-type <Join>`.  Unqualified
  operator names are `"operator"` glued to the rest (`operatorint`,
  `operatornew[]`, `operator""_scale`); qualified forms are plain Join.  A
  `typename` prefix is dropped in specifiers but kept in template
  arguments.
- decl-specifier-seq: keywords `decl-specifier KW_..`; lone identifier
  type `decl-specifier TT_IDENTIFIER:x`; qualified/template-id
  `decl-specifier <Join>`; `decl-specifier decltype(Join)` with the
  expression as child; nested `class-specifier [name]` /
  `enum-specifier [name]` subtrees.  type-specifier-seq (type-id
  contexts): `type-specifier KW_..`, `cv-qualifier KW_CONST:const`,
  `type-name <Join>`, `decltype-specifier decltype(Join)`;
  `type-id{type-specifier-seq, abstract-declarator?}`.
- Declarations: `simple-declaration{decl-specifier-seq,
  init-declarator-list{init-declarator{declarator, initializer{expr |
  braced-init-list | paren-initializer | special-initializer
  default|delete}}}}`; `function-definition{decl-specifier-seq, declarator,
  compound-statement}`; a lone class/enum definition or `struct S;` prints
  the bare `class-specifier` / `enum-specifier` /
  `class-forward-declaration S{class-key}`; `empty-declaration`;
  `alias-declaration NAME{type-id}`; `static-assert-declaration{expr,
  message "..."}`; `namespace-definition NAME|<unnamed>{inline?, decls}`;
  `namespace-alias-definition NAME{target}`; `using-directive{target}`;
  `using-declaration{target}`; `linkage-specification C|C++{decls}`;
  `explicit-instantiation-declaration{decl}`.
- Declarator: `declarator{ptr-operator…, parameter-pack ...?, identifier |
  nested-declarator{declarator}, parameter-clause{parameter-declaration{
  decl-specifier-seq, declarator?, default-argument{initializer{e}}}…,
  parameter-pack ...}, array-suffix{e?}, cv-qualifier, ref-qualifier,
  function-qualifier noexcept | noexcept(Join){e} | throw(...),
  virt-specifier TT_IDENTIFIER:override|final, trailing-return-type
  <Join>{type-id}}`.
- Classes: `class-specifier NAME{class-key, base-clause{base-specifier{
  virtual?, access-specifier?, base-name}}, members…}` with
  `access-specifier` siblings; `bit-field-declaration{decl-specifier-seq,
  bit-field-declarator{declarator?, e}}`; `special-member-declaration|
  definition <name>{member-specifiers{specifier KW_INLINE:inline |
  specifier explicit}?, declarator, ctor-initializer{mem-initializer{
  mem-initializer-id, paren-argument-list|braced-init-list}}?,
  initializer?, compound-statement?}`; `enum-specifier NAME?{enum-key
  KW_CLASS:class?, type-id?, enumerator NAME{e?}…}`.
- Templates: `template-declaration{template-parameter-clause{
  template-parameter-list{…}}, decl}` (explicit specialization: empty
  clause); `type-parameter{template-template-parameter,
  template-parameter-clause}?, parameter-key, parameter-pack?, identifier?,
  default-template-argument{type-id}}`; `non-type-template-parameter{
  decl-specifier-seq, parameter-pack?, declarator?,
  default-template-argument{e}}` where a declarator-less parameter with a
  single keyword type and a single-token literal default prints the token
  (`literal TT_LITERAL:0`); partial specialization → `class-specifier
  NAME<Join>`.
- Statements: `compound-statement`, `expression-statement{e?}`,
  `if-statement{condition, then{s}, else{s}?}`, `while-statement{condition,
  s}`, `do-statement{s, condition}`, `for-statement{for-init-statement{
  decl|e}?, condition?, iteration{e}?, s}`, `switch-statement{condition,
  s}`, `case-statement{e, s}`, `default-statement{s}`, `labeled-statement
  NAME{s}`, `goto-statement NAME`, `break-statement`, `continue-statement`,
  `return-statement{e?}`, `throw-statement{e?}`, `try-block{compound,
  handler{exception-declaration{ellipsis ... | decl-specifier-seq,
  declarator?}, compound}+}`; `condition{e | condition-declaration{
  decl-specifier-seq, declarator, initializer}}`.
- Expressions: `binary-expression OP{l,r}`, `assignment-expression
  OP_ASS:={l,r}`, `conditional-expression{c,a,b}`, `unary-expression
  OP{e}`, `postfix-expression OP_INC:++{e}`, `call-expression{callee,
  argument-list{…}}` (simple-type functional cast: `call-expression{
  id-expression unsigned, paren-argument-list{…}}`),
  `subscript-expression{e,i}`, `member-expression OP_DOT:.|OP_ARROW:->{obj,
  identifier}`, `parenthesized-expression{e}`, `cast-expression
  KW_STATIC_CAST:static_cast{type-id, e}`, `sizeof-expression{type-id|e}`,
  `type-trait-expression KW_TYPEID|KW_ALIGNOF|KW_NOEXCEPT{type-id|e}`,
  `new-expression{global-scope?, placement (Join){paren-argument-list}?,
  type-id, initializer{paren-initializer|braced-init-list}?}`,
  `delete-expression{array-delete?, e}`, `lambda-expression{
  lambda-introducer, lambda-declarator{parameter-clause, lambda-specifier
  KW_MUTABLE:mutable?, noexcept-specification{e}?, trailing-return-type{
  type-id}?}?, compound-statement}`, `braced-init-list{…}`,
  `pack-expansion-expression{e}`.

## Performance Bounds

- Specifiers and the first declarator of a declaration are parsed once;
  class bodies are never re-parsed.
- Expression and template-id speculation is memoized; the memo survives
  every restore that undoes no binding (0 clears on the suite).
- Speculation over `<` chains with unknown left operands is quadratic in
  the number of such `<` in one expression; everything else is linear in
  tokens.  Retained memory is about 4 KB per declaration (tokens + nodes);
  abandoned speculative nodes stay in the arena, bounded by the memo.
- Probes and measurements: `audit.md`.

## Checkpoint Ledger

- CP1 driver, model/printer, scope table, declaration/statement/expression
  core — COMPLETE (58/157; through-pa9 432/432).
- CP2 namespaces, linkage, using/alias, classes, members, special members,
  enums — COMPLETE (94/157).
- CP3 templates, dependent names, explicit instantiation and
  specialization — COMPLETE (143/157).
- CP4 residual closure — COMPLETE (157/157; through-pa9 432/432).
- CP5 final architecture cleanup — COMPLETE (589/589 through pa10; file
  audit passing; evidence and findings in `audit.md`).  No further PA10
  packet is open; preserve the source lists, fixtures, transactional scope
  behaviour and memo discipline when starting PA11.
