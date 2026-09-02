# PA13 Plan — lowir2cy86 (LowIR text → PA9 CY86 source text)

Grading: `lowir2cy86 -o x.my x.t`; exit status must match `x.ref.exit_status`
(29 fixtures expect EXIT_FAILURE, 67 expect EXIT_SUCCESS plus a byte-exact
CY86 text match).  No reference binary: the 96 `.ref` files under
`pa13/tests/spec` are the only format oracle, so every emission convention
below is fixture-pinned.  `cppgm.tests/course/pa13` is empty.
`pa13/tests/debuginfo` belongs to later stages (`test-debuginfo`, not
`make test-pa13`).  The harness is the shared batch runner: it links
`dev/src/test_runner.cpp` as `main`, so the scaffold's `--batch-stdin` guard
in `dev/lowir2cy86.cpp` needs no work.  Inputs are lexed by a LowIR-specific
lexer, not the PA5 preprocessor (`@%$^` sigils, `!dbg(...)`, `obj<8x4>`,
`1.5f`/`1.75L` literal spellings must survive verbatim into the CY86 text).

## Stage Design

Data flow: `parse_lowir_program_files(argv order)` → one `lowir_model::Program`
(items concatenated in file order) → `ValidateLowirProgram` → `LowirProgramFacts`
(symbol index, entry/init/fini, `uses_eh`) → `EmitCy86Program` → text written to
`<outfile>`.  Any exception → EXIT_FAILURE.  Three new `dev/src` modules, all
listed under `FRONTEND_OBJ_BASENAMES_lowir2cy86` in `dev/frontend_source_sets.mk`;
the parser and validator are the LowIR boundary later reused by `lowiropt` and
`lowir2native`.

- `dev/src/lowir_model.h` (scaffold, editable) — typed model: `Program`,
  `GlobalDeclaration/Definition`, `FunctionDeclaration/Function`, `Block`,
  `Instruction {kind, dest, type, source_type, op, byte_count, byte_alignment,
  index_projection, first/second/third, args, call_* , debug_location}`,
  `Operand {kind TEMP|SLOT|GLOBAL|LABEL|INTEGER|FLOAT, text (verbatim
  spelling incl. leading '-'), int_value, float_value}`, metadata enums.  Add
  `struct LowTypeInfo {kind VOID|INT|FLOAT|PTR|OBJ; bits; bytes; align}` and
  `LowTypeInfo describe_low_type(const LowType &)` (throws on unknown text).
  Switch arms go in `args` as alternating value, label operands.  Memory
  orders are parsed and ignored.  `serialize_lowir_program` stays undefined
  until a later stage needs it.
- `dev/src/lowir_parse.cpp` — `LowirLexer` (whitespace-insensitive tokens:
  sigil names `@x %x $x ^x`, identifiers/keywords, integer and floating
  literals with optional `f`/`L` suffix, `nullptr`, `obj<NxM>` as one type
  token, `!dbg(` prefix whose payload is `file , line , column )` with the
  file read as one non-comma token, punctuation `= : , ( ) [ ] { } -> + -`)
  and `LowirParser` (recursive descent over `pa13.gram`: top-level items,
  metadata brackets, function items, every rvalue/void instruction form,
  optional trailing `!dbg`).  Keyword-driven parse of unknown spellings or
  wrong operand classes (e.g. `switch %s, %s, ...`) throws `ParseError`.
  `!dbg` line or column ≤ 0 throws.  Also owns `describe_low_type`.
- `dev/src/lowir_validate.h/.cpp` — `struct LowirProgramFacts {unordered_map
  <string, SymbolRef{kind DECL_GLOBAL|DEF_GLOBAL|DECL_FUNCTION|DEF_FUNCTION,
  index}> symbols; int entry, init, fini (function indices, -1 = none); bool
  uses_eh;}` and `LowirProgramFacts ValidateLowirProgram(const Program &)`.
  Checks (each a `runtime_error`): duplicate top-level names across
  declarations and definitions; alias object spelled twice or targeting a
  non-top-level symbol; `tls_for` on non-functions, targeting a non
  thread-local global, or two wrappers for one global; role legality (function
  roles on functions, global roles on globals, singleton roles once; entry =
  `role=entry` else `@main` definition, required exactly once; init/fini =
  role or legacy `@__cppgm_init`/`@__cppgm_fini` definitions); unknown
  metadata key or value in any bracket; symbol keys (`role linkage binding
  object tls_for keep_alias prefer_local trivial_lifecycle force_inline
  storage`) on call signatures; `storage` only on globals, function-only keys
  only on functions; parameter `pass≠direct`, `capture`, `access`, `alias`
  require type `ptr`; `indirect_result` only first parameter and only with
  `void` return (function headers and call signatures alike); per function:
  duplicate parameter/slot/block names, at least one block, instruction
  before the first block, every block ends with exactly one terminator
  (`jump branch switch return throw resume`) and nothing follows it, every
  block target defined in the same function, every temp used is a parameter
  or defined by some instruction, slots declared, `@` operands present in the
  symbol table; `convert` legality (`zext/sext`: int→wider int; `trunc`:
  int→narrower int; `sitofp/uitofp`: int→float; `fptosi/fptoui`: float→int;
  `fpext`: float→wider; `fptrunc`: float→narrower); `unary decay` requires
  `ptr`; `copyobj/zeroinit` alignment a positive power of two and byte count
  positive; indirect calls (callee is a temp or global-as-value) require the
  `as (...) -> ...` signature; `index` projection ∈ {array_element, field,
  base_subobject, reference_field}.  Sets `uses_eh` when any of
  `eh_try eh_cleanup eh_end throw exception resume` appears.
- `dev/src/lowir_cy86_codegen.h/.cpp` — `std::string EmitCy86Program(const
  Program &, const LowirProgramFacts &)`.  `FrameLayout` per function (one
  pass): values in order params (a hidden result pointer first when the
  return type is `f80` or `obj<…>`), then slots, then temps in first-definition
  order; sizes `f80` 16, `obj<BxA>` B rounded up to 8, everything else 8;
  offset of value k = −(sum of sizes through k) so the first value is
  `[bp-8]`; `L` = total; frame = `L + 64` when the function has an `f80` or
  `obj` parameter or any `f80`-typed instruction (dest, operand or conversion
  type), else `L`; wide scratch areas `SA=[bp-(L+16)]`, `SB=[bp-(L+32)]`,
  `SC=[bp-(L+48)]`.  `Cy86Writer` appends `\t<op> <operands>;\n` lines and
  `label:\n` lines; one program-wide counter for synthesized labels.
  Per-function value map `unordered_map<string, ValueInfo{offset, type}>`.
  Emission is one linear pass per function via per-family lowering functions
  (each under the 240-line audit limit).
- `dev/lowir2cy86.cpp` — keep the scaffold envelope (help, batch guard,
  `-o` parsing); replace the `NotImplementedException` throw with parse →
  validate → emit → `ofstream` write (unwritable file → failure).

Program text (fixture-pinned; `F` = function name, `G` = global name):

- Names: `fn__F`, `g__G`, blocks `fn__F__label`, `fn__F__epilogue`;
  synthesized `__eh_handler__N`, `__eh_unhandled__N`,
  `__atomic_cmpxchg_success__N`, `__atomic_cmpxchg_end__N` (N from the one
  counter, allocated in that pair order).
- Layout: `start:` block, then every function definition in source order,
  then (if `uses_eh`) `fn____cppgm_eh_unhandled:` / `\tsyscall1 t64 60 x64;`,
  then every global definition in source order, then (if `uses_eh`)
  `g____cppgm_eh_top:` and `g____cppgm_eh_value:` each `\tdata64 0;`.  Items
  are separated by one blank line; the file ends with a single `\n`.
  Declarations and `alias object` lines emit nothing; all metadata and `!dbg`
  are validated then ignored.
- `start:` = `move64 bp sp;` [`call fn__init;`] `call fn__entry;` [`isub64 sp
  sp 8; move64 [sp] x64; call fn__fini; move64 x64 [sp]; iadd64 sp sp 8;`]
  `syscall1 t64 60 x64;`.
- Prologue: `isub64 sp sp 8; move64 [sp] bp; move64 bp sp;` then `isub64 sp
  sp <frame>;` only when frame > 0, then parameter spills: register params
  1–4 from `x64 y64 z64 t64` as `move64 [P] r64;`, params 5+ as `move64 x64
  [bp+16+8k]; move64 [P] x64;`; a wide (`f80`/`obj`) param arrives as a
  pointer in its register: `move64 x64 r64; move64 z64 [x64]; move64 [P] z64;`
  then per further 8-byte chunk `move64 z64 [x64+8]; move64 [P+8] z64;`.
  Then `fn__F__<first block>:`.  Epilogue label always emitted: `move64 sp
  bp; move64 bp [sp]; iadd64 sp sp 8; ret;`.
- Operand loading (`T` = `[bp-off]` of a temp, `V` = any value operand):
  64-bit temp `move64 r64 T`; 32-bit `move32 r32 T`; 16-bit `move64 r64 0;
  move16 r16 T`; 8-bit likewise with `move8`; literal always `move64 r64
  <verbatim text>` (`nullptr` → `0`; f32 literals `move32 r32 1.5f`); `$slot`
  as a value = its address `isub64 r64 bp <off>`; `@global` as a value
  `move64 r64 g__G`; `@function` `move64 r64 fn__F`.  Storing a result:
  `move64 T x64` / `move32 T x32` / `move16 T x16` / `move8 T x8` by width.
- `const`: load literal into `x64`, store by width.  `copy`, `unary decay`:
  load into x, store.  `load T @g` = `moveW xW [g__G]` + store; `load T $s` =
  `moveW xW [bp-off]` + store; `load T %p` = `move64 x64 P; moveW xW [x64]`
  + store, and for 32-bit integer loads through a pointer insert `move8 t8
  32; lshift64 x64 x64 t8; srshift64 x64 x64 t8;` before the `move32` store.
  `store T v, @g` = load v into x; `moveW [g__G] xW`; `store T v, $s` =
  `moveW [bp-off] xW`; `store T v, %p` = load v into x; `move64 y64 P;
  `moveW [y64] xW`.  `addr $s/@g/@f` = address into x64, `move64 T x64`.
- `binary op T a, b`: load a into y, b into x; `<opc>W xW yW xW`; store.
  Opcodes: add `iadd`, sub `isub`, mul `smul`, div `sdiv`, mod `smod`, udiv
  `udiv`, umod `umod`, and `and`, or `or`, xor `xor`; shifts insert `move64
  z64 x64; move8 x8 z8;` then `lshift/srshift/urshift W xW yW x8`; floats
  f32/f64 use `fadd/fsub/fmul/fdiv` with `move32/move64` loads.
- `cmp pred T a, b`: load a into y, b into x; `<pred>W z8 yW xW; move64 x64
  0; move8 x8 z8; move64 T x64` (result temp is i64).  eq `ieq`, ne `ine`,
  lt `slt`, le `sle`, gt `sgt`, ge `sge`, ult..uge `ult..uge`; floats `feq
  fne flt fle fgt fge`; `ptr` as 64-bit int.
- `unary neg` int: `moveW xW A; moveW yW 0; isubW xW yW xW`; `not`: `move64
  x64 A; ieq64 z8 x64 0; move64 x64 0; move8 x8 z8`; `bitnot`: `notW xW xW`;
  `bswap`: load narrow into x, `bswapW xW xW`, store by width.
- `convert`: `sext` = load source (literal via `move64 x64 lit`, temp by
  source width into x), `move8 t8 <64-srcbits>; lshift64 x64 x64 t8;
  srshift64 x64 x64 t8`, store at dest width; `zext` = load source, store at
  dest width; `trunc` = load 64-bit, store at dest width.  Int↔float go
  through f80 in `SA`: `sNNconvf80 / uNNconvf80 SA xNN`, `f32convf80 SA x32`,
  `f64convf80 SA x64`, each followed by the pad `move64 z64 0; move32 [SA+10]
  z32; move16 [SA+14] z16;`, then `f80convsNN/f80convuNN/f80convf32/
  f80convf64 T SA` for narrow results, or for an `f80` result copy `SA`
  into the 16-byte temp (`move64 z64 [SA]; move64 T z64; move64 z64 [SA+8];
  move64 [T+8] z64`).  `f80` sources are staged into `SA` first.
- `f80` values: a temp holds 16 bytes (`T` low, `T+8` high).  Stage a temp
  into scratch `S`: `isub64 x64 bp <off>; move64 z64 [x64]; move64 S z64;
  move64 z64 [x64+8]; move64 [S+8] z64;`; a global: `move64 x64 g__G` then
  the same copy.  `const f80 lit`: `move80 SA lit;` pad, copy `SA`→`T`.
  `binary` stages a→`SA`, b→`SB`, `f<op>80 SC SA SB;` pad `SC`, copy
  `SC`→`T`; `unary neg` stages a→`SA`, `move80 SB 0.0L;` pad, `fsub80 SC SB
  SA;` pad, copy; `cmp` stages a→`SA`, b→`SB`, `feq80 z8 SA SB` etc. then
  the usual i64 materialization; `load f80 @g/%p/$s` copies 16 bytes via
  `SA` into `T`.
- `index T [proj] base, off`: `move64 y64 <base>; move64 x64 <off>;` then
  unless the element size is 1: `move64 z64 <size>; smul64 x64 x64 z64;`,
  `iadd64 x64 y64 x64; move64 T x64`.  Sizes: i8/u8/i1 1, i16/u16 2,
  i32/u32/f32 4, i64/ptr/f64 8, f80 16, obj<BxA> B.
- `copyobj BxA src, dst`: `move64 x64 <dst>; move64 y64 <src>` (an
  `obj`-typed temp source is its address `isub64 y64 bp off`); per 8-byte
  chunk `move64 z64 [y64]; move64 [x64] z64;` with `iadd64 x64 x64 8; iadd64
  y64 y64 8;` between chunks.  `zeroinit BxA dst`: `move64 x64 <dst>; move64
  z64 0; move64 [x64] z64;` then per further chunk `iadd64 x64 x64 8; move64
  [x64] z64;`.
- Calls: indirect callee first: `move64 x64 <callee>; isub64 sp sp 8; move64
  [sp] x64;`; direct with more than four register args first `isub64 sp sp
  8*(n-4)`.  Register args in order into `x64 y64 z64 t64`: temps `move64
  r64 T`, literals `move64 r64 lit`, slot/`obj`-temp/`f80`-temp arguments
  and the hidden result pointer are addresses computed into `x64` then
  `move64 r64 x64` (emitted even when r is x64).  Stack args: value into
  `x64`, `move64 [sp+8k] 0; move64 [sp+8k] x64;`.  Then `call fn__F;` or
  `call [sp]; iadd64 sp sp 8;`, then for a scalar result `moveW T xW`; wide
  results are written by the callee through the hidden pointer.
- Terminators: `return T v` = load v into x by width, `jump fn__F__epilogue`;
  `return void` = the jump; `return f80 v` stages v→`SA`, `move64 x64
  [bp-8]`, writes `SA`/`SA+8` through `[x64]`/`[x64+8]`; `return obj v`
  copies from v's address through `[bp-8]` with `[y64]`, `[y64+8]` chunks.
  `jump` = `jump fn__F__b`; `branch c, t, e` = `move64 x64 C; ieq64 z8 x64 0;
  jumpif z8 fn__F__e; jump fn__F__t`; `switch s, d, v:b…` = `move64 x64 S;`
  per arm `move64 t64 <v>; ieq64 z8 x64 t64; jumpif z8 fn__F__b;` then
  `jump fn__F__d`.
- Atomics (single-threaded): `atomic_load` = `move64 y64 P; moveW xW [y64]`
  + store; `atomic_store` = `move64 y64 P;` load v into x; `moveW [y64] xW`;
  `atomic_exchange` = `move64 y64 P;` load v into x; `move64 t64 [y64];
  move64 [y64] x64; move64 x64 0; move64 x64 t64;` + store; `atomic_add_fetch`
  = `move64 y64 P; move64 x64 [y64]; move64 z64 <delta>; iadd64 x64 x64 z64;
  move64 [y64] x64;` + store; `atomic_compare_exchange` = `move64 y64 P;
  move64 z64 E; move64 t64 [y64]; move64 x64 [z64]; ieq64 x8 t64 x64; jumpif
  x8 __atomic_cmpxchg_success__N; move64 [z64] t64; move64 x64 0; move64 T
  x64; jump __atomic_cmpxchg_end__N+1;` label N: `move64 x64 <desired>;
  move64 [y64] x64; move64 x64 1; move64 T x64;` label N+1.  Fences: nothing.
- EH: `eh_try ^h` / `eh_cleanup ^h` = `isub64 sp sp 32; move64 z64
  [g____cppgm_eh_top]; move64 [sp] z64; move64 z64 fn__F__h; move64 [sp+8]
  z64; move64 [sp+16] bp; move64 z64 sp; iadd64 z64 z64 32; move64 [sp+24]
  z64; move64 z64 sp; move64 [g____cppgm_eh_top] z64;`.  `eh_end` = `move64
  x64 [g____cppgm_eh_top]; move64 y64 [x64]; move64 [g____cppgm_eh_top] y64;
  move64 sp x64; iadd64 sp sp 32;`.  `throw T v` = load v into x, `move64
  [g____cppgm_eh_value] x64;` then the dispatch; `resume` = the dispatch:
  `move64 x64 [g____cppgm_eh_top]; ieq64 z8 x64 0; jumpif z8
  __eh_unhandled__N+1;` label `__eh_handler__N:` `move64 y64 [x64]; move64
  [g____cppgm_eh_top] y64; move64 z64 [x64+8]; move64 bp [x64+16]; move64 sp
  [x64+24]; jump z64;` label `__eh_unhandled__N+1:` `move64 x64
  [g____cppgm_eh_value]; call fn____cppgm_eh_unhandled; syscall1 t64 60
  x64;`.  `%t = exception T` = `move64 x64 [g____cppgm_eh_value]` + store.
- Globals: `g__G:` then items.  Scalar: `data8/16/32/64 <verbatim literal>`
  by type width (`zero` → `0`), `addr @s` → `data64 fn__s` / `data64 g__s`;
  `f80` → `data64 <int64 of bytes 0–7>; data16 <uint16 of bytes 8–9>;` then
  six `data8 0;` (value from `strtold` of the spelling).  Structured: items in
  order, each preceded by `data8 0;` padding up to its natural alignment
  (size), `zero N` → N × `data8 0;`.

## Failure Map (at planning)

All 96 fixtures report EXIT_NOT_IMPLEMENTED.  By owning boundary:

- Parser/validator (29 EXIT_FAILURE fixtures): syntax/structure —
  `100-bad-missing-terminator`, `200-bad-duplicate-{block,slot,parameter,
  top-level-symbol,object-alias}`, `200-bad-undefined-{block-target,
  object-alias-target}`, `200-bad-instruction-after-terminator`,
  `200-bad-switch-target`; metadata legality — `200-bad-{function-arity,
  unknown-binding,unknown-function-effect,unknown-global-storage,
  index-projection}-metadata`, `200-bad-call-signature-symbol-metadata`,
  `200-bad-function-debug-zero-line`, `200-bad-nonptr-parameter-{access,
  alias,capture}`, `200-bad-reference-pass-nonptr`,
  `200-bad-indirect-result-{not-first,nonvoid-return}`,
  `200-bad-{duplicate-tls-wrapper,tls-for-non-thread-local-global}`;
  instruction constraints — `200-bad-invalid-integer-width-conversion`,
  `200-bad-unary-decay-nonptr`, `200-bad-storage-op-alignment`,
  `200-bad-missing-indirect-call-signature`.
- Integer/pointer/control codegen plus globals, calls and hooks (53 success
  fixtures): `100-{ret0,simple-call,global-variable,local-arith,if-else,
  while-break,index-slot,indirect-call,structured-global-data,
  nullptr-return-lowir,object-abi-lowered,startup-shutdown-hooks,
  legacy-runtime-hooks,function-and-instruction-debug-locations,copyobj,
  zeroinit}`, `200-{binary-int-ops,unsigned-int-ops,compare-predicates,
  unsigned-compare-predicates,unary-ops,bswap-unary,copy-instruction,
  switch-terminator,integer-width-conversions,global-pointer-address-init,
  structured-global-zero-padding,unused-declarations,
  unary-decay-pointer-smoke,index-projection-metadata-smoke,object-slot-smoke,
  object-slot-array-element-smoke,call-boundary-metadata (5 args),
  variadic-function-arity,prototype-relaxed-arity-smoke,indirect-call-signature,
  binding/linkage/function-effect/global-storage/parameter-access/
  parameter-alias/parameter-capture/thread-local-global-storage/
  thread-local-wrapper metadata smokes, atomic-{load-store,exchange,
  add-fetch,compare-exchange-success,compare-exchange-failure,fences}}`,
  `300-{object-alias,symbol-object-metadata}-smoke`.
- f32/f64 width parameterization (4): `100-f32-global`, `100-f64-arith-cmp`,
  `200-f64-direct-call`, `200-structured-float-data`.
- Wide values, f80 staging, obj params/returns (7): `200-{f80-direct-call,
  f80-global,f80-unary-binary-cmp,integral-float-conversions,
  float-width-conversions,direct-object-return-boundary-smoke}`,
  `100-small-direct-object-argument`.
- Exception runtime (3): `100-eh-{cleanup-resume,same-function-catch,
  end-normal}`.

## Performance Risks

Fixtures are tiny (≤ 40 lines), so the risks are structural: every lookup
must be a hash probe (top-level symbols, per-function params/slots/blocks/
temps, value offsets), never a linear scan; frame layout, validation and
emission are each one pass over the instruction list; output is built in one
`std::string`/`ostringstream` with no per-line reallocation of the whole
buffer; the lexer works on an in-memory copy of each file without
substring-per-character copies; no regex.  Target: a synthetic 200k-instruction
function translates in well under a second and scales linearly.

## Checkpoint Ledger

- CP1 (completed) — LowIR lexer, parser, typed model, validator, and driver
  wiring: `make test-pa13` reaches 29/96 (all 29 EXIT_FAILURE fixtures pass
  through real validation; all 67 clean fixtures parse and validate before the
  intentional translation-boundary status), through-pa12 823/823, and the
  pa13 file audit passes. The final 100k/200k probe scales 0.35s/0.72s and
  85MB/153MB RSS.
- CP2 (completed) — CY86 codegen core: frame layout, prologue/epilogue,
  `start` with init/fini, integer/pointer/f32/f64 instruction families,
  index/copyobj/zeroinit, direct/indirect/stack-arg calls, terminators,
  atomics, scalar and structured globals. Evidence: 86/96 pa13 fixtures,
  823/823 through-pa12, and the pa13 file audit pass; the ten remaining
  failures are the planned f80/object/EH boundary.
- CP3 — wide values and EH: f80 staging and conversions, `f80`/`obj`
  parameters and returns through hidden pointers, EH handler stack and
  runtime support emission: 96/96, through-pa13 clean.
- CP4 — architecture audit and cleanup (one operand-loading authority, one
  width table, one label counter, linear passes, audit warnings), `audit.md`.

## Active Checkpoint: CP3 — wide values and EH

Goal: extend deterministic CY86 emission to f80 staging, object/f80 ABI
boundaries, and exception-handler runtime support while preserving the 86
passing pa13 fixtures and all earlier PAs. Progress proof: the ten remaining
pa13 success fixtures become byte-exact EXIT_SUCCESS with no malformed-fixture
regressions.

### Implementation Packet

Files/symbols:

- `dev/src/lowir_cy86_codegen.h/.cpp`: extend the existing `FrameLayout`,
  value map, scratch staging, wide calls/returns, global encoding, and EH
  handler emission; keep every emission family under the 240-line audit limit.
- `dev/lowir2cy86.cpp`: preserve the validated parse → emit → output path and
  the existing help, batch, and `-o` envelope.
- `dev/frontend_source_sets.mk`: retain `lowir_cy86_codegen` in the
  lowir2cy86 source set; keep parser and validator unchanged.

Fixture groups: `100-small-direct-object-argument`,
`200-direct-object-return-boundary-smoke`, `200-f80-{direct-call,global,
unary-binary-cmp}`, `200-{float-width-conversions,integral-float-conversions}`,
and `100-eh-{cleanup-resume,end-normal,same-function-catch}`. Keep the 86
passing fixtures and 29 malformed fixtures as the regression boundary.

Required implementation facts: preserve the existing names and layout, use
one program-wide synthetic-label counter, 16-byte f80/object staging and
hidden result pointers, and the EH globals/handler stack specified in Stage
Design. Validate before emission, use hash maps for top-level and per-function
lookup, and build output once. Resolve unpinned shapes by the LowIR contract
and nearby fixtures, not by test-specific branches.

Commands: focused wide/object and EH checks; broad
`make test-report-through-pa12`, `make test-pa13`, and
`perl scripts/cppgm_file_audit.pl --stage pa13 --paths dev/src`.
