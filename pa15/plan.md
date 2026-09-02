# PA15 Plan — cppgm++ --emit-lowir (PA12 semantic tree → PA13 LowIR)

Grading: `cppgm++ --emit-lowir -O0 -o x.my x.t`; exit status must match
`x.ref.exit_status` (3 fixtures expect EXIT_FAILURE, 106 expect EXIT_SUCCESS
plus a relaxed LowIR match).  No reference binary: the 109 `.ref` files under
`pa15/tests/general` are the only oracle (`cppgm.tests/course/pa15` does not
exist).  The harness links `dev/src/test_runner.cpp` and runs many inputs in
one process: no mutable static state.  `pa15.gram` is byte-identical to the
PA10/PA12 grammar, so every parse failure below is a parser gap, not a
grammar change.

Relaxed compare (`scripts/compare_results_common.pl`, `compare_lowir_text`):
both texts are validated, then every metadata item with key `linkage`,
`binding`, `object`, `keep_alias`, `prefer_local`, `tls_for`,
`trivial_lifecycle`, `effects`, `unwind`, `return`, `capture`, `access`,
`alias`, `projection` (and `storage=readonly`) is dropped; `alias object`
lines are dropped; function `@names` are replaced by placeholders after
pairing (by `object=`/`role=` key, then by identical normalized shape, then
by order); top-level entries are sorted by kind rank then text.  Everything
else is exact: global `@names`, `$slot` names, `%temp` names, `^labels`,
`pass=`/`arity=`/`role=` metadata, instruction order, blank lines inside a
function.  `CPPGM_LOWIR_DIRECT_TEXT_COMPARE=1` switches to byte comparison;
aim for byte parity, it makes diffs readable.

## Stage Design

Data flow (per input file, one shared program): `dev/cppgm++.cpp`
`run_emit_lowir_mode` → PreprocEngine → `Pa10Parser` → `TypeTable` +
`SemaModel` + `SemaTree` built by `ScopeBuilder` in dump mode (the PA12
tree is the only semantic source of truth; lowering never re-derives types
from tokens) → `LowerTranslationUnit(tree, model, tokens, builder)` appends
to one `lowir_model::Program` → `serialize_lowir_program` → outfile.  Any
exception → EXIT_FAILURE (driver envelope unchanged).

Modules (new files under `dev/src/lower/`, all added to
`FRONTEND_OBJ_BASENAMES_cppgm++` together with `abi_mangle_encode
abi_mangle_types`; verified: those two objects have no undefined symbol
from `abi_fact_parse.o`):

- `dev/src/lowir_serialize.cpp`: implements the declared but missing
  `lowir_model::serialize_lowir_program` (the canonical text of PA13
  `lowir.md`, exact fixture layout below).  Owned by the PA13 boundary so
  later stages print through it.
- `dev/src/lower/lowir_lowering.h` (internal): `ProgramState` (the
  `lowir_model::Program`, declared/defined symbol sets, string-literal
  pool, dynamic-init actions), `FunctionFrame` (temp/label/slot counters,
  slot-name set, binding→slot map, block list, break/continue/label
  stacks), `class FunctionLowering` (statements + expressions API:
  `Value`, `Address`, `Branch`, `Discard`, `Convert`), type helpers.
- `dev/src/lower/lowir_types.cpp`: `TypeId` → `LowType` (`bool`→`u8`,
  `char`→`i8`, `signed char`→`i8`, `unsigned char`→`u8`, `short`/`unsigned
  short`→`i16`/`u16`, `int`/`unsigned`→`i32`/`u32`, every 64-bit integer
  →`i64` (LowIR has no `u64`; signedness comes from the C++ type),
  `float/double/long double`→`f32/f64/f80`, pointers/references/functions/
  nullptr_t→`ptr`, enums→their underlying type, arrays→`obj<bytesxalign>`),
  signedness/size/alignment queries via `TypeTable::SizeOf/AlignOf`, and
  the conversion instruction table (`Convert`).
- `dev/src/lower/lowir_symbols.cpp`: LowIR internal names (qualified
  components joined with `__`: `N__g`, `inc__counter`; overloads of one
  qualified name numbered in first-declaration order `f`, `f__ov2`,
  `f__ov3`; `operator delete` → `operatordelete`), symbol metadata
  (`role`, `linkage`, `binding`, `object`, `keep_alias`, `unwind`), the
  PA14 adapter (`TypeId` → `abi_mangle::AbiType`, function/variable
  targets → `mangle_target`), and on-demand `declare function` /
  `declare global` for referenced entities with no definition in the
  program.  Mangle once per emitted symbol, never per call site.
- `dev/src/lower/lowir_function.cpp`: function definitions (header,
  parameter slots + entry stores, implicit `return i32 0` for `main`),
  frame naming rules, blocks, and statement lowering (compound, local
  declarations, expression statement, return, if, while, do, for, switch,
  case/default, break/continue, condition declarations; goto/labels in
  CP2).
- `dev/src/lower/lowir_expr.cpp`: expression lowering over `SemaKind`.
- `dev/src/lower/lowir_globals.cpp` (CP4): global definitions, constant
  initializer evaluation, structured data, string-literal globals,
  `@__cppgm_init`.
- `dev/src/lower/lowir_program.cpp`: `LowerTranslationUnit`, top-level
  walk (functions in source order, globals in source order, declarations
  and init function appended at the end), public entry for the driver.

Semantic facts PA15 adds to the PA12 model (dump-neutral, no PA12 fixture
changes): `Binding::{c_linkage, is_static, is_extern_declaration}`,
`FunctionEntity::{c_linkage, internal_linkage, no_throw, declaration_index,
default_arguments (CP2)}`, set in `BuildSimpleDeclaration`,
`BuildFunctionDefinition`, `BuildLinkage` (a C-linkage depth counter while
building `extern "C"` children) and `DeclareFunction`.  Internal linkage of
a namespace-scope object: `static`, or const-qualified and not `extern`
(3.5p3).  `Pa6TokenCollector::emit_literal_array` currently discards the
decoded code units; keep them on `Pa6Token` (`lit_data`) so string
literals (CP3) reuse PA3's decoder instead of re-decoding escapes.

### Output layout (byte-exact against fixtures)

Top level, in order: `declare function`/`declare global` lines, blank line,
`global` definitions, blank line, `function` definitions with no blank line
between functions, `@__cppgm_init` last.  A function:
`function @f(%a : i32, %r : ptr [pass=reference]) -> i32 [meta] {`, then
`  slot $x : T` lines, a blank line (omitted when there are no slots), then
blocks `  block ^label:` with 4-space instructions and one blank line
between blocks, then `}`.  Metadata order: `arity`, `effects`, `unwind`,
`return`, `role`, `linkage`, `binding`, `object`, `keep_alias`,
`prefer_local`, `trivial_lifecycle`, `force_inline`.  `main`:
`[role=entry, binding=strong, keep_alias=yes]` and no `object`.  Other
symbols: `[unwind=no]` first when `noexcept`/`throw()`, then `linkage=c`
for C linkage, `binding=strong|internal`, `object=<mangled>` only when the
object name differs from the LowIR name (extern "C" external symbols use
their source name and omit `object`; internal-linkage symbols inside
`extern "C"` are still mangled: `_ZN1a1fEv`, `_ZN1nL5valueE`).
Declarations: `declare function @sink(%arg0 : ptr) -> i32 [arity=variadic,
linkage=c, binding=strong]` (parameters `%argN`), `declare global @target :
ptr [binding=strong, object=_Z6target]`, untyped `declare global
@source__text [...]` for arrays.  Floating operands print the source token
spelling (`1.25F`, `1.0f`, `4.0L`, `0.0`); integers print decimal values.

### Frame naming (all per function, fixture-pinned)

- Temps `%tN`, N from 1, one per value-producing instruction; skip any N
  for which `tN` is a parameter name (`same(const int &t1, const int &t2)`
  starts at `%t3`).
- Labels `^entry`, then `^<kind>_<N>` from one counter, allocated when a
  construct is entered, before its condition is lowered: if → `if_then`,
  `if_else`, `if_end` (the else block is always emitted, `jump ^if_end`);
  while → `while_cond`, `while_body`, `while_end`; do → `do_body`,
  `do_cond`, `do_end`; for → `for_cond`, `for_body`, `for_iter`,
  `for_end`; switch → `switch_dispatch`, `switch_end`, then a pre-scan of
  the body (not descending into nested switches) allocates
  `switch_case`/`switch_default` in source order, the dispatch block is
  emitted at once with its `switch` terminator, then the body, then the
  end block (always emitted); `&&`/`||` in branch context → `land_rhs` /
  `lor_rhs`; in value context → `land_rhs`, `land_short`, `land_end` (or
  `lor_*`) plus a slot; conditional → `cond_then/else/end` with a slot,
  lvalue conditional → `condaddr_then/else/end` with a `ptr` slot, void
  conditional → `discard_cond_then/else/end` and no slot; goto label →
  `goto_N` on first mention.  Blocks appear in the order they are started;
  a block that is still open when a label statement or case label arrives
  gets `jump ^label`.
- Slots in encounter order: parameters (`$name`, unnamed → `$__paramN`),
  each local when its declaration is lowered (nested blocks included; a
  second local of the same name in one function is `$name__shadow2`, then
  `__shadow3`), generated slots `$cond__K`, `$condaddr__K`, `$land__K`,
  `$lor__K`, `$refarg__K` from one shared counter that skips any K whose
  name is already a source slot (`int cond__1` → generated `$cond__2`).
  `$land__K`/`$lor__K` are `i64`; `$cond__K` has the conditional's type.

### Lowering conventions (fixture-pinned unless marked open)

- Parameters: every parameter gets `slot $p : T` and `store T %p, $p` at
  entry in order; reference parameters are `%p : ptr [pass=reference]`
  with a `ptr` slot; decayed array/function parameters are `ptr` (open:
  `[pass=decay]` per lowir.md, no fixture).
- Operand evaluation order: binary/comparison → `Value(lhs)`, `Value(rhs)`,
  then conversions lhs then rhs, then the instruction; call → each
  argument fully (value, conversion, reference materialization) in order,
  then the callee expression for indirect calls, then `call`; simple
  assignment → `Value(rhs)`, `Address(lhs)`, `store`, result = the stored
  temp (lvalue = the address); compound assignment → `Address(lhs)`,
  `load`, `Value(rhs)`, conversions to the operation type, op, `store`;
  prefix `++`/`--` → `load`, `add/sub T v, 1`, `store`, rvalue = new temp,
  lvalue = address; postfix → same but value = old temp; comma →
  `Discard(lhs)` then `Value`/`Address`(rhs).
- Immediates: integer/char/bool literals and enumerators are immediates
  (decimal); `sizeof` is `%t = const i64 N`; `unary neg` on a literal is
  an instruction (`return -1` → `unary neg i32 1`, `convert sext i64 i32`).
  An integral literal converted to a narrower, same-width or signed 64-bit
  integral type becomes an immediate of the target type; widening a
  literal into an unsigned 64-bit type is explicit (`convert sext i64 i32
  2` for `size_t` contexts, `return i64 1` for `long`); floating literal
  conversions are explicit (`convert fptrunc f32 f64 0.125`).
- Conversions (`Convert(value, from C++ type, to C++ type)`): same LowIR
  text → nothing; same width, different signedness → `copy T v` (`copy u32
  %t`, `copy u8 %t`); widening → `convert sext|zext TO FROM v` by source
  signedness; narrowing → `convert trunc TO FROM v`; int↔float →
  `sitofp|uitofp|fptosi|fptoui`; float↔float → `fpext|fptrunc`; bool→int
  → `convert zext i32 u8 v` even when `v` is an `i64` cmp temp (C++ types
  drive the spelling, never temp types); `nullptr`→pointer → `copy ptr
  nullptr`; integer→pointer → `copy ptr v`; zero literal against a
  pointer → immediate `0`; pointer→pointer casts and reference casts emit
  nothing (address reuse); cast to `void` → `Discard`.
- Arithmetic: `binary add|sub|mul|div|mod|and|or|xor|shl|shr T` with
  `udiv|umod|ushr` for unsigned C++ types; `cmp eq|ne|lt|le|gt|ge|ult|ule|
  ugt|uge T` (result is a canonical `i64` truth temp); `!x` → `cmp eq T x,
  0` (`u8` for bool operands); `~` → `unary bitnot`; unary `-` → `unary
  neg`; unary `+` → operand after promotion.
- Truth branch on a value (statement conditions after `Branch` recursion,
  `?:` conditions, value-context logical LHS): integral/bool/enum → `branch
  v`; floating → `cmp ne fT v, 0.0` then branch (open: pointers → `cmp ne
  ptr v, 0`).  `Branch(node, T, F)` for `&&`: allocate `land_rhs`,
  `Branch(lhs, land_rhs, F)`, start `land_rhs`, `Branch(rhs, T, F)`; `||`
  symmetric with `lor_rhs`; anything else → `Value` + truth branch.  Only
  if/while/do/for conditions use `Branch`; a `?:` condition and a
  value-context logical LHS use `Value` then a truth branch (a nested
  logical operand there gets its own slot).  `Value(&&)`: allocate labels
  and slot, `Value(lhs)` + truth branch → rhs/short; rhs block: `r =
  Value(rhs)`, `cmp ne i64 r, 0` (always `i64` for integral r; open for
  floating r), `store i64`, jump end; short block stores `0` (`1` for
  `||`); end block `load i64 $slot` is the bool result.  A logical node
  whose LHS is a literal that short-circuits (`true || …`, `false && …`)
  is the immediate `1`/`0` and its RHS is never lowered (PA12 records this
  as `has_value`; no other `has_value` folding is applied — `1 + 2` and
  `1 < 2` stay instructions).
- Conditional: value form stores each converted arm into `$cond__K`, `load`
  at the end; lvalue form (both arms lvalues of one type, or array
  operands) stores addresses into `$condaddr__K` and the loaded pointer is
  used directly (no `unary decay` after a condaddr load); void form has no
  slot; a literal condition branches on the immediate (`branch 1`).
- Storage access: local scalar `load T $x` / `store T v, $x`, address `addr
  $x`; global `load T @g`; reference slot/param `load ptr $r` is the
  address, value = `load T` through it; local/global array or function
  entity → `addr $a`/`addr @f` and, when converted to a pointer, `unary
  decay ptr`; a reference-to-function/array load also gets `unary decay
  ptr` when it decays.  Subscript → `index T [projection=array_element]
  base, idx` (idx as-is, immediate or its own width) then load/store;
  pointer ± integer → `binary mul i64 idx, size` (idx converted to i64;
  the `mul` is omitted when size is 1) then `index i8 p, off`, subtraction
  first negates with `binary sub i64 0, off`; pointer difference → `binary
  sub ptr a, b` then `binary div i64 d, size`.
- Locals: scalar `T x = e` → `store T v, $x`; reference `T& r = e` →
  `store ptr Address(e), $r`; converted temporaries bound to a const
  reference (argument or local) → `$refarg__K : Referent` store then
  `addr`; array `T a[N] = {…}` → `%b = addr $a`, `store` element 0 at
  `%b`, later elements at `index i8 %b, byteoffset`, missing elements
  store zero; no initializer → nothing.
- Calls: direct `%t = call T @f(args)` / `call void @f(args)`; indirect
  `call T %callee(args) as (%arg0 : T [pass=reference], …) -> T`; variadic
  callees `[arity=variadic]` and default promotions on extra arguments
  (`convert fpext f64 f32`); default arguments come from the semantic tree
  (CP2) and are lowered in argument position.
- Return: `return T Convert(v)`, `return ptr Address(e)` for reference
  results, `return void`, `return {}` → `return T 0`; `main` falling off
  the end → `return i32 0` (open: other functions get `return T 0`).
- Statements after a terminator in the same block: open; start a fresh
  `^dead_N` block rather than dropping code.
- 6.7p3: a `case`/`default` label reached after a declaration with an
  initializer in the same switch-body block (still in scope) is an error
  (`100-switch-label-bypasses-initialization-bad`); checked in switch
  lowering, since PA12 fixtures must keep their current acceptance.
- Globals (CP4): scalar `global @g : T [meta] = literal|zero|addr @s|addr @s
  + bytes` (`= zero` only when no initializer; an explicit `0` prints `0`);
  arrays are structured `global @a [meta] = { T lit … | ptr addr @s | zero
  bytes }` with consecutive zero elements merged; constant evaluation
  accepts folded integrals, enumerators, `&obj`, `&f`, decayed
  array/function names, `array + N`, casts of constants, `?:` with a
  constant condition, string literals (`addr @__strlit__N`, `[binding=
  internal]`, `i8` items plus terminator, numbered in first-use order);
  everything else (pinned: `&values[1]`) initializes the object to `zero`
  and appends a store to `@__cppgm_init() -> void [role=init,
  binding=internal]` in declaration order.

## Failure Map (at planning)

All 109 fixtures fail with EXIT_NOT_IMPLEMENTED from the driver stub.
Running `--emit-semantics` over the inputs: 88 pass PA12, 21 are rejected
(20 of them expect EXIT_SUCCESS; `200-bad-excess-array-initializer` is
correctly rejected), and 2 EXIT_FAILURE fixtures are wrongly accepted
(`100-scoped-enum-no-implicit-int-bad`, `100-switch-label-bypasses-
initialization-bad`).  By owning boundary:

- CP1 scalar procedural lowering (39): `100-{bad-switch,continue-inside-
  switch-targets-loop,do-while-lowering,for-loop,if-else,literal-
  canonicalization,local-arith,namespace-call,nested-switch-cases-stay-
  inner,ret0,scoped-enum-previous-enumerator-bitwise-or,simple-call,sizeof-
  local-value-shadows-type-name,switch-condition-declaration,switch-label-
  bypasses-initialization-bad,unary-logical-conditional,while-break}`,
  `200-{direct-short-circuit-condition-branch,enum-class-scalar-lowering,
  extern-c-internal-functions-stay-distinct,five-arg-call,floating-
  compound-assign-integral-rhs,floating-condition-declaration-negative-
  zero,floating-logical-branch,floating-return-integral-conversion,
  for-iteration-discards-void-comma-rhs,generated-slot-name-collision,
  immediate-widening-canonicalization,
  integral-multiply-compound-assignment,literal-logical-short-circuit-
  omits-unreachable-call,local-int-slot-width,qualified-namespace-
  overload-definition-symbol,scoped-enum-underlying-type,shadowed-local-
  slot-names,signed-enum-compare-lowering,switch-case-nested-inside-if,
  unscoped-enum-promotion-overload,wide-unscoped-enum-promotion,bad-excess-
  array-initializer}`.
- CP2 semantic boundary (9 fixtures pass, 20 more become accepted):
  parser gaps (`return {}` / `return {e}`, braced-init-list as assignment
  operand, `bool (&)(char, char)` type-id in a cast); sema gaps (default
  arguments, `extern T a[]`, unary `+` on arrays, `operator delete`
  declarator-id, comma lvalue category, function typedefs, for-init
  expression, functional cast to a reference typedef, fewer braced
  elements than the bound, goto/labels, volatile modifiable lvalues,
  conditional array decay to a common pointer, `return void-call;`, scoped
  enum → int rejection).  Passing after CP2: `100-{enum-default-argument-
  constant-fold,scoped-enum-braced-assignment,scoped-enum-no-implicit-int-
  bad,unnamed-parameter-storage}`, `200-{for-init-assignment-expression,
  goto-case-block-entry-label,goto-case-block-label-after-statement,return-
  void-call-expression}`, `300-return-empty-braces-scalar`.
- CP3 references, pointers, arrays, string literals, indirect calls (34):
  `100-{array-cv-rvalue-reference-overload,c-linkage-reference-declaration-
  metadata,condition-declaration-variable-rvalue,const-integral-lvalue-
  overload-category,function-pointer-ref-call,string-hex-escape-code-unit,
  subscript-sizeof,unary-plus-array-decay}`, `200-{address-of-local-const-
  integral-uses-storage,comma-expression-xvalue-reference-return,
  conditional-array-decay-subscript,const-cast-pointer-const-drop,const-
  cast-reference-array-subscript,const-cast-reference-similar-pointer,
  const-ref-converted-float-argument,function-reference-static-cast-call,
  functional-reference-typedef-cast,inferred-local-array-bound,local-
  direct-init-array-subscript,local-function-type-typedef-reference,local-
  lvalue-reference-alias-init,lvalue-conditional-address,lvalue-
  conditional-reference-return,partial-local-array-zero-initialization,
  pointer-compound-assignment-scale,pointer-operator-array-decay,prefix-
  incdec-lvalue-address,prefix-pointer-decrement-reference-argument,
  reference-parameter-temp-name-collision,reinterpret-enum-to-pointer,
  reinterpret-reference-conditional-materialization,scalar-assignment-
  address-lvalue,scalar-reference-static-cast-return,variadic-float-
  argument-promotes-to-double}`.
- CP4 globals and program-level data (27): `100-{extern-unknown-bound-
  array-reference,global-function-pointer-argument-call,global-variable,
  using-directive-imported-value-function-body}`, `200-{comma-expression-
  lvalue-address,compound-assignment-evaluates-lhs-once,extern-c-internal-
  header-const,extern-function-pointer-indirect-call,global-address-
  reinterpret-cast-initializer,global-array-bitwise-or-enum-init,global-
  array-conditional-cast-initializer,global-array-decay-compare,global-
  array-element-address-initializer,global-array-one-past-end-pointer,
  global-array-scalar-cast-init,global-array-static-const-byte-init,global-
  object-address-initializer,global-pointer-array-null-fill,global-pointer-
  array-nullptr-init,global-pointer-array-subscript-load,included-
  namespace-global-definition,namespace-default-argument-declaration-
  lookup,nested-conditional-array-decay,pointer-deref-byte-load,postfix-
  incdec-evaluates-lhs-once,scoped-enum-global-constant-init,scoped-enum-
  unsigned-high-bit}`.

## Performance Risks

Fixtures are tiny; the requirements are structural.  Every pass is linear
in the semantic tree: one statement walk per function, one expression walk
per statement, the switch pre-scan re-walks each switch body once (nested
switches cost depth × size, bounded by nesting).  Recursion depth follows
expression/statement nesting, which the parser already bounds; keep frames
small (no per-call vectors of children) and iterate siblings through
`first_child`/`next_sibling`.  Naming: counters plus one
`unordered_set<string>` of slot names and one `unordered_map<BindingId,
slot>` per function, an `unordered_map<FunctionEntityId, name>` /
`unordered_map<BindingId, name>` per translation unit for symbol names,
so mangling runs once per emitted symbol (never per call).  String
literals: one pool keyed by content in first-use order.  The program model
is held in memory and serialized once into one `std::string`; no text is
re-parsed.  No static mutable state (batch runner).

## Checkpoint Ledger

| checkpoint | commit | outcome |
| --- | --- | --- |
| plan | this commit | stage design, failure map, conventions |
| CP1 serializer, driver mode, symbols, scalar procedural lowering | this checkpoint | 39 CP1 fixtures pass; pa15 42/109 (67 failures); through-pa14 1030/1030; file audit passed with 2 pre-existing header warnings |
| CP2 (active) PA12 semantic boundary: parser gaps, default arguments, goto/labels, array/typedef/cast/comma/volatile/return rules, scoped-enum rejection | pending | target 48/109; all 109 inputs accepted or rejected as the refs require |
| CP3 memory objects: references, pointers, arrays, string literals, indirect calls, refarg temporaries | pending | target 82/109 |
| CP4 globals, constant initializers, structured data, declarations, `@__cppgm_init` | pending | target 109/109; through-pa15 clean |
| CP5 architecture audit and cleanup (one naming authority, one conversion table, perf probe evidence in `audit.md`) | pending | 109/109; through-pa15 clean |

## Completed Checkpoint: CP1 — serializer, driver mode, symbols, scalar lowering

Delivered: `--emit-lowir` runs the PA12 pipeline and lowers every function
definition whose body uses only scalar locals/parameters, literals,
enumerators, arithmetic/bitwise/shift/comparison/logical/conditional/comma
operators, casts among scalars, `sizeof`, direct calls, and the full
statement set (if/while/do/for/switch/break/continue, condition
declarations), with fixture-exact naming, slot layout, metadata and
mangled object names.  Evidence: all 39 CP1 fixtures pass, the stage gate
is 42/109 (67 failures, down from 109), through-pa14 is 1030/1030, and
the file audit passes.  The packet scaling probe completed at 10k and 20k
functions in 3.02s/759136 KB and 6.02s/1519872 KB, respectively, showing
approximately linear time and memory growth.

### Implementation Packet

Files/symbols:

- `dev/frontend_source_sets.mk`: `FRONTEND_OBJ_BASENAMES_cppgm++ +=
  lowir_serialize lower/lowir_types lower/lowir_symbols
  lower/lowir_function lower/lowir_expr lower/lowir_program
  abi_mangle_encode abi_mangle_types`.
- `dev/cppgm++.cpp` `run_emit_lowir_mode`: keep
  `parse_source_output_invocation(args, true)`; build tokens → AST →
  `TypeTable types; SemaModel model(types); SemaTree tree; ScopeBuilder
  builder(tokens, arena, model, tree); builder.Build(root);` per input,
  then `LowerTranslationUnit(...)` into one `lowir_model::Program`; write
  `serialize_lowir_program(program)`.  Emit mode accepts a translation unit
  without `main`, as pinned by `100-sizeof-local-value-shadows-type-name`;
  the link-time main requirement does not apply to LowIR emission.
- `dev/src/lowir_serialize.cpp` (new): `std::string
  lowir_model::serialize_lowir_program(const Program &)` printing the
  layout in Stage Design.  Operand text: `Operand::text` verbatim for
  temps/slots/globals/labels/floats/`nullptr`; integers from `int_value`.
  Instruction spellings per `pa13/lowir.md` §Required PA13 Instructions
  (`%t = const T v`, `copy`, `addr`, `load T s`, `store T v, s`, `index T
  [projection=k] b, o`, `unary op T v`, `binary op T a, b`, `cmp p T a,
  b`, `convert op TO FROM v`, `call`, `jump`, `branch c, ^a, ^b`, `switch
  %s, ^d, v:^l, …`, `return T v`/`return void`).  Operand slots follow
  `dev/src/lowir_parse.cpp` (`parse_instruction_body`, lines ≈1060–1420):
  `first`/`second`/`third`, `args` for call arguments and switch arms,
  `call_params`/`call_return_type` for the `as (…)` signature.  Round-trip
  check in a debug build: `parse_lowir_program_text(serialize(p))` must
  succeed and `ValidateLowirProgram` (`dev/src/lowir_validate.h`) must
  accept it.
- `dev/src/lower/lowir_lowering.h`, `lower/lowir_types.cpp`,
  `lower/lowir_symbols.cpp`, `lower/lowir_function.cpp`,
  `lower/lowir_expr.cpp`, `lower/lowir_program.cpp` (new) as in Stage
  Design; CP1 stubs `Address()` of non-scalar entities, string literals,
  reference bindings, subscripts, pointer arithmetic and global
  definitions with `logic_error`.
- `dev/src/sema/scope_model.h` + `scope_builder.cpp`/`type_builder.cpp`:
  the linkage/storage/noexcept facts listed in Stage Design
  (`SequenceHasKeyword(specifiers, KW_STATIC|KW_EXTERN)`, a C-linkage depth
  in `BuildLinkage`, a `HasNoexceptQualifier(declarator)` beside
  `HasConstFunctionQualifier`; find the AST kind with `grep -n
  "NOEXCEPT\|EXCEPTION" dev/src/parser/ast_model.h`).  `--emit-types` and
  `--emit-semantics` output must not change.
- `dev/src/abi_mangle.h` + `abi_mangle_encode.cpp`: add
  `AbiTargetRecord::internal_linkage` and use `mangle_internal_name` for
  `ABI_TARGET_FACT_VARIABLE` when set (CP4 needs `_ZL5masks`,
  `_ZN1NL1KE`); default false keeps pa14 at 111/111.  Function targets:
  `ABI_TARGET_FACT_FUNCTION` with `ABI_FUNCTION_TARGET_PATH`,
  `qualified_name` = `N::f`, `path_operands` = parameter `AbiType`s plus
  `ABI_FUNCTION_PATH_VARIADIC`; operator functions use
  `ABI_FUNCTION_TARGET_ENCODING` with `ABI_FUNCTION_RECORD_NAME_SOURCE`
  pieces, `ABI_FUNCTION_RECORD_TERMINAL{ABI_TERMINAL_OPERATOR}` and
  `ABI_FUNCTION_RECORD_PARAMETER` records (CP2).  `AbiType` construction:
  fundamental → `ABI_TYPE_BUILTIN` (`char`→CHAR, `signed char`→SCHAR,
  `unsigned char`→UCHAR, …, `bool`→BOOL, `wchar_t`→WCHAR, `char16_t/32_t`
  →CHAR16/CHAR32, `nullptr_t`→NULLPTR), cv → `is_const/is_volatile` on the
  node, pointer/reference/array(`ABI_ARRAY_BOUND_VALUE`)/function
  (`types` = result then parameters, `variadic`) → the matching
  `ABI_TYPE_*` with children in `types`, enum/class → `ABI_TYPE_NAMED`
  with the qualified name; mirror the compact constructors in
  `abi_fact_parse.cpp` (`parse_compact_type`) for exact field use.  An
  empty `AbiDefinitionTable` suffices.  Expected spellings: `_Z1gi`,
  `_ZN1N1gEi`, `_Z5whichj`, `_Z1f1E`, `_Z1f2Op`, `_Z5scaley`,
  `_Z4sameRKiS0_`, `_Z12select_arrayRA3_Ki`, `_Z4callPFiiE`,
  `_Z6invokeRFiiEi`, `_ZdlPvS_`, `_Z6values`, `_ZL5masks`, `_ZN1NL1KE`.

Fixture groups: the 39 CP1 fixtures above are the must-pass set.  Pinning
fixtures to read first: `100-bad-switch` (switch pre-scan and block
order), `100-nested-switch-cases-stay-inner`, `200-switch-case-nested-
inside-if`, `200-direct-short-circuit-condition-branch` (branch context),
`100-unary-logical-conditional` and `300-return-empty-braces-scalar`
(value-context logical and conditional slots), `200-literal-logical-short-
circuit-omits-unreachable-call` (discard conditional, folded LHS),
`200-generated-slot-name-collision`, `200-shadowed-local-slot-names`,
`200-reference-parameter-temp-name-collision` (temp numbering skips
parameter names), `200-immediate-widening-canonicalization` and
`300-return-empty-braces-scalar` (literal widening rules),
`200-floating-logical-branch` (float truth, `!` on `u8`), `200-scoped-
enum-underlying-type` (enum widths), `200-qualified-namespace-overload-
definition-symbol` (`__ov2`, out-of-line definitions), `200-extern-c-
internal-functions-stay-distinct` (linkage metadata), `100-sizeof-local-
value-shadows-type-name` (`obj<7x1>` slot without accesses).

Required spec facts: `pa13/lowir.md` — canonical top-level order and
metadata keys (§Program Structure), `cmp` destinations are canonical `i64`
truth values, conversion operator meanings, `index` semantics and
`projection`, `switch`/`branch`/`return` terminator forms, every block
ends in exactly one terminator, temporaries defined before use, `role`
singletons.  `pa15/README.md` — `--emit-lowir -O0`, direct short-circuit
control flow for statement conditions, widened immediates may be
canonicalized, generated slot/helper names must stay distinct from source
identifiers, a required `main`, no class helpers, `EXIT_FAILURE` on any
error.  C++11: 4.5/4.7/4.8/4.9 conversions and 5p9 usual arithmetic
conversions (drive `Convert` and operation types), 5.3.3 `sizeof` is
`unsigned long`, 5.16 conditional value category, 5.14/5.15 short
circuit, 6.4p1/6.4p3 condition scopes, 6.6.3 `main` returns 0, 6.7p3
jump past initialization, 7.2p9 unscoped enum promotion, 3.5p3 internal
linkage, 7.5 linkage specifications, 13.1 overload identity (naming
`__ovN`).  Harness facts: `lowir_metadata_item_ignored_for_compare`,
`paired_lowir_function_symbol_maps` and `validate_lowir_instruction` in
`scripts/compare_results_common.pl` (read the validator before emitting
any instruction shape not present in a fixture).

Commands: build `make -C dev cppgm++`; focused `make test-pa15` (or `cd
pa15 && make test`); one case `make -C pa15 check
TEST=tests/general/100-bad-switch.t`; byte diff
`CPPGM_LOWIR_DIRECT_TEXT_COMPARE=1 make -C pa15 check TEST=…` then `diff
pa15/tests/general/X.ref pa15/tests/general/X.my`; relaxed diffs land in
`X.my.lowir.compare.diff`; exit codes in `X.my.exit_status`; semantic view
`dev/cppgm++ --emit-semantics -o /tmp/s.out pa15/tests/general/X.t`; broad
`make test-report-through-pa14` (must stay 1030/1030) and `perl
scripts/cppgm_file_audit.pl --stage pa15 --paths dev/src` (functions ≤ 240
lines, nesting ≤ 8, no source-includes-source, no `EXIT_NOT_IMPLEMENTED`
spelling under `dev/src`).

Performance probe: generate `/tmp/pa15_big.t` with N copies of a function
shaped like `int fK(int a, int b) { int s = 0; for (int i = 0; i < a; i =
i + 1) { if (i % 2 == 0 && b) s = s + i; else s = s - 1; } switch (s) {
case 0: return b; case 1: return a; default: return s; } }` plus a `main`
that sums a bounded number of calls; `time dev/cppgm++ --emit-lowir -O0 -o
/tmp/big.lowir /tmp/pa15_big.t` at N = 10000 and 20000 must scale ≈ 2×
and use memory linear in N; one extra function with a 3000-term `1 + 1 +
…` chain and one with 1000 nested `{ if (a) { … } }` blocks must complete
under the default 8 MB stack.

Known uncertainties (resolve by probing fixtures, never by special-casing
names): truth branches and `&&`/`||` RHS materialization for pointer and
floating operands (only integral RHS `cmp ne i64` and floating LHS `cmp ne
f64 v, 0.0` are pinned); `for(;;)` with a missing condition/iteration
(labels still allocated?); a non-void conditional in an expression
statement (value form assumed); non-`main` functions falling off the end;
code after a terminator (`^dead_N` assumed); `int`→bool non-literal
conversions (`cmp ne T v, 0` assumed); `char`/`short`/bool variadic
promotions; int literal → floating (`convert sitofp` assumed);
`[pass=decay]` on adjusted array/function parameters; the second and
later `__strlit__` numbering for repeated identical literals (dedupe by
content assumed); whether static functions outside `extern "C"` mangle
with `L` (the PA14 encoder cannot, so they do not); switch on `char`/enum
prints case values as plain integers (assumed from the `switch` grammar).

## Active Checkpoint: CP2 — semantic boundary and control-flow handoff

Goal: move the PA12-owned parser and semantic facts needed by the next
LowIR families across the boundary without changing earlier assignment
fixtures.  Focus first on default arguments, braced scalar/array
initialization, comma value categories, function typedef/reference casts,
array/function decay, volatile lvalues, goto/labels, and the scoped-enum
conversion rejection; keep lowering unsupported memory/global constructs
explicit and bounded.

Implementation packet: start with the failing fixtures named in the CP2
row above and trace each fact through `dev/src/parser/ast_parser*.cpp`,
`dev/src/sema/expr_sema.cpp`, `dev/src/sema/stmt_builder.cpp`,
`dev/src/sema/scope_builder.cpp`, and the corresponding `lowir_expr.cpp` /
`lowir_program.cpp` consumer.  Preserve the canonical semantic tree as
the owner of conversions and default arguments.  Use focused `check`
targets for each repaired boundary, then the pa15 stage gate and
through-pa14 gate; the next checkpoint is complete only when it increases
the accepted fixture count without weakening coverage.
