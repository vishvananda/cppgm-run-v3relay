# PA10 Final Architecture Audit — cppgm++ --emit-ast

## Scope and method

Traced representative syntax facts through their full ownership paths:
srcfile → PA5 `PreprocEngine::RunSingleFile` (host services from the shared
`dev/src/preproc_host.cpp`) → `Pa6TokenCollector::tokens` (typed kinds,
`>>` pre-split into two pieces) → `Pa10Parser` (recursive descent, one
transactional `Mark`/`restore` discipline, `SyntaxScopes` name
classification with an undo log, memo keyed on rule/position/angle
boundary) → `AstArena` nodes (kind, dump text rendered once at the owning
rule, token span, children) → `PrintUnit` → outfile in the `dev/cppgm++.cpp`
envelope.  Facts followed end to end: a class name (bound at `{`, printed as
the specifier text, recovered for template binding from its token span), a
declarator-id (span → single-token `declared_identifier` → VALUE/TYPE
binding), a speculative `id <` (memoized template-id, comparison fallback),
a typedef (typed keyword check on the specifier span), and the
declarator-less non-type template default (`TT_LITERAL:0` fixture rule).

Checked scaling with generated inputs: nested class-with-declarator,
nested member classes and member functions, nested lambdas in assignment
statements, `<` chains with unknown and with bound names, deep
parentheses, deep template-ids, long bodies, long declaration lists, deep
`if/else` and call nesting.  Compared the pre-cleanup and final executables
(immutable copies) on 251 checked-in `.t` inputs from pa6, pa7, pa8 and
pa10 plus 52 probes, and timed representative probes with interleaved runs
and medians.  Corroborated timing with a temporary counter build (memo
hits/misses/clears, nodes allocated versus reachable); the counters were
reverted and the rebuilt binary is byte-identical to the shipped one.

## Ownership trace (final)

- **Token facts**: `Pa6Token` owns kind, simple type, spelling and the
  final/override/zero flags; the parser never re-derives a fact from a
  spelling except for name-equality against the enclosing class name.
- **Syntax facts**: every rule returns a node or 0 after restoring its
  mark.  `parse_declaration` dispatches on the leading keyword, tries the
  qualified special-member definition, then `parse_specified_declaration`,
  which parses the decl-specifier-seq and the first declarator exactly
  once and chooses function body, bit-field (class bodies only) or
  init-declarator continuation from the next token.  A lone
  class/enum/forward specifier followed by `;` is returned as the
  specifier itself.
- **Name facts**: `SyntaxScopes` binds TYPE/TEMPLATE/NAMESPACE/VALUE at the
  owning rule (class and enum names before their body, namespace names
  before their body, template parameters at the parameter, everything else
  when the declaration is accepted).  Parameters bind in a pushed scope for
  function, special-member and lambda bodies (and mem-initializers).  The
  declared identifier of a declarator is read from the declarator-id's
  token span and only a single identifier token binds; a template name is
  the last identifier outside `<...>` of the declared name, none for
  operator and destructor names.  `restore()` rolls back bindings and
  drops the memo when a binding was undone.
- **Presentation facts**: every node whose dump line carries text records
  the token range it was rendered from (`AstNode::first/last`); structural
  nodes carry none.  Later passes classify names from those spans and
  never split joined text.  The dump conventions themselves are listed in
  `plan.md`.
- **Envelope**: `dev/cppgm++.cpp` owns argv validation, per-file arena and
  parser, header/unit framing, and the failure exit.  Host file identity
  and the build stamp live once in `dev/src/preproc_host.cpp`, shared by
  preproc, recog, nsdecl, nsinit, cy86 and cppgm++.

## Findings and changes (this cleanup)

1. **Exponential re-parse of decl-specifier-seqs (material, fixed)**: a
   class body followed by a declarator was parsed by the bit-field attempt,
   the bare-class dispatch, the function-definition attempt and finally the
   simple-declaration, about four times per nesting level (`struct S0 {
   struct S1 { ... } s1; } s0;` at depth 10: 8.8 s and 2.9 GB); member
   classes and in-class member function bodies doubled per level.
   `parse_specified_declaration` now parses specifiers and first declarator
   once; the function-body path falls back to the init-declarator
   continuation only if the body fails.
2. **Exponential re-parse through the memo that was never wired
   (material, fixed)**: the plan's memo, `hard_failure_` and
   `angle_refusal_` existed but no rule called `try_memoized`.  Statement
   parsing re-read every expression starting with an unknown name after
   the declaration attempt failed (nested lambdas: 2× per level, depth 18:
   5.6 s / 1.45 GB), and every `id <` with an unknown left operand
   speculated a template-id over the rest of the expression (18 `<` in one
   condition: 2.5 s / 372 MB).  `parse_assignment_expression` and
   `parse_simple_template_id` are now memoized on (rule, position, angle
   boundary), storing failures as well as successes; the memo is cleared
   only when a restore undoes a binding, which never happens on the suite
   (counter: 0 clears over 157 tests).  The committed-template-id hard
   failure of the plan was not needed by any fixture and was removed rather
   than wired.
3. **Latent memory-safety defect (fixed)**: the leading parameter pack was
   inserted with `children.insert(children.begin(), make(...))`, taking a
   reference into the arena vector before `make` could reallocate it.
   The pack node is now created before the reference is taken; no other
   site holds an `AstNode&` across a `make`.
4. **Textual downgrades inside the parser (fixed)**: typedef detection by
   the `KW_TYPEDEF:` label prefix, the `int = 0` fixture rule by comparing
   `"KW_INT:int"` and `"0"` texts, sizeof classification by comparing the
   operator label, a dead `++/--` label ternary, and template-name recovery
   by splitting the joined declared name on `::` and `<`.  All read typed
   token spans now (`specifier_seq_has_keyword`, `declared_identifier`,
   `template_name`, keyword type checks).  Binding walked every
   `identifier` node under a declarator (including member names in array
   bounds and default arguments) and bound joined qualified names and
   template-ids as if they were identifiers; those bindings were
   unreachable garbage and are gone.
5. **Stale derived state and dead vocabulary (removed)**: the span written
   as `(pos_, pos_)` on every node, `SetSpan`, `leaf`, `operator_name`,
   seven unreferenced rules, one declared-but-undefined rule, seven AST
   kinds never produced, `consume_kind`, unused `Undo::kind`, unused
   includes.  The member and general declarator suffix loops are one
   `parse_function_suffixes`; nested-declarator and array-suffix parsing
   are their own rules.
6. **Missing parameter bindings (fixed)**: lambda bodies and special-member
   bodies/mem-initializers did not bind their parameters, so `a < b > (1)`
   with parameter `a` printed as a call of the template-id `a<b>`; the plan
   states parameters are values in those scopes and they now are.
7. **Operator names (fixed)**: the qualified `C::operator ...` gate for
   "conversion function without return type" listed five tokens; it is now
   the typed rule that a type must follow `operator`.  The accepted
   operator-function-id set grew from 17 tokens to every overloadable
   operator including the two-piece `>>`; `C& operator+=(int);` previously
   failed the whole translation unit.
8. **Duplicated authority (fixed)**: cppgm++ carried the sixth copy of the
   stat-based file id and asctime build stamp; all six drivers now use
   `preproc_host`.
9. **Plan rules expressed as typed checks**: the `TT_LITERAL:0` fixture
   quirk is the plan's rule (declarator-less parameter, single keyword
   type, single literal token) so `int N = 0` prints `literal 0`; special
   members in a partial specialization match the base identifier instead
   of the joined `S<T*>` text.

Behaviour on the 251 checked-in inputs and 52 probes is unchanged except
for two pa6 inputs the old binary could not finish in 20 s: the 300-deep
`TC1<...>` chain now succeeds in 0.10 s, and the course "-bad" variant
(digraph `<%` in the innermost argument) is now rejected in 0.10 s, both
matching the pa6 references.  Items 6, 7 and 9 change output only for
inputs outside every fixture corpus, in the direction the plan specifies.

## Performance evidence

Immutable executables (pre-cleanup and final), interleaved, medians of 5:

| probe | old | final |
| --- | --- | --- |
| nested `struct {…} s;` depth 8 | 0.60 s / 186 MB | 0.10 s / 7 MB |
| nested `struct {…} s;` depth 10 (one sample) | 8.81 s / 2.9 GB | 0.10 s / 7 MB |
| nested lambdas in assignments, depth 14 / 18 | 0.40 s / 94 MB; 5.61 s / 1.45 GB | 0.10 s / 7 MB |
| nested member functions in member classes, depth 16 | 0.90 s / 200 MB | 0.10 s / 7 MB |
| `a0 < b0 && … ` unknown names, 14 / 18 clauses | 0.20 s / 27 MB; 2.50 s / 372 MB | 0.10 s / 7 MB |
| same with bound names, 18 clauses | 0.10 s | 0.10 s |
| 8000-statement body | 0.20 s / 30 MB | 0.20 s / 30 MB |
| 24000 top-level declarations | 0.50 s / 96 MB | 0.50 s / 96 MB |
| 400 nested parentheses; 120-deep type-name; 200-deep if/else | 0.10 s | 0.10 s |

Structural counters (final build with temporary counters): the 157-test
suite allocates 7464 nodes for 5898 reachable (21% abandoned speculation),
615 memo misses, 27 hits, 0 clears.  The 24000-declaration probe allocates
336001 nodes for 288001 reachable with 8000 misses and no hits; the 18-term
`<` chain allocates 787 nodes for 83 reachable with 154 hits doing the
work; the depth-18 lambda probe has 19 hits.  Speculation over `<` chains
is quadratic in the number of unknown-name `<` per expression, not linear,
and is accepted at that bound.

Profile of the 24000-declaration probe (perf, 0.69 s task-clock, 36.9k page
faults): kernel page faults ≈13%, lexer UTF-8 decoding ≈9%, ostream
insertion ≈7%, arena vector growth 2.7%, `Pa6Token::IsSimple` 2.4%; no
parser rule above 2.5%.  Cost is linear and dominated by tokenization,
node construction and output streaming, about 4 KB of retained tokens and
nodes per declaration.  The 157-test suite runs in 0.45 s wall (median of
3) against the 10 s per-test harness budget.

Accepted at stage scale: `AstNode` keeps a per-node `std::string` text and
`std::vector` children because the tree is the requested dump's model and
the text is rendered once at the owning rule; `has_angle_boundary()` is
linear in bracket depth per memo key; the statement-level preference for an
expression containing a call walks that one expression (fixture-pinned by
300-declaration-statement-ambiguity and
200-mock-type-declaration-ambiguity).

## Conformance validation

- `make test-report-through-pa10`: 589/589 (432 through pa9 unchanged +
  157 pa10; the course pa10 set has 0 tests).
- `perl scripts/cppgm_file_audit.pl --stage pa10 --paths dev/src`: passes
  with the one pre-existing `recog_parser.h` division warning.
- Differential run, pre-cleanup vs final executable: 251 fixture inputs
  and 52 probes, identical exit status and dump except the two pa6 inputs
  above, both of which now match their references.
- Parser sources: 4023 lines over three translation units (was 4279),
  139 rule and helper definitions (was 131 with 8 dead).

## Checkpoint ledger

- CP1 core parser, model, printer, driver — `make test-pa10` 58/157,
  through-pa9 432/432.
- CP2 namespaces, classes, enums, members, special members — 94/157.
- CP3 templates, dependent names, explicit instantiation/specialization —
  143/157.
- CP4 residual closure (parameter packs, attributes, decltype, placement
  new, ambiguity fixes) — 157/157, through-pa9 432/432, suite 0.46 s.
- CP5 final architecture cleanup (this audit) — exponential paths removed,
  memo wired, typed spans and name facts, shared host helper, dead code
  removed; 589/589 through pa10, file audit passing, outputs unchanged on
  every checked-in input.
