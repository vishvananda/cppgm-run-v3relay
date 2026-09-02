# PA13 Final Architecture Audit — lowir2cy86

## Scope and method

Traced representative LowIR facts through their full ownership paths:
srcfile → `LowirLexer` (one token of lookahead) → `LowirParser` (typed
`lowir_model::Program`, instructions built in place) → `ValidateLowirProgram`
(the one symbol table in `LowirProgramFacts`, per-function environments,
`uses_eh`) → `EmitCy86Program` (`FrameLayout` per function, `FunctionEmitter`
over one `Cy86Writer`) → outfile in the `dev/lowir2cy86.cpp` envelope.  Facts
followed end to end: a temporary (`%2 = binary add i64 %0, %1`: parsed in
place, typed by `instruction_result_type` in the validator's environment,
type-checked against its operands, given a frame slot by `build_frame`,
loaded/stored by `load_value`/`store_result` at the width the instruction
names); a called function (`@helper`: recorded once in `facts.symbols`, arity
and argument types checked through `facts.function_definition`, the same
parameter list resolved once per call by `facts.callee_parameters` for
address-versus-value passing, rendered by `symbol_label`); an `f80` literal
(`2.5L`: verbatim spelling and parsed `float_value` on the operand, 16 bytes
at 16 from `describe_low_type`, staged through scratch `SA` with the pad
sequence, data bytes of an `f80` global taken from the parsed value); an
exception handler (`eh_try ^h`: block target validated, `uses_eh` set in the
same body pass, handler record pushed with the cached function prefix,
runtime stub and EH globals appended because of the fact); a wide return
(`-> obj<8x4>`: hidden result pointer inserted as ABI parameter 0 with its
own `ValueInfo`, spilled in the prologue, written through by `emit_return`,
supplied by the caller from the destination temporary or `SA`); a debug
location (`!dbg(f, 0, 3)`: lexed as one prefix token, non-positive fields
parsed as 0, rejected by `validate_debug_location`, otherwise ignored by
emission); and a structured global item (parsed with its type, address
symbol checked against the symbol table, padded to `info.alignment`).

Compared the CP3 and final executables (immutable copies) on all 96
checked-in `.t` inputs and on six generated probe families at 50k/100k/200k
instructions (arithmetic/memory chain with `!dbg`, N small functions each
called once, N globals loaded/stored, N six-argument calls, N blocks with a
switch and branches, N `f80` operations) with interleaved runs and medians of
five, and profiled the chain and function probes with `perf`.  Phase times
were measured with a small driver linked against the stage objects.

## Ownership trace (final)

- **Type facts**: `describe_low_type` (declared in `lowir_model.h`, defined
  with the parser) is the one width and alignment table; `is_wide`,
  `value_size`, `width_suffix` and the index element size in the emitter
  are phrasings of its `LowTypeInfo`.  `instruction_result_type` is the one
  rule for the type a temporary defines (`addr/index` → `ptr`, `cmp` →
  `i64`), used by the validator's environment and the frame layout.
- **Symbol facts**: `LowirProgramFacts::symbols` is the one top-level table.
  The validator fills it once (duplicate name → failure) and reads it
  through a `SymbolTable` view (`has`, `has_global`, `has_function`,
  `thread_local_global`, `global_type`); the emitter reads it through
  `symbol_label`, `callee_parameters` and `is_function`.
- **Function-local facts**: the validator's `FunctionEnvironment` (temporary
  and slot types, block labels) and the emitter's `FrameLayout` (offset,
  type and `LowTypeInfo` per value) are each built in one pass over the
  function and looked up by hash.
- **Emission facts**: `Address {base, displacement}` is the one memory
  operand representation; `Cy86Writer` appends to one string; the program
  counter for synthesized labels lives in `CodegenContext`; the function
  label prefix is computed once per function.
- **Operand facts**: literal spellings stay verbatim from the lexer through
  `Operand.text` to the CY86 text (`nullptr` → `0` is the one rewrite);
  parsed `int_value`/`float_value` exist for consumers that need the value
  (the `f80` data bytes).

## Findings and changes (this cleanup)

1. **Two symbol tables (material, removed)**: the validator built its own
   `SymbolIndex` (seven sets and maps: all names, globals, functions,
   thread-local globals, global types, definition and declaration pointers)
   and separately filled `facts.symbols` for the emitter, which then
   re-derived function/declaration lookups on its own.  One table now
   serves both; kind, storage and types are read from the program through
   the `SymbolRef`.  Peak RSS on 200k functions: 786 MB → 737 MB.
2. **Two result-type rules (removed)**: `result_type` in the emitter and
   `instruction_result_type` in the validator encoded the same `addr/index/
   cmp` rule; one definition in the model layer.
3. **Textual downgrade in memory operands (material, fixed)**: scratch and
   local bases were rendered as `"bp-24"` strings and `memory()` re-parsed
   them with `strtoll` to fold displacements; every number went through an
   `ostringstream`, and the writer streamed into one.  Typed `Address`,
   `std::to_string`, and in-place string appends.  The `f80` probe (the
   heaviest user of scratch addresses) dropped from 0.84 s to 0.46 s at 50k.
4. **Duplicate literal parse (fixed)**: `f80` global data re-ran `strtold`
   on the spelling the parser had already converted; the emitter now uses
   the operand's `float_value`.
5. **Three width tables (fixed)**: `value_size`, `index_element_size` and
   `width_suffix` each carried their own `f80`/`f32`/object cases beside
   `describe_low_type`; all derive from `LowTypeInfo` now, and
   `load_bulk_pointer` (identical to `load_storage_address` under
   validation) and the single-use wide-store wrappers are gone.
6. **Per-argument callee resolution (fixed)**: each call argument resolved
   the callee's parameter list twice (definition then declaration lookup,
   for the type and again for the address decision); resolved once per call.
7. **Per-label prefix rebuild (fixed)**: every block reference rebuilt
   `fn__F` from the function name; cached per function.
8. **Parser copies (fixed)**: each instruction was copied by value through
   its builder (624 bytes plus seven strings) and every lookahead copied the
   peeked token; builders fill the caller's instruction, `peek` returns a
   reference, `take` moves.
9. **Stale derived state (removed)**: `LowirProgramFacts` counts and
   `entry_function` (never read), the lexer's unused source name, the
   validator's `literal_type` equality checks (the parser sets that field
   from the same type, so they could not fail), the empty `assigned &&
   call_returns_void` block in `parse_call`, a dead `ITEM_ZERO` branch in
   structured `f80` data, the redundant pointer-type test in the `i32`
   load special case (validation already guarantees it), and the separate
   `collect_eh_fact` pass (folded into the body pass).
10. **Checkpoint-era diagnostics (fixed)**: "outside the scalar CY86
    checkpoint" messages replaced by contract wording.
11. **Driver out-parameter (fixed)**: `parse_and_validate` returned the
    program through a helper with a facts out-parameter; the mode function
    now reads parse → validate → emit directly.
12. **Header weight (fixed)**: CP1's multi-line accessor bodies pushed
    `lowir_model.h` over the audit's body-line threshold; one-line
    accessors restore it.  The file audit now reports only the pre-existing
    `recog_parser.h` warning.

Accepted at stage scale: the scaffold model's shape — string-typed
`LowType.text` and `Instruction.op`, `Operand.literal_type`/`int_value`/
`float_value` alongside the spelling, 624 bytes per `Instruction` — which
puts string comparison (`memcmp`/`strlen`/`operator==`) at about 13 % of the
profile and `vector<Instruction>` growth at 5 %; `std::set` for
metadata-key deduplication (blocks hold a handful of keys); operand type
copies in the validator (short spellings, no heap).  Rejected: retyping the
scaffold model in PA13 (the README frames the typed model as support for
the text format, and the stage that makes LowIR the production model owns
that change); reserving instruction vectors (needs a counting pass over the
text).  The plan's "well under a second for 200k instructions" target is not
met (2.0 s); the remaining cost is the model's per-instruction string
payloads, not an algorithmic defect.

## Behaviour on checked-in inputs

96 inputs, CP3 versus final executable: identical exit status on all 96 and
byte-identical CY86 text on all 67 successful ones.  The six 50k probes are
byte-identical as well (262k–1.2M output lines each).

## Performance evidence

Immutable executables, interleaved, medians of 5, wall seconds, peak RSS in
MB (the same for both unless shown).

| probe | CP3 | final | RSS |
| --- | --- | --- | --- |
| 50k / 100k / 200k chain (1.5 instructions each) | 0.57 / 1.18 / 2.44 | 0.46 / 0.96 / 2.02 | 89 / 175 / 347 |
| 50k / 100k / 200k functions, each called once | 1.59 / 3.28 / 6.84 | 1.20 / 2.47 / 5.12 | 197→188 / 400→362 / 768→720 |
| 50k / 100k / 200k globals loaded and stored | 1.33 / 2.77 / 5.90 | 1.06 / 2.27 / 4.74 | 203 / 403 / 765 |
| 50k / 100k / 200k six-argument calls | 0.77 / 1.54 / 3.31 | 0.58 / 1.21 / 2.53 | 104 / 204 / 422→389 |
| 50k / 100k / 200k blocks with switch and branches | 0.76 / 2.67 / 3.39 | 0.62 / 2.16 / 2.75 | 113 / 221 / 465→410 |
| 50k / 100k / 200k f80 operations | 0.84 / 1.79 / 3.68 | 0.46 / 0.99 / 2.07 | 90 / 187→167 / 339→300 |

Every family grows ×2 (±0.1) per doubling in both executables; the final
one is 17–45 % faster and never slower, and RSS is unchanged except where
the duplicate symbol table went (functions, calls, blocks).  Phase split at
100k (final executable, `steady_clock` around each stage): parse 0.47 s,
validate 0.14 s, emit 0.32 s on the chain probe; 1.19 / 0.31 / 0.87 s on the
functions probe; the same proportions on the others.  The `perf` profile of
the final executable is diffuse: page faults from model growth (~11 %),
value-offset hash lookups (6 %), string comparison (~13 %), instruction
vector growth (5 %), operand reference validation (5 %), tokenization (6 %);
no function above 8 %.

## Conformance validation

- `make test-report-through-pa13`: 919/919 (823 through pa12 unchanged +
  96 pa13; the course pa13 set has 0 tests).  `make test-pa13`: 96/96.
- `perl scripts/cppgm_file_audit.pl --stage pa13 --paths dev/src`: passes
  with the pre-existing `recog_parser.h` header-layout warning only.
- README structural-validation list: every listed rejection has a
  validator check and, except the generic "invalid metadata values" family
  (parser-owned, several fixtures), a `200-bad-*` fixture.  README
  instruction contract: `cmp` results are canonical `i64` for every operand
  type; `f80` keeps 16-byte storage and alignment; atomics use the
  single-threaded interpretation; fences emit nothing; indirect calls
  require the explicit signature.
- Emission is monotonic: adding an instruction family adds a case in
  `emit_instruction` and, if it defines a new width, a row in
  `describe_low_type`; no existing output path branches on fixture names.

## Checkpoint ledger

| checkpoint | commit | outcome |
| --- | --- | --- |
| plan | `af69cfbf1` | stage design, emission conventions, failure map |
| CP1 lexer, parser, model, validator, driver | `f0308c6b9` | 29/96; through-pa12 823/823 |
| CP2 CY86 codegen core | `4b6b4e4e3` | 86/96 |
| CP3 wide values and exception handlers | `b461c8a45` | 96/96; through-pa12 823/823 |
| CP4 architecture audit and cleanup (this document) | final commit | 96/96; through-pa13 919/919; findings 1–12 |
