# PA14 Plan — abimangle (normalized ABI facts → Itanium mangled names)

Grading: `abimangle -o x.my x.t`; exit status must match `x.ref.exit_status`
(2 fixtures expect EXIT_FAILURE, 109 expect EXIT_SUCCESS plus a byte-exact
match of one name per case, newline-terminated).  No reference binary: the
111 `.ref` files under `pa14/tests/abi` are the only oracle, so every
emission convention below is fixture-pinned (fixture names cited as
`<prefix>` without the `.t`).  `cppgm.tests/course/pa14` is empty.  The
harness links `dev/src/test_runner.cpp` and renames the tool's `main`
(`-Dmain=test_runner_real_main`), so one process may run many inputs: no
mutable static state anywhere.  `doc/itanium-mangling.txt` is the grammar
reference (it predates `Tn`, see Uncertainties).

## Stage Design

Data flow: `dev/abimangle.cpp` (scaffold envelope: `-o`, help, exceptions →
EXIT_FAILURE) → `mangle_fact_files(paths)` reads each file, splits lines into
words, groups records into cases → `parse_fact_record_words` builds typed
`AbiFactRecord`s → per case an `AbiDefinitionTable` (id → definition,
duplicate id is an error) plus exactly one target → `Mangler` encodes the
target to one string → names joined with `\n` and written once.  Text is
never re-parsed after the reader; the encoder consumes only typed records.

Modules (all listed under `FRONTEND_OBJ_BASENAMES_abimangle`; PA15 links only
the encoder objects):

- `dev/src/abi_mangle.h` (scaffold, extended): the typed model as shipped
  plus `struct AbiDefinitionTable { unordered_map<string, AbiDefinitionRecord>;
  add (throws on duplicate); find_type/argument/expression/context/entity }`,
  `AbiFactFile parse_fact_text(const string & text, const string & name)`,
  `string mangle_fact_case(const AbiFactCase &)`, and the direct encoder
  entry PA15 will call: `string mangle_target(const AbiTargetRecord &,
  const vector<AbiFunctionRecord> &, const AbiDefinitionTable &)`.
- `dev/src/abi_fact_parse.cpp` — the line-oriented fact reader: line/case
  splitting, `parse_fact_record_words` (every `let-*`, target and function
  record form below, compact `a:b:c` type grammar, nonnegative-index and
  integer validation), `serialize_fact_file` (inverse, multiword
  `member-pointer` form).  Words are split on single spaces keeping empty
  words, so `name-source  -` yields name `""`, key `-`.
- `dev/src/abi_mangle_encode.cpp` + internal `abi_mangle_encoder.h` — class
  `Mangler {string out; SubstitutionTable subs; const AbiDefinitionTable &
  defs; int depth;}` with the name grammar: qualified names, prefixes,
  `St`, local names, template argument lists, `FunctionShape` lowering and
  emission, special names, entity encodings, driver `mangle_target`.
- `dev/src/abi_mangle_types.cpp` — `Mangler` methods for `<type>`,
  `<expression>`, `<expr-primary>`, builtin table, integral value spelling,
  and the structural key builders (`key_of_type/argument/expression`).

Resolution: a bare word in type position resolves in order definition id
(`let-type`) → builtin word → compact constructor → named class `Q`; arg
refs must be `let-arg` ids, expr refs `let-expr` ids, context refs
`let-context` ids, entity refs `let-entity` ids (missing → failure).
Definitions are case-scoped and may appear anywhere in the case (fixtures
interleave `let-*` with `param`), so resolve lazily at encode time with a
depth guard (`depth > 512` → failure) against `let-type A ptr:A`.

Substitution table: `vector<string> keys` + `unordered_map<string,size_t>`.
Lookup before emitting a candidate; on miss emit the component, then
register (children before parents).  Registering an existing key is a
no-op (one slot per component: `500-distinct-integral-decltype-substitution`
pins `T_` as a single slot).  Seq-ids `S_`, `S0_`, …, base 36.  Keys are the
represented structure, never the emitted text: all name-like keys are the
qualified-name text with leading `::` stripped (`ns`, `ns::C`, `std::__1`,
template name `ns::Outer`, specialization `ns::Outer<`+arg keys+`>`), so a
`name-source ns ns` prefix, a `named:ns::C` type's prefix and a
`template ns::traits …` template name all share slots
(`300-function-template-substitution-index`, `200-abi-tagged-member-and-
constructor` `RKS_`, `200-conversion-terminal-substitution-order` `cvS_`).
Fact-provided keys (`name-source`, `name-template`, `function-template-
prefix`, `member-template-entity`) are used verbatim in that same space.
Structural keys: `P(k) R(k) O(k) VK(k) A<n>(k) M(o,m) F(r;p;z) U<n>(k)
u<n>(k) Dp(k) DT(ek) Tn T<i> TT<i><args> Z(ctx)…`; builtins never register;
`template-param` (non-subst) never registers and never looks up
(`600-inline-namespace-basic-string-param`, `600-template-param-template-
type-substitution`); everything else registers exactly once.

Names (`Q` = components after stripping a leading `::`):

- 1 component: unscoped `<len>X` (not a candidate).  `std::X`: `St<len>X`
  unscoped (`100-std-variable`); `std::a::b…`: `NSt<a><b>…E`, registering
  `std::a`, `std::a::b`, … but never `std` (`100-std-inline-namespace-
  variable`, `NS3_11char_traitsIcEE` in `300-namespace-class-and-string-
  substitution`).  Otherwise `N <p1> <p2> … <last> E`, registering each
  prefix `p1`, `p1::p2`, … (not the last component of a function/variable).
- Named type: same as the prefix chain, then register the full name as the
  type key (so `C` as a scope and `named:C` are one slot).  `named:C`,
  `name C` and bare `C` are one structure (`300-equivalent-named-type-
  spelling-substitution`).
- Template specialization `template Q args`: prefixes; template name
  (`St11char_traits` or `N…<X>` before args, registered as `Q`);
  `I<args>E`; register `Q<argkeys>`.  `template std::allocator X` is
  `St9allocatorI…E`, never `Sa`; `Sa/So/Ss…` arise only from `std-template
  <code> <yes|true|no|false> Q args`: `yes` emits the code alone and
  registers nothing, `no` emits `<code>I…E` and registers `Q<argkeys>`
  (`300-std-allocator-substitution`, `300-std-ostream-member-template-
  result`).
- `name-template <name> <key> <complete-key> <stdsub|-> <yes|no> <argrefs>`
  as a prefix component: `stdsub` non-`-` and `yes` → the code alone;
  otherwise `<len>name`, register `key`, `I…E`, register `complete-key`.
  As the final component of a function it supplies the function's template
  args (`300-inline-template-result-substitution`: `St9addressof` is `S_`).
- `name-source <name> [<key>]`: non-final components emit `<len>name` and
  register `key` unless `-`/absent; the final component with empty name is
  the ctor/dtor placeholder; a final key is registered as the template
  prefix only when function template args follow.
- `function-template-prefix <key>`: register `key` after the unqualified
  name and ABI tags, before `I…E` (`300-abi-tagged-function-template`:
  `15tagged_templateB9nqe220100` is `S_`, then `T_` is `S0_`).  `function
  path Q argrefs` registers `Q` the same way (`300-atomic-addressof-
  template`).
- `member <owner> <name>` / `member-template <owner> <name> <argrefs>`:
  `N <owner as prefix> <len>name [I…E] E`; the owner is emitted by the type
  path without its own `N…E` (`Nu14__remove_constI…E10first_typeE`,
  `NT_6rebindIT0_E5otherE`, `N2ns5OuterIT_E8IteratorIT0_EE`), registered as
  a type, then the whole member type is registered.
- `member-template-entity <owner> <name> <key>` (template-template arg):
  `N`, owner template name + `I…E` **without registering the owner
  composite**, then look up `key` → `S<n>_` else `<len>name` and register
  `key`, then `E`.  Pinned by `300-member-template-template-function-
  argument`: second use emits `NS0_IS1_ES2_E`.
- ABI tags: `B<len>tag` per tag, sorted by byte order, deduplicated,
  appended to the unqualified name, ctor/dtor code, or operator code
  (`200-canonical-abi-tag-order`, `C1B9nqe220100`, `clB9nqe220100`);
  `tagged <type> <tags…>` puts them on the type's last unqualified
  component and its typeinfo/vtable inherit them (`_ZTI1CB3tag`).

Types: builtins `void v bool b char c schar a uchar h short s ushort t int i
uint j long l ulong m longlong x ulonglong y int128 n uint128 o float f
double d longdouble e float128 g wchar w char16 Ds char32 Di nullptr Dn auto
Da complex-float Cf complex-double Cd complex-longdouble Ce`; `ptr:` P,
`ref:` R, `rref:` O, `const:`/`volatile:` fold into one cv node emitted
`[V][K]` (`300-canonical-cv-type-substitution`: `VK1C` then `S0_`),
`array:<n>:` `A<n>_`, `memberptr:<owner>:<member>` and `member-pointer`
`M<owner><member>` (owner ends at the first `:` not part of `::`),
`function-type r p…` `F<r><p…|v>E`, `function-type-variadic` adds `z`,
`vendor N T` `U<len>N<T>`, `builtin-transform N T` `u<len>NI<T>E`, `pack T`
`Dp<T>`, `template-param i` `T_`/`T<i-1>_` unregistered, `template-param-
subst i` the same but a candidate, `template-param-template i args`
`T<i>_I…E` registering only the whole type (`600-template-param-template-
type-substitution`: second `ref AllocT` is `S1_`), `decltype e` `DT<e>E`
(CP4), `lambda-closure ctx d [sig…]` `Z<ctx>EUl<sig|v>E<d>_`, `local-type
ctx name d` `Z<ctx>E<len>name[_<d-1>]`, `namespace-lambda src [ns…]`
`N<ns…><len>src E` (unscoped when no qualifiers).  Negative or non-numeric
indices/bounds are failures (`100-reject-negative-template-parameter-
index`).  `type` targets emit the bare `<type>` (no `_Z`).

Template arguments: `type` → type; `value <builtin> <n>` → `L<code><n>E`,
negative signed → `n<magnitude>` (compute the magnitude in unsigned
arithmetic: `300-minimum-signed-integral-value`), negative unsigned → the
value modulo the width (`uchar 8 ushort 16 uint 32 ulong/ulonglong 64`,
`100-unsigned-int-integral-value`), bool → `0/1`; `dependent-value <ptype>
<builtin> <n>` → `Tn<ptype>L<code><n>E` (`300-dependent-integral-value-
argument`); `expression e` → always `X<e>E`, even for an expr-primary
(`300-internal-variable-entity-expression`); `entity-address ent` →
`XadL<entity encoding>EE`; `member-external-address <sym> <owner> <member>
<fn> <c> <v> <lref> <rref> <variadic> <params…>` → `XadL<sym>EE` (typed
fields kept for PA15); `template-param-template i` → `T<i>_` (not
registered); `template-entity Q` → the template name (untested).  Value
keys normalize spelling so `value int 1` twice is one slot
(`300-equivalent-value-argument-substitution`); array bounds are part of the
key (`300-distinct-array-bound-substitution`).

Entities: `variable Q` → name; `internal-variable Q` → name with `L` before
the last component (`N2nsL5limitE`); `function Q params…` → name + params
(`_ZN1C1fEi`); `symbol S` → S verbatim.  Entity operands are encoded as
`L_Z<enc>E` (or `L<sym>E`) by a **fresh nested `Mangler`** with its own
substitution table: `300-function-owner-member-pointer-nttp-data` ends in
`R1C`, so nothing inside `L_ZN1C1mEE` registered.

Expressions (`let-expr`, CP4; op words are Itanium codes): `template-param
i` → `T_` (never a candidate: `500-equivalent-dependent-expr-substitution`
gives `DTT_ES_`), `function-param i` → `fp_`/`fp<i-1>_`, `literal n` →
`Li<n>E`, `binary op a b`, `unary op a`, `conditional c t f` → `qu…`, `pack
e` → `sp<e>`, `call callee args…` → `cl…E`, `cast op <type> e` →
`<op><type><e>` (`sclfp_`), `template-id name argrefs…` → `<len>nameI…E`,
`type-trait name types…` → `u<len>name<types>E` (`500-dependent-type-trait-
expression`), `sizeof-type t` → `st<t>`, `member <type> <yes|no|0|1> name
[argrefs]` → `sr<type>[E]<len>name[I…E]` where the flag emits the `E`
(`srT_E4type` vs `srT_5value`) and the type goes through the ordinary type
path (a `template-param-subst` owner is a candidate: `srS2_5value`),
`object-member op obj name [argrefs]` → `<op><obj><len>name[I…E]`,
`entity-reference ent` → the entity operand.  `decltype` types are
candidates keyed by expression structure.

Function shape (one struct built from every function form, then one
emitter): components (std flag, `name-source`, `name-template`, `local-
context`, `lambda-context`, `namespace-lambda-context`), terminal, cv/ref
qualifiers, ABI tags, template-prefix key, template args, optional result,
params, variadic, C linkage.  Compact forms: `function [path] Q operands…`
where each operand is classified at encode time (a `let-arg` id → template
argument; `variadic` → variadic; anything else → parameter type; `let-
context … function path host int` therefore yields `Z4hostiE`); `function
local ctx name terminal d`; `function lambda ctx d terminal sig…` (trailing
types are the lambda signature, params come only from `param` lines:
`_ZZN2ns1C4makeEvENUliE0_clEv`); `function namespace-lambda src terminal
[ns…]`; `function encoding` + records; `c-function N params…` emits `N`.
Terminals: `terminal-source X` `<len>X`; `terminal constructor-complete/
base/allocating` `C1/C2/C3`, `destructor-deleting/complete/base`
`D0/D1/D2`; `operator-terminal` by the README table (`plus` `pl`/`ps`,
`minus` `mi`/`ng`, `address-of`/`bit-and` `ad`/`an`, `deref`/`multiply`
`de`/`ml`, unary vs binary decided from parameter count plus member shape
unless the explicit `unary-*`/`binary-*` word is used; `literal S`
`li<len>S`; `call` `cl`; `operator-call` in local/lambda forms is `cl`);
`conversion-terminal T` `cv<T>` with `T` a normal type-path candidate and no
result record emitted.  Emission: `_Z`; C linkage → bare name; local
context → `Z<ctx encoding>E` then `N[V][K][R|O]<local class[_d-1] or
Ul<sig>E<d>_ or <len>$_0><terminal>[B…]E`; nested when more than one
component (counting the empty ctor/dtor placeholder) or any qualifier:
`N[V][K][R|O]<prefixes><unqualified>[B tags][I args]E`; else unscoped
`[St]<unqualified>[B tags][I args]`.  Then result only when a `result`
record exists (`_Z1fIiEi` has none), then params or `v`, then `z`.  The
local context function is encoded with the **shared** table
(`ZNS_4makeEvE` in `600-function-local-class-template-arg`, `NS_1CE` in
`200-local-context-lambda-substitution-order`); a `raw` context is pasted.

Special targets: `typeinfo/vtable/vtt T` → `_ZTI/_ZTV/_ZTT<T>`;
`construction-vtable T n B` → `_ZTC<T><n>_<B>`; `tls-wrapper variable Q` →
`_ZTW<name>`; `variable Q` → `_Z<name>`; `thunk h [function…]` → `_ZTh<h>_`,
`thunk h r` → `_ZTch<h>_h<r>_`, `thunk h virtual-result f v` →
`_ZTch<h>_v<f>_<v>_`, `virtual-base-thunk v` → `_ZTv0_<v>_`, each followed
by the base function encoding without `_Z`; numbers spell negatives as
`n<abs>`.

## Failure Map (at planning)

All 109 success fixtures fail with EXIT_FAILURE from the scaffold's
`NotImplementedException`; the 2 EXIT_FAILURE fixtures pass vacuously.  By
owning boundary:

- Reader + types + names + substitution + non-function targets (CP1, 35):
  the 20 `100-*` type/variable/special fixtures (both negatives now by real
  rejection), `200-{abi-tagged-special-types,builtin-transform-type,
  tls-wrapper}`, `300-{construction-vtable,dependent-integral-value-
  argument,dependent-value-builtin-transform-member-owner,member-external-
  function-nttp,member-pointer-nttp-data,minimum-signed-integral-value,
  raw-symbol-entity-nttp,std-allocator-substitution,template-template-
  argument}`, `400-{builtin-transform-member-owner,dependent-owner-member-
  template,dependent-rebind-other}`.
- Function encodings, terminals, thunks, function templates (CP2, 48):
  `100-{c-linkage-function,constructor-destructor-variants,global-function,
  namespace-function-params,variadic-function}`, the 15 non-local `200-*`
  function/operator/thunk fixtures, the 24 remaining `300-*` fixtures
  except the four listed under CP4, `600-{inline-namespace-basic-string-
  param,nested-helper-owner,template-param-template-type-substitution,
  template-parameter-pack-reference-constructor}`.
- Local-name contexts and lambdas (CP3, 10): `200-{lambda-closure-type,
  local-class-call-operator,local-context-destructor-terminal,local-context-
  lambda-substitution-order,member-lambda-call-operator,raw-lambda-context-
  function,raw-local-context-function}`, `600-{function-local-class-
  template-arg,function-template-local-class-arg,function-template-local-
  lambda-arg}`.
- Dependent expressions, decltype, member-template entities (CP4, 18): all
  13 `500-*`, `400-dependent-alias-type-id`, `300-{internal-variable-entity-
  expression,member-template-template-argument,member-template-template-
  function-argument,qualified-member-template-substitution-order}`.

## Performance Risks

Fixtures are tiny, so the requirements are structural.  Every id and
substitution lookup is a hash probe; the output is one `std::string`
appended in place; each file is read once and split once.  Structural keys
are rebuilt per candidate, which is quadratic in nesting depth for a long
`let-type` chain; cache `key_of_*` results per definition id (the `let-*`
graph is a DAG of shared nodes) so key cost is linear in fact size.
Lazy resolution recurses through definitions: bound it with the depth guard
rather than an unbounded stack.  Entity operands spawn a nested `Mangler`
per use; that is bounded by the number of entity references.  No regex, no
static tables built at runtime per call, no per-case reparsing.

## Checkpoint Ledger

- CP1 (complete) — fact reader for the full vocabulary, typed definition
  table, substitution table, type/name/template-argument encoder,
  non-function targets and variables: all 35 CP1 fixture files pass exactly
  (33 newly green plus the 2 real negative reader rejections); stage total is
  37/111 because two adjacent simple type cases also pass (failures 109 →
  74).  `make test-report-through-pa13` is clean at 919/919, the pa14 file
  audit passes, and the 10k/20k probe scales 2.85s → 5.60s at ~48 MB RSS;
  the 5k pointer chain completes in 0.12s without stack overflow.
- CP2 (complete) — `FunctionShape` lowering and emission for compact, `path`
  and `encoding` forms; terminals, qualifiers, tags, template prefixes/args,
  results, thunks, `c-function`, function entities: all 48 CP2 cases pass;
  stage total is 85/111 (failures 74 → 26).  `make
  test-report-through-pa13` remains clean at 919/919 and the pa14 file audit
  passes with three non-fatal structural warnings.
- CP3 (complete) — local-name contexts (`function`/`raw`), local types, lambda
  closures, namespace lambdas, and their call operators share the substitution
  table.  The ten packet fixtures plus the adjacent namespace-lambda fixture
  pass; the stage is 96/111 (failures 26 → 15).  `make
  test-report-through-pa13` remains clean at 919/919 and the pa14 file audit
  passes with the same three non-fatal warnings.  The remaining failures are
  the planned CP4 dependent-expression boundary.
- CP4 — dependent expressions, decltype, type traits, entity-reference
  expressions, `member-template-entity`: 111/111, through-pa14 clean.
- CP5 — architecture audit and cleanup: one key authority, one function
  emitter, serializer round-trip check, perf probe evidence in `audit.md`.

## Completed Checkpoint: CP1 — reader, model, substitution table, types and names

Goal: every fixture parses into typed records (the two negatives fail from
real duplicate-id and negative-index rejection inside the reader), the
encoder handles every `type`, `variable`, `typeinfo`, `vtable`, `vtt`,
`construction-vtable` and `tls-wrapper` target, and `function`/thunk targets
throw `logic_error("unsupported target")` until CP2.  Progress proof: the 35
fixtures in the CP1 failure-map group pass (33 newly), no other fixture
changes status, and no success fixture fails in the reader (check
`.my.exit_status` after `make test-pa14`: a CP2–CP4 fixture must fail from
the encoder, never from `parse_fact_record_words`).

### Implementation Packet

Files/symbols:

- `dev/src/abi_mangle.h`: add `AbiDefinitionTable`, `parse_fact_text`,
  `mangle_fact_case`, `mangle_target`; keep every scaffold enum/struct
  (fill fields in place; add `AbiType::rvalue_ref` use for `rref:`).
- `dev/src/abi_fact_parse.cpp` (new): `parse_fact_text` (lines → words →
  records; `case <label>` starts a case, blank lines ignored, records
  before the first `case` form an implicit case; a case needs exactly one
  target), `parse_fact_record_words` with one small function per record
  family (`parse_type_words`, `parse_compact_type`, `parse_argument_words`,
  `parse_expression_words`, `parse_context_words`, `parse_entity_words`,
  `parse_target_words`, `parse_function_record_words`),
  `serialize_fact_file`, `mangle_fact_files` (unreadable file → failure).
- `dev/src/abi_mangle_encoder.h` (new, internal): `class Mangler` with
  `SubstitutionTable`, `mangle_type`, `mangle_qualified_name`,
  `mangle_prefix_chain`, `mangle_template_args`, `mangle_template_arg`,
  `mangle_entity_encoding`, `mangle_special_target`, `key_of_*`.
- `dev/src/abi_mangle_encode.cpp` (new): names, prefixes, `St` handling,
  template argument lists, entity encodings, special targets, `mangle_target`
  dispatch, `mangle_fact_case`.
- `dev/src/abi_mangle_types.cpp` (new): builtin table, type constructors,
  integral value spelling, structural keys.
- `dev/abimangle.cpp`: delete the stub definitions; keep the envelope.
- `dev/frontend_source_sets.mk`: `FRONTEND_OBJ_BASENAMES_abimangle :=
  abi_fact_parse abi_mangle_encode abi_mangle_types`.

Fixture groups: the 35 CP1 fixtures above are the must-pass set; the
`function`/`thunk`/`c-function` fixtures must parse and fail only in the
encoder; every fixture in the suite must parse (this pins the reader's
vocabulary: `name-source  -`, `name-template` with 6+ words, `member-
external-address` with 9 fixed words then types, `thunk … virtual-result …`,
`std-template So yes …`, `member T yes type` and `member T 0 value`).

Required spec facts: `<seq-id>` (doc §5.1.2 lines 152–154) and substitution
rules (§5.1.10 lines 809–860: components numbered left to right, children
before parents, builtins excluded, one slot per component, `St`/`Sa`/`So`
catalog, `St` needs `N…E` only with more than one further component or
qualifiers); local names and closure types for CP3 (§5.1.7, §5.1.8);
`<type>` constructors and `[V][K]` order (§5.1.5); `<template-param>` is a
candidate distinct from its argument (§5.1.5.8); `<expr-primary>` `L<type>
<n>E` and `L_Z<encoding>E` (§5.1.6); special names `TV TT TI TC TW` and
`<call-offset>` `h<n>_`/`v<n>_<n>_` (§5.1.4); `<abi-tag> ::= B
<source-name>` sorted; `Tn<type>` before a dependent-typed value argument
comes from the current ABI's `<template-param-decl>` (absent from the local
doc, pinned by `300-dependent-integral-value-argument`); README facts:
`float128` → `g`, complex words → `Cf/Cd/Ce`, unsigned negative values are
modulo the width, adjacent cv words are one type, `memberptr` owner
delimiting, `function-type` empty list → `v`, definition ids are binders
that never appear in names, redefinition and negative indices are failures.

Commands: focused `make test-pa14` (or `cd pa14 && make test`); one case
`make -C pa14 check TEST=tests/abi/100-member-pointer.t`; inspect
`pa14/tests/abi/*.my.exit_status` for reader-vs-encoder failures; broad
`make test-report-through-pa13` (must stay clean) and `perl
scripts/cppgm_file_audit.pl --stage pa14 --paths dev/src` (no fatal
findings: functions under 240 lines, nesting under 8, no source includes
source, no `EXIT_NOT_IMPLEMENTED` spelling under `dev/src`).

Performance probe: generate `/tmp/abi_big.t` with 20k cases of the
`300-std-vector-string-substitution` shape (each case its own `let-*`
block) and one case with a 5k-long `let-type tN ptr:tN-1` chain ending in
`type tN`; `time dev/abimangle -o /tmp/abi_big.my /tmp/abi_big.t` at 10k and
20k cases must scale ×2, and the chain case must not overflow the stack or
go quadratic (key caching per definition id).

Known uncertainties (resolve by probing fixtures, never by special-casing
names): whether a case may hold several targets (none do; reject); whether
records before the first `case` line are legal (accept as an implicit
case); `member`'s third word is a boolean spelled `yes/no/0/1` (only `yes`
and `0` appear); lambda discriminators are emitted literally as `<d>_` (only
`0` appears, giving `0_`) while local classes use `_<d-1>` for `d ≥ 1`;
whether the conversion type or the `function-template-prefix` key registers
first (unpinned; register the type first, matching left-to-right emission);
whether `name-template`'s complete key should equal the derived
specialization key (unpinned; verbatim); `template-entity`, `external-
address`, `untyped-value`, argument packs, `rref:`, ref-qualifiers, raw or
expression array bounds and `constructor-allocating` have no fixtures —
implement the obvious Itanium spelling or reject, never guess silently;
`path` in `function path …` is an optional keyword with no separate
semantics; `Tn` and the `member-template-entity` slot order are
fixture-pinned quirks to keep even where they diverge from GCC/Clang.

## Completed Checkpoint: CP2 — FunctionShape lowering and emission

Goal and evidence: lower compact, `path`, and `encoding` function targets
through one structured emitter, including terminals, qualifiers, tags,
template prefixes and arguments, results, C-linkage functions, function
entities, and thunks.  The CP1 37/37 pass set was preserved; the broad pa14
run reached 85/111, with only the planned CP3/CP4 boundaries failing.

### Implementation Packet

Files/symbols:

- `dev/src/abi_mangle.h` and `dev/src/abi_mangle_encoder.h`: extend the typed
  function shape and shared lowering state without adding a second emitter.
- `dev/src/abi_mangle_encode.cpp`: lower compact/path/encoding forms and emit
  terminals, qualifiers, ABI tags, function-template components, results,
  C-linkage names, function entities, and thunk call offsets.
- `dev/src/abi_mangle_types.cpp`: keep type/argument candidates and the shared
  substitution table in the same emission order.

Fixture group: the 48 CP2 cases in the failure map—100-level function forms,
non-local 200-level functions/operators/thunks, the CP2 300-level cases, and
the four listed 600-level cases. Preserve the CP1 pass set and ensure every
later fixture still parses before its unsupported encoder boundary.

## Active Checkpoint: CP4 — Dependent expressions and member-template entities

Goal: lower dependent expressions, decltype types, and member-template entity
arguments through the existing type/argument substitution state while
preserving the 96/111 CP3 result and all earlier PAs.

### Implementation Packet

Files/symbols:

- `dev/src/abi_mangle.h` and `dev/src/abi_mangle_encoder.h`: preserve one
  structural key authority for expression, decltype, and entity candidates.
- `dev/src/abi_mangle_types.cpp`: implement `mangle_expression_impl`,
  decltype and member-template type lowering, dependent template arguments,
  entity-reference expressions, and their shared substitution order.
- `dev/src/abi_mangle_encode.cpp`: integrate only the target/function paths
  needed by those existing type and argument candidates; keep one function
  emitter.

Fixture group: the 18 CP4 cases in the failure map—13 `500-*` expression
fixtures, `400-dependent-alias-type-id`, and the four `300-*` internal/entity
and member-template fixtures.  Keep all 96 currently passing cases green and
finish with `make test-pa14` at 111/111.
