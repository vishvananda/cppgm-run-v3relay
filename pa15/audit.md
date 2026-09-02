# PA15 Final Architecture Audit — cppgm++ --emit-lowir

## Scope and method

Traced representative facts from their first typed owner to the emitted
LowIR and accounted for every classification, name, cache, and render on
the way:

- **A function symbol** (`N::f(int)` defined, `consume` declared `extern
  "C"`, `undefined_fn` only named by `&undefined_fn`): `CollectSymbols`
  records one `FunctionSymbol` per entity (first declaration, definition);
  `NameSymbols` renders the LowIR name from the scope components joined with
  `__` and the object name from the same components through the PA14
  encoder; `TopLevelName` makes the name unique in the program; every use
  (call, address, initializer) passes through `FunctionSymbolName`, so
  `BuildDeclarations` emits `declare function` exactly for the referenced
  entities the unit never defines.
- **A namespace-scope object** (`extern int x; int x = 3;`, `static const
  unsigned n::value`, `int a[5] = {1, 2}`, `int *p = &values[1]`): the
  semantic layer links a redeclaration to its first binding
  (`Binding::redeclared_binding`); one `GlobalSymbol` per object merges the
  linkage facts and remembers the defining node; `ConstantGlobalItem`
  decides constant data, zero runs, or a startup store; `GlobalFor` serves
  every load and store; the object name comes from `mangle_target` with the
  variable target's `internal_linkage` fact.
- **A scalar conversion** (`unsigned long ul = 4`, `bool b = 256`, `return
  d` from `double`): `LowInfoOf` classifies the C++ type once from the type
  table (kind, width, signedness); `Convert` is the one rule set; the LowIR
  text is rendered once by `RenderLowType` and never parsed back.
- **A local object and its slot** (`int t1` beside parameter `%t1`, a second
  `x` in a nested block, `$cond__2` beside `int cond__1`): parameters, then
  every body declaration in encounter order, through one slot-name set; the
  generated-slot counter skips source names; temporaries skip parameter
  spellings.
- **A statement condition and a value-context logical**: labels are
  allocated in source order before the condition is lowered; branch context
  threads `land_rhs`/`lor_rhs`; value context stores the canonical truth in
  an i64 slot.
- **A goto label**: the semantic layer assigns a per-function ordinal to the
  labeled statement and to every goto naming it, checks 6.7p3 for each jump,
  and lowering indexes blocks by the ordinal; no token spelling is read.
- **A string literal**: the token's decoded code units become one internal
  structured global numbered across the program, then `addr` and decay.

Compared the pre-cleanup executable (built from `eb7d5c227`) and the final
one on all 109 fixtures, on README-derived probes, and on generated scaling
families, with interleaved runs and medians of five; profiled with `perf`.

## Ownership (final)

| fact | owner | consumers |
| --- | --- | --- |
| linkage, `extern`, `noexcept`, default arguments, object redeclaration identity, label ordinals, 6.7p3 jump legality | `ScopeBuilder` (`scope_builder.cpp`, `stmt_builder.cpp`) into `Binding`, `FunctionEntity`, `SemaNode` | lowering reads them; the semantic dump prints none of them |
| LowIR storage class of a type | `LowInfoOf` + `RenderLowType` (`lowir_types.cpp`) | conversions, slot and parameter types, data items |
| ABI object names | PA14 `mangle_target` through `MangleFunction` / `GlobalObjectName` | symbol metadata; the operator vocabulary bridge is `OperatorWord` |
| program-wide names, external-symbol identity, startup initializer, string-literal numbering | `ProgramLowering` | every unit's `Lowerer` |
| unit symbol tables | `CollectSymbols` / `NameSymbols` (`lowir_symbols.cpp`) | `FunctionSymbolName`, `GlobalFor`, declarations, definitions |
| global data and dynamic initialization | `BuildGlobalDefinitions` / `ConstantGlobalItem` / `GlobalAddress` | `BuildGlobalInitializers` |
| function frame: blocks, temps, slots, terminators | `lowir_function.cpp` (`StartBlock`, `NewTemp`, `NewGeneratedSlot`, `AddSourceSlot`, `Emit*`) | statements and expressions |
| statements and conditions | `lowir_program.cpp` | — |
| expressions, one conversion rule set, operator spellings | `lowir_expr.cpp` (`Convert`, `BinaryName`, `CompareName`, `ConversionName`) | — |
| canonical text | `serialize_lowir_program` (`lowir_serialize.cpp`) | driver |

## Findings and changes

1. **Quadratic block and slot bookkeeping (material, fixed).** Every emitted
   instruction found its block by scanning all block labels, every new block
   scanned for duplicates, and every slot scanned all slot names.  A function
   with 20 000 conditionals took 34 s (4.3× per doubling); it now takes
   0.71 s (2× per doubling).  Blocks are indexed, labels and slot names are
   sets, and a block started twice is an internal error instead of a silent
   merge.
2. **Global array zero runs doubled geometrically (fixed).** The pending zero
   run was fed back into itself for every missing element: `int a[5] = {1,
   2}` produced `zero 16`, `int b[6] = {7}` produced `zero 64`; any array with
   three or more trailing missing elements was wrong (fixtures had at most
   two).  The loop also visited every element of the bound: a 50 000 000-
   element array took 1.06 s.  Runs are now summed once from the element
   size; the loop is bounded by the written initializers.
3. **Valid programs rejected (fixed).** `for (;;)` and any `for` without a
   condition ("for statement shape"), unbraced loop bodies (`while (c) i++;`
   reached "statement node"), value-returning functions whose end is
   unreachable after `while (true)` or an exhaustive `switch` ("falling off
   a non-void function"), and every declaration directly after a case label.
   Loop bodies are lowered as statements, the for-statement's parts are
   recognized by kind, and a value-returning function that can fall off its
   end gets a zero terminator (main's `return i32 0` is the same rule).
4. **Invalid LowIR emitted (fixed).** `%` and `>>` were spelled `rem`,
   `ashr`, `lshr`, none of which is a LowIR operator; unsigned 64-bit `/`
   and `%` used the signed forms (now `udiv`/`umod`, `ushr`); float to signed
   integer was always `fptoui` (now `fptosi`); conversion to `bool`
   truncated (`bool b = 256` stored 256; now `cmp ne`, immediates fold); an
   address-only reference to a declared function got no `declare function`;
   `extern int x; int x = 3;` emitted a declaration and a definition
   (duplicate top-level symbol); two `declare global` lines printed on one
   line; `int *p = 0;` printed `= 0` for a pointer; `int *p = nullptr;`
   scheduled a spurious startup store.  Fixture refs never exercised these
   shapes.
5. **6.7p3 jump checks below the owning layer (fixed).** The semantic check
   rejected any declaration right after a case label, which is legal when no
   later label enters its scope, and missed real bypasses (`case 1: { int x
   = 5; case 2: return x; }`, a forward `goto` past `int x = 5;`).  The
   semantic layer now records each initialized automatic object with its
   scope and position, checks case labels against the switch entry, and
   checks gotos once the body is complete, walking only the scopes the jump
   enters (O(depth · log n) per target).  Labels get ordinals there too, so
   lowering stopped resolving label spellings.
6. **Two naming authorities (fixed).** Qualified names were joined with `::`
   and split again to spell the LowIR name, and object names of variables
   came from a second, hand-written Itanium emitter beside the PA14 encoder
   (whose `internal_linkage` variable fact, added in CP1 for this purpose,
   was never used).  One scope-component walk feeds both spellings, and
   every object name comes from `mangle_target`.
7. **Textual downgrades in lowering (fixed).** Conversions rendered the LowIR
   type string and parsed it back with `describe_low_type` to learn width and
   class, up to four times per conversion; `noexcept` was recognized by a
   substring search on the qualifier text (`noexcept(false)` counted as
   no-throw, `throw()` did not).  `LowInfo` is computed from the type table
   and rendered once; the qualifier is classified by its first token with
   the operand evaluated as a constant.
8. **Two conversion routines (merged).** `Convert` and `ConvertExpression`
   differed only in an unreachable immediate-to-pointer branch; one rule set
   remains, with the fixture-pinned immediate policy stated once.
9. **Duplicated work and stale state (removed).** Global initializer items
   were classified twice (once for data, once to decide dynamic init); three
   copies of the operand constructors and literal predicates lived in three
   units; `Value::binding`, `MakeBoolValue`, `LowerExpression`,
   `AddBlockIfMissing`, a `BindingFor` whose branches both returned `strong`,
   the unused `seen` set of `CollectFunctions`, a dead callee assignment,
   unused `literal_type` writes, the AST arena and root parameters (used
   only for a check the driver makes), nine `const_cast<TypeTable&>` sites
   (the driver passes the mutable model), and the "CP1" error prefixes.
10. **Multi-unit programs (fixed).** The README contract is one program from
    N source files, but units were lowered independently and their LowIR
    concatenated: a function defined in one unit and declared in another
    was a duplicate top-level symbol, two unit-local `static` functions of
    one name collided, and each unit would have emitted its own `role=init`
    singleton.  `ProgramLowering` owns the program-wide facts: an external
    symbol keeps one LowIR name across units (keyed by its object name), a
    unit-local name gets `__ovN`, one startup initializer accumulates unit
    by unit, string literals number across units, and declarations that a
    definition satisfies are dropped when the program is finished.  The
    LowIR text parser is no longer linked into the compiler.
11. **Canonical top-level layout (fixed).** One blank line separates
    declarations, globals, and functions, as the fixtures and lowir.md show;
    the relaxed comparison had hidden the missing line.
12. **Small contract gaps (fixed).** The value-context logical compared a
    pointer right operand as `i64`; `throw()` did not mark `unwind=no`;
    array elements whose initializer is not a constant were silently zeroed
    (now stored by the startup initializer at their byte offset).

Accepted at stage scale, with measurements: the PA13 LowIR model is
text-typed with owning strings (`Instruction` 624 bytes, `Operand` 96,
`LowType` 32); the 760 403 instruction records of the 20 000-function probe
are 474 MB by themselves against a 1.1 GB peak, of which the front end holds
674 MB (the same input under `--emit-semantics`).  The profile of the final
executable has no lowering symbol above 1.5 %: allocation 9 %, page faults
8.6 %, instruction copies 2.7 %, `Children()` vectors below 1 %.  A compact
typed LowIR (enums for operators and kinds, interned type ids, operand ids)
belongs to the PA13 boundary and its consumers (`lowiropt`, `lowir2cy86`,
`lowir2native`) and is the next structural target; emitting by move follows
from it.  The PA14
encoder takes a `::`-joined qualified name at its API boundary; lowering
joins the known components once there.  Per-unit tables are `std::map`s
keyed by entity ids (a few thousand entries at most).  The PA13 C++
validator behind `lowir2cy86` is stricter than the harness oracle: it
rejects fixture-pinned shapes such as `copy u32 %t` of an i32 value and
`branch` on a loaded i32, including the checked-in refs
`200-floating-logical-branch` and `200-direct-short-circuit-condition-branch`;
probes without those shapes validate through it.  Two unit-local functions
of one name in different units share an `object=` name; their LowIR names
differ, and ELF local symbols may repeat.  When PA16 adds class types, the
6.7p3 records must also count declarations without an initializer whose type
has a non-trivial constructor.

## Behaviour on checked-in and probe inputs

All 109 fixtures: identical exit status, and byte-identical LowIR between
the pre-cleanup and final executables apart from the blank line of finding
11.  Probes not pinned by fixtures, pre-cleanup → final:

| input | before | after |
| --- | --- | --- |
| `int f(int a) { for (;;) { if (a) return a; } }` | rejected | `for_cond` jumps to the body; `for_end` returns `i32 0` |
| `while (i < 3) i++;` (unbraced body) | rejected | lowered |
| `int k(int a) { switch (a) { case 1: return 1; } }` | rejected | trailing `return i32 0` |
| `int a[5] = {1, 2}; int b[6] = {7};` | `zero 16`, `zero 64` | `zero 12`, `zero 20` |
| `int big[50000000] = {1, 2, 3};` | 1.06 s, `zero 562949953421312` | 0.00 s, `zero 199999988` |
| `int undefined_fn(int); ... &undefined_fn` | no declaration | `declare function @undefined_fn(...)` |
| `extern int x; int x = 3; extern int y;` | two declarations on one line plus `global @x` | `declare global @y`, `global @x` |
| `int *p = nullptr;` / `int *q = 0;` | startup store / `= 0` | `= zero` / `= zero`, no initializer function |
| `int f(double d) { return d; }` | `convert fptoui i32 f64` | `convert fptosi i32 f64` |
| `bool h(int i) { return i; }` / `bool k(double d) { return d; }` | `convert trunc u8 i32` / `convert fptoui u8 f64` | `cmp ne i32 %t, 0` / `cmp ne f64 %t, 0.0` |
| `case 1: int x = 5; return x;` (no later label) | rejected | accepted |
| `case 1: int y; y = 1; ... case 2: y = 2;` | rejected | accepted (no initializer) |
| `case 1: { int x = 5; case 2: return x; }` | accepted | rejected: jump bypasses variable initialization |
| `if (s) goto skip; int x = 5; skip:` | accepted | rejected |
| `int f() throw() { return 1; }` | no `unwind` | `[unwind=no]` |
| `u1.t u2.t`: `static int helper` in both, `extern int counter` in one, defined in the other, startup stores in both | duplicate top-level symbols | one program: `@helper`, `@helper__ov2`, one `@counter`, one `@__cppgm_init`; validates through `lowir2cy86` |
| `%` and `>>` on `int`, `/` on `unsigned long` | `binary rem`, `binary ashr`, `binary div` | `binary mod`, `binary shr`, `binary udiv` |

## Performance evidence

Immutable executables, interleaved, medians of five; wall seconds and peak
RSS in MB.  `wide N` is N copies of the plan's loop-and-switch function;
`blocks N` is one function with N sequential `if`s; `slots N` one function
with N locals; `chain` a 3000-term `+` chain; `nested` 1000 nested blocks.

| probe | before | after | RSS before → after |
| --- | --- | --- | --- |
| wide 10k / 20k / 40k | 3.04 / 6.18 / 12.47 | 2.69 / 5.58 / 11.29 | 761 / 1524 / 3042 → 550 / 1095 / 2186 |
| blocks 5k / 10k / 20k | 1.95 / 7.59 / 33.99 | 0.17 / 0.35 / 0.71 | 66 / 128 / 252 → 44 / 84 / 164 |
| slots 10k / 20k | 0.91 / 1.96 | 0.51 / 1.02 | 156 / 309 → 151 / 298 |
| chain 3000 / nested 1000 | 0.02 / 0.10 | 0.02 / 0.04 | 12 / 13 → 10 / 11 |
| globals 10k | 0.14 | 0.16 | 36 → 37 |
| 50M-element array | 1.06 | 0.00 | 5 → 5 |

Scaling after the change is linear in functions, blocks, slots, and
initializers (2× per doubling everywhere; blocks were 4.3×).  On `wide 20k`
the same executable spends 3.42 s / 674 MB in `--emit-ast`, 3.93 s / 674 MB
in `--emit-semantics`, and 5.58 s / 1095 MB in `--emit-lowir`: lowering plus
serialization adds 1.7 s and 420 MB of peak RSS; the 760 403 instruction
records are 624 bytes each and the rendered text is 29 MB.  The 3000-term chain
and the 1000-deep nesting complete under the default 8 MB stack.

## Conformance validation

- `make test-pa15`: 109/109.  `make test-report-through-pa15`: 1139/1139
  (1030 through pa14 unchanged plus 109 pa15); the PA10–PA12 dumps are
  unchanged by the new semantic facts.
- `perl scripts/cppgm_file_audit.pl --stage pa15 --paths dev/src`: passes
  with the pre-existing `abi_mangle.h` and `recog_parser.h` header-weight
  warnings; the lowering header stays under that heuristic after the shared
  vocabulary moved to `lowir_support.h`.
- README contract: `--emit-lowir -O0`; N source files form one program;
  statement conditions branch directly through `&&`/`||`; widened
  immediates are canonical; generated names stay distinct from source
  identifiers (`__ovN`, `__shadowN`, generated slots skip source spellings,
  temporaries skip parameter spellings); any error is `EXIT_FAILURE`;
  `100-switch-label-bypasses-initialization-bad` is still rejected.
- Fixture-pinned quirks kept deliberately: `convert sext i64 i32 <n>` for
  an unsigned 64-bit target while other integral immediates stay immediate;
  `cmp ne i64` for the integral right operand of a value-context logical;
  `copy u32 %t` for a same-width signedness change; pointer `+`/`-` always
  emits the element-size `mul` while `++`/`--` omits it for byte elements;
  `if_end` is omitted only when both arms terminate; one `@__strlit__N`
  per occurrence.

## Checkpoint ledger

| checkpoint | commit | outcome |
| --- | --- | --- |
| plan | `8eb912485` | stage design, conventions, failure map |
| CP1 serializer, driver mode, symbols, scalar lowering | `0ff4b7caa` | 42/109; through-pa14 1030/1030 |
| CP2 semantic boundary and control-flow handoff | `1f9072228` | 48/109 |
| CP3/CP4 memory objects, calls, globals, initialization | `eb7d5c227` | 109/109; through-pa14 1030/1030 |
| CP5 architecture audit and cleanup (this document) | final commit | 109/109; through-pa15 1139/1139; findings 1–12 |
