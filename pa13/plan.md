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

## Stage Design (as built)

Data flow: `parse_lowir_program_files(argv order)` → one `lowir_model::Program`
(items concatenated in file order) → `ValidateLowirProgram` →
`LowirProgramFacts` (the symbol table, entry/init/fini indices, `uses_eh`) →
`EmitCy86Program` → text written to `<outfile>`.  Any exception →
EXIT_FAILURE.  Three `dev/src` modules, listed under
`FRONTEND_OBJ_BASENAMES_lowir2cy86` in `dev/frontend_source_sets.mk`; the
parser and validator are the LowIR text boundary later stages reuse.

- `dev/src/lowir_model.h` (scaffold, extended) — the typed model: `Program`,
  `GlobalDeclaration/Definition`, `FunctionDeclaration/Function`, `Block`,
  `Instruction {kind, dest, type, source_type, op, byte_count,
  byte_alignment, index_projection, first/second/third, args, call_*,
  debug_location}`, `Operand {kind TEMP|SLOT|GLOBAL|LABEL|INTEGER|FLOAT, text
  (verbatim spelling), int_value, float_value, literal_type}`, metadata
  enums.  Two model-level rules are declared here and defined in
  `lowir_parse.cpp`: `describe_low_type` (the one width/alignment table:
  `LowTypeInfo {kind, bits, bytes, alignment, signed}`; `f80` is 16 bytes at
  16, `obj<BxA>` is B at A, throws on unknown text) and
  `instruction_result_type` (`addr/index/stack_alloc/va_start` → `ptr`, `cmp`
  → `i64`, else the written type), shared by the validator and the emitter.
- `dev/src/lowir_parse.cpp` — `LowirLexer` (whitespace-insensitive tokens:
  sigil names, identifiers/keywords, integer and floating literals with
  optional `f`/`L` suffix, `nullptr`, `obj<NxM>` pieces, `!dbg` prefix,
  punctuation; one-token lookahead returned by reference) and `LowirParser`
  (recursive descent over `pa13.gram`; each instruction is built in place in
  the caller's `Instruction`).  Unknown spellings, wrong operand classes,
  bad metadata values and `!dbg` line/column ≤ 0 throw `ParseError`.  Memory
  orders are parsed and ignored.  `serialize_lowir_program` stays undefined
  until a later stage needs it.
- `dev/src/lowir_validate.h/.cpp` — `LowirProgramFacts {unordered_map<string,
  SymbolRef{kind DECL_GLOBAL|DEF_GLOBAL|DECL_FUNCTION|DEF_FUNCTION, index}>
  symbols; int entry, init, fini; bool uses_eh}` with the lookup helpers
  (`find`, `is_function`, `is_global`, `function_definition`,
  `function_declaration`, `callee_parameters`) that both validation and
  emission resolve `@name` through.  `ValidateLowirProgram` indexes the
  symbols once (duplicate → failure), then checks aliases, `tls_for`, roles
  (`role=entry` else `@main`, exactly once; init/fini by role or legacy
  name), role domains and singleton roles, parameter metadata legality,
  function debug locations, global initializers, and each body in one pass:
  duplicate parameter/slot/block names, at least one block, every block
  terminated exactly once with nothing after the terminator, block targets,
  operand references (temporaries are any parameter or definition in the
  function), operand types against the instruction, `convert` legality,
  `unary decay` on `ptr`, bulk spans (positive size, power-of-two
  alignment), call arity against the callee's parameters (`variadic` /
  `prototype_relaxed` allow extras), indirect calls with a signature, and the
  instruction debug location; `uses_eh` is set during that same pass.
- `dev/src/lowir_cy86_codegen.h/.cpp` — `std::string EmitCy86Program(const
  Program &, const LowirProgramFacts &)`.  `FrameLayout` per function (one
  pass): values in order params (a hidden result pointer first when the
  return type is wide), then slots, then temps in first-definition order,
  each `ValueInfo {offset, type, LowTypeInfo}`; sizes `f80` 16, `obj<BxA>` B
  rounded up to 8, everything else 8; offset of value k = −(sum of sizes
  through k); `L` = total; frame = `L + 64` when the function has a wide
  parameter or any `f80`-typed instruction (dest, operand or conversion
  type), else `L`; scratch areas `SA/SB/SC` at `bp-(L+16k)`.  `Address
  {base register, displacement}` is the one memory-operand representation.
  `Cy86Writer` appends `\t<op> <operands>;\n` and `label:\n` lines to one
  string; one program-wide counter for synthesized labels.  `FunctionEmitter`
  emits one linear pass per function through per-family lowering functions
  over four operand-loading primitives: `load_value` (scalar temp at a
  width, slot/wide temp/symbol as address, literal verbatim),
  `load_address`, `load_storage_address` (scalar temp as a pointer value,
  else address) and `stage_f80_operand`.
- `dev/lowir2cy86.cpp` — the scaffold envelope (help, batch guard, `-o`
  parsing) with parse → validate → emit → `ofstream` write (unwritable file
  → failure).

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
  pointer in its register: `move64 x64 r64;` then per 8-byte chunk `move64
  z64 [x64+8j]; move64 [P+8j] z64;`.  Then `fn__F__<first block>:`.
  Epilogue label always emitted: `move64 sp bp; move64 bp [sp]; iadd64 sp sp
  8; ret;`.
- Operand loading (`T` = `[bp-off]` of a temp): 64-bit temp `move64 r64 T`;
  32-bit `move32 r32 T`; 16-bit `move64 r64 0; move16 r16 T`; 8-bit likewise
  with `move8`; literal `move64 r64 <verbatim>` (`nullptr` → `0`; f32
  literals `move32 r32 1.5f`); `$slot`, `f80` temp or `obj` temp as a value =
  its address `isub64 r64 bp <off>`; `@global` `move64 r64 g__G`; `@function`
  `move64 r64 fn__F`.  Storing a scalar result: `moveW T xW` by width.
- `const`: load literal into `x64`, store by width.  `copy`, `unary decay`:
  load into x, store.  `load T @g` = `moveW xW [g__G]` + store; `load T $s` =
  `moveW xW [bp-off]` + store; `load T %p` = `move64 x64 P; moveW xW [x64]`
  + store, and for `i32` loads through a pointer temp insert `move8 t8 32;
  lshift64 x64 x64 t8; srshift64 x64 x64 t8;` before the store.  `store T v,
  @g` = load v into x; `moveW [g__G] xW`; `store T v, $s` = `moveW [bp-off]
  xW`; `store T v, %p` = load v into x; `move64 y64 P; moveW [y64] xW`.
  `addr $s/@g/@f` = address into x64, `move64 T x64`.
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
- `convert`: `sext` = load source by width into x, `move8 t8 <64-srcbits>;
  lshift64 x64 x64 t8; srshift64 x64 x64 t8`, store at dest width; `zext` =
  load source, store at dest width; `trunc` = load 64-bit, store at dest
  width.  Int↔float go through f80 in `SA`: `sNNconvf80 / uNNconvf80 SA
  xNN`, `f32convf80 SA x32`, `f64convf80 SA x64`, each followed by the pad
  `move64 z64 0; move32 [SA+10] z32; move16 [SA+14] z16;`, then
  `f80convsNN/f80convuNN/f80convf32/f80convf64 T SA` for narrow results, or
  for an `f80` result copy `SA` into the 16-byte temp.  `f80` sources are
  staged into `SA` first.
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
  `iadd64 x64 y64 x64; move64 T x64`.  Sizes from `describe_low_type`:
  i8/u8/i1 1, i16/u16 2, i32/u32/f32 4, i64/ptr/f64 8, f80 16, obj<BxA> B.
- `copyobj BxA src, dst`: `move64 x64 <dst>; move64 y64 <src>` (an
  `obj`-typed temp source is its address `isub64 y64 bp off`); per 8-byte
  chunk `move64 z64 [y64]; move64 [x64] z64;` with `iadd64 x64 x64 8; iadd64
  y64 y64 8;` between chunks.  `zeroinit BxA dst`: `move64 x64 <dst>; move64
  z64 0; move64 [x64] z64;` then per further chunk `iadd64 x64 x64 8; move64
  [x64] z64;`.
- Calls: indirect callee first: `move64 x64 <callee>; isub64 sp sp 8+S;
  move64 [sp+S] x64;`; direct with more than four ABI args first `isub64 sp
  sp S` (S = 8 per stack arg).  ABI args in order into `x64 y64 z64 t64`
  (the hidden result pointer of a wide return first): temps `move64 r64 T`,
  literals `move64 r64 lit`, slot/`obj`-temp/`f80`-temp arguments, wide
  parameters and the hidden result pointer are addresses computed into
  `x64` then `move64 r64 x64` (emitted even when r is x64); an `f80` literal
  argument is staged into `SA` and its address passed.  Stack args: value
  into `x64`, `move64 [sp+8k] 0; move64 [sp+8k] x64;`.  Then `call fn__F;`
  or `call [sp+S]; iadd64 sp sp 8+S;`, then for a scalar result `moveW T
  xW`; wide results are written by the callee through the hidden pointer.
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
- EH: `eh_try ^h` / `eh_cleanup ^h` push a 32-byte handler record {previous
  top, `fn__F__h`, bp, sp+32} and store its address in `g____cppgm_eh_top`.
  `eh_end` pops it (`move64 x64 [g____cppgm_eh_top]; move64 y64 [x64]; move64
  [g____cppgm_eh_top] y64; move64 sp x64; iadd64 sp sp 32;`).  `throw T v` =
  load v into x, `move64 [g____cppgm_eh_value] x64;` then the dispatch;
  `resume` = the dispatch: `move64 x64 [g____cppgm_eh_top]; ieq64 z8 x64 0;
  jumpif z8 __eh_unhandled__N+1;` label `__eh_handler__N:` `move64 y64 [x64];
  move64 [g____cppgm_eh_top] y64; move64 z64 [x64+8]; move64 bp [x64+16];
  move64 sp [x64+24]; jump z64;` label `__eh_unhandled__N+1:` `move64 x64
  [g____cppgm_eh_value]; call fn____cppgm_eh_unhandled; syscall1 t64 60
  x64;` and a blank line.  `%t = exception T` = `move64 x64
  [g____cppgm_eh_value]` + store.
- Globals: `g__G:` then items.  Scalar: `data8/16/32/64 <verbatim literal>`
  by type width (`zero` → `0`), `addr @s[+n]` → `data64 fn__s` / `data64
  g__s[+n]`; `f80` → `data64 <int64 of bytes 0–7>; data16 <uint16 of bytes
  8–9>;` then six `data8 0;` (from the parsed `float_value`).  Structured:
  items in order, each preceded by `data8 0;` padding up to its natural
  alignment, `zero N` → N × `data8 0;`.

## Failure Map (at planning)

All 96 fixtures reported EXIT_NOT_IMPLEMENTED.  By owning boundary:

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
  fixtures): the `100-*` and `200-*` integer, control-flow, call, metadata
  smoke, atomic, structured-data and hook fixtures and the two `300-*`
  alias/object-metadata smokes.
- f32/f64 width parameterization (4): `100-f32-global`, `100-f64-arith-cmp`,
  `200-f64-direct-call`, `200-structured-float-data`.
- Wide values, f80 staging, obj params/returns (7): `200-{f80-direct-call,
  f80-global,f80-unary-binary-cmp,integral-float-conversions,
  float-width-conversions,direct-object-return-boundary-smoke}`,
  `100-small-direct-object-argument`.
- Exception runtime (3): `100-eh-{cleanup-resume,same-function-catch,
  end-normal}`.

## Performance Design

Fixtures are tiny (≤ 40 lines), so the requirements are structural: every
lookup is a hash probe (top-level symbols, per-function params/slots/blocks/
temps, value offsets), never a linear scan; symbol indexing, validation,
frame layout and emission are each one pass over the instruction list; the
output is one `std::string` appended in place; the lexer works on the
in-memory file text with one token of lookahead; no regex.  Measured on
generated 50k/100k/200k-instruction probes (`audit.md`): every family scales
×2 per doubling, at roughly 10 µs and 1.7 KB of peak RSS per instruction
end to end; parsing is about half of the time, emission a third.  The
remaining cost is the scaffold model's per-instruction string payloads
(`Instruction` is 624 bytes) and the string-keyed opcode/type dispatch that
model implies; it is accepted for the text adapter and is the first thing
to retype when a later stage makes LowIR the production model.

## Checkpoint Ledger

- CP1 (`f0308c6b9`) LowIR lexer, parser, typed model, validator, and driver
  wiring — 29/96 (all EXIT_FAILURE fixtures through real validation; every
  clean fixture parses and validates), through-pa12 823/823, file audit
  passing.
- CP2 (`4b6b4e4e3`) CY86 codegen core: frame layout, prologue/epilogue,
  `start` with init/fini, integer/pointer/f32/f64 instruction families,
  index/copyobj/zeroinit, direct/indirect/stack-arg calls, terminators,
  atomics, scalar and structured globals — 86/96.
- CP3 (`b461c8a45`) wide values and EH: f80 staging and conversions,
  `f80`/`obj` parameters and returns through hidden pointers, EH handler
  stack and runtime support emission — 96/96, through-pa12 823/823.
- CP4 (final commit) architecture audit and cleanup: one symbol authority
  shared by validation and emission, one result-type rule and one width
  table in the model layer, typed memory addresses instead of re-parsed
  operand text, in-place instruction parsing, single-pass validation,
  measured linear scaling — 96/96, through-pa13 919/919, file audit
  passing with only the pre-existing `recog_parser.h` warning; findings,
  changes and evidence in `audit.md`.
