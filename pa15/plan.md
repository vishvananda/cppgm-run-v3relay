# PA15 Plan — cppgm++ --emit-lowir (PA12 semantic tree → PA13 LowIR)

Grading: `cppgm++ --emit-lowir -O0 -o x.my x.t`; exit status must match
`x.ref.exit_status` (3 fixtures expect EXIT_FAILURE, 106 expect EXIT_SUCCESS
plus a relaxed LowIR match).  No reference binary: the 109 `.ref` files under
`pa15/tests/general` are the only oracle.  The harness links
`dev/src/test_runner.cpp` and runs many inputs in one process: no mutable
static state.  `pa15.gram` is byte-identical to the PA10/PA12 grammar.  The
final architecture review, findings, performance evidence and validation are
in [audit.md](audit.md).

Relaxed compare (`scripts/compare_results_common.pl`, `compare_lowir_text`):
both texts are validated, metadata items `linkage`, `binding`, `object`,
`keep_alias`, `prefer_local`, `tls_for`, `trivial_lifecycle`, `effects`,
`unwind`, `return`, `capture`, `access`, `alias`, `projection` and
`storage=readonly` are dropped, function `@names` are paired and replaced by
placeholders, and top-level entries are sorted by kind rank then text.
Everything else is exact: global `@names`, `$slot` and `%temp` names,
`^labels`, `pass=`/`arity=`/`role=`, instruction order, blank lines inside a
function.  `CPPGM_LOWIR_DIRECT_TEXT_COMPARE=1` switches to byte comparison.

## Stage design

Data flow: `dev/cppgm++.cpp` `run_emit_lowir_mode` builds one
`lowir_model::Program` through one `lowir_lowering::ProgramLowering`; for
each input, PreprocEngine → `Pa10Parser` → `TypeTable` + `SemaModel` +
`SemaTree` from `ScopeBuilder` (the PA12 tree is the only semantic source of
truth) → `ProgramLowering::AddUnit(tokens, model, tree)`; then `Finish()`
(closes the startup initializer, drops declarations a definition satisfies)
→ `serialize_lowir_program` → outfile.  Any exception → EXIT_FAILURE.

Modules (`FRONTEND_OBJ_BASENAMES_cppgm++` adds `lowir_serialize`, the
`lower/` units, and the two PA14 encoder units; the LowIR text parser is not
linked):

- `dev/src/lower/lowir_support.h`: `LowInfo` (kind, width, signedness of a
  C++ type as LowIR storage), `RenderLowType`, operand constructors, and the
  shared predicates; defined in `lower/lowir_types.cpp` together with the
  PA14 `AbiType` adapter.
- `dev/src/lower/lowir_lowering.h`: `ProgramLowering` (program-wide names,
  external-symbol identity across units, the single `@__cppgm_init`,
  string-literal numbering) and the per-unit `Lowerer`.
- `lower/lowir_symbols.cpp`: `CollectSymbols` (one walk: function entities
  and namespace-scope objects, redeclarations folded through
  `Binding::redeclared_binding`), `NameSymbols` (`N__f`, `__ovN` for the
  second entity of one base name; object names through `mangle_target`),
  `FunctionSymbolName` (marks references), global data, zero runs, dynamic
  initializers, on-demand `declare function`.
- `lower/lowir_function.cpp`: frame state (indexed current block, label and
  slot-name sets, one generated-slot counter), terminators, the function
  definition builder, the resumable startup initializer.
- `lower/lowir_program.cpp`: `ProgramLowering`, conditions (`Prepare
  ConditionLabels`, `LowerCondition`, `LowerTruthBranch`), statements.
- `lower/lowir_expr.cpp`: expressions, `Convert` (the one conversion rule
  set), operator spellings (`div|udiv`, `mod|umod`, `shr|ushr`, `ult…` by
  the C++ signedness), calls, string literals.
- `dev/src/lowir_serialize.cpp`: canonical PA13 text; groups separated by
  one blank line.

Semantic facts PA15 adds to the PA12 model (dump-neutral):
`Binding::{internal_linkage, c_linkage, extern_declaration,
noexcept_qualifier, redeclared_binding}`, `FunctionEntity::{internal_linkage,
c_linkage, noexcept_qualifier, default_arguments}`, `TypeTable::
IncompleteArray`, `SEMA_LABELED_STATEMENT`/`SEMA_GOTO_STATEMENT` carrying the
label ordinal in `value`, `IsNoThrowDeclarator` (`noexcept`,
`noexcept(constant)`, `throw()`), and the 6.7p3 jump check (initialized
automatic objects per scope; case labels checked against the switch entry,
gotos once the body is complete).

## Conventions (fixture-pinned unless marked open)

Output layout: `declare global`/`declare function` lines, blank line,
`global` definitions, blank line, `function` definitions with no blank line
between them, `@__cppgm_init` last.  A function: header, `slot` lines, a
blank line when there are slots, blocks with a blank line between them.
Metadata order: `arity`, `effects`, `unwind`, `return`, `role`, `linkage`,
`binding`, `object`, `keep_alias`.  `main`: `[role=entry, binding=strong,
keep_alias=yes]`.  Other symbols: `[unwind=no]` when no-throw, `linkage=c`,
`binding=strong|internal`, `object=<mangled>` unless C linkage.

Frame naming: temps `%tN` from 1, skipping parameter spellings; labels
`^entry`, then `^<kind>_<N>` from one counter allocated when a construct is
entered, before its condition (`if_then/if_else/if_end`, `while_cond/body/
end`, `do_body/cond/end`, `for_cond/body/iter/end`, `switch_dispatch`,
`switch_end`, then `switch_case`/`switch_default` in source order,
`land_rhs`/`lor_rhs`, value-context `land_rhs/short/end`, `cond_then/else/
end`, `condaddr_*`, `discard_cond_*`, `goto_N` on first mention); slots in
encounter order (parameters `$name` or `$__paramN`, locals nested blocks
included, a second same-name local `$name__shadow2`, generated
`$cond__K`/`$condaddr__K`/`$land__K`/`$lor__K`/`$refarg__K` from one counter
that skips source names).

Lowering: parameters are stored to their slots at entry; reference
parameters are `ptr [pass=reference]`.  Binary operands evaluate left then
right, then convert, then operate; calls evaluate arguments in order, then
an indirect callee; simple assignment evaluates the right operand, then the
lvalue, then stores.  Immediates: integer/char/bool literals and enumerators;
`sizeof` is `const i64`; `-1` is `unary neg`; an integral immediate stays an
immediate when the target has the same width or signedness, otherwise the
widening is explicit (`convert sext i64 i32 2` for unsigned 64-bit targets);
floating literal conversions are explicit; conversion to `bool` is `cmp ne`
(immediates fold); same-width signedness change is `copy T v`; `nullptr` to
pointer is `copy ptr nullptr`.  `cmp` results are canonical i64 truth values
typed as the expression's `bool`; `!x` is `cmp eq`; `&&`/`||` in statement
conditions branch through operand blocks, in value context store into an
i64 slot (`cmp ne i64` for an integral right operand).  Conditional: value
form stores converted arms into `$cond__K`; lvalue/designator form stores
addresses into `$condaddr__K`; void form has no slot.  Storage: `load T $x`,
`load T @g`, references hold addresses, arrays and functions take `addr`
then `unary decay ptr`; subscript is `index T [projection=array_element]`;
pointer ± integer is `binary mul i64`, optional `binary sub i64 0`, `index
i8`; pointer difference is `binary sub ptr` then `binary div i64`.  Locals:
scalar store, reference slot stores the address (converted temporaries get
`$refarg__K`), arrays store each element (missing elements zero).  Returns:
`return T Convert(v)`, `return ptr` for references, `return void`; a
value-returning function that can fall off its end returns zero.  Globals:
scalar `= literal|zero|addr @s ± bytes`; arrays are structured data with
merged zero runs; non-constant initializers become stores in
`@__cppgm_init() -> void [role=init, binding=internal]` in declaration
order; each string literal occurrence is `@__strlit__N [binding=internal]`.

## Performance notes

Every pass is linear in the semantic tree: one symbol walk per unit, one
statement walk per function, one expression walk per statement; the switch
label pre-scan re-walks each switch body once.  Frame bookkeeping is indexed
(current block index, label and slot-name sets); global data is bounded by
the written initializers, not the array bound; scalar binary chains reduce
along an iterative left spine.  Symbol names are rendered once per symbol.
Measured scaling and the remaining cost (the text-typed PA13 instruction
record) are in [audit.md](audit.md).

## Checkpoint ledger

| checkpoint | commit | outcome |
| --- | --- | --- |
| plan | `8eb912485` | stage design, conventions, failure map |
| CP1 serializer, driver mode, symbols, scalar lowering | `0ff4b7caa` | 42/109; through-pa14 1030/1030 |
| CP2 semantic boundary and control-flow handoff | `1f9072228` | 48/109 |
| CP3/CP4 memory objects, calls, globals, initialization | `eb7d5c227` | 109/109; through-pa14 1030/1030 |
| CP5 architecture audit and cleanup | final commit | 109/109; through-pa15 1139/1139; [audit.md](audit.md) |
