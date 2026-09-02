# PA14 Plan — abimangle (normalized ABI facts → Itanium mangled names)

Grading: `abimangle -o x.my x.t`; exit status must match `x.ref.exit_status`
(2 fixtures expect EXIT_FAILURE, 109 expect EXIT_SUCCESS plus a byte-exact
match of one name per case, newline-terminated).  There is no reference
binary: the 111 `.ref` files under `pa14/tests/abi` are the only oracle, so
every emission convention below is fixture-pinned.  `cppgm.tests/course/pa14`
is empty.  The harness may run many inputs in one process, so the tool keeps
no mutable static state.  `doc/itanium-mangling.txt` is the grammar
reference.  The final architecture review, findings, performance evidence and
validation are in [audit.md](audit.md).

## Stage design

Data flow: `dev/abimangle.cpp` (scaffold envelope: `-o`, help, exceptions →
EXIT_FAILURE) → `mangle_fact_files` reads each file and streams one case at a
time through `FactCaseReader` → typed `AbiFactCase` (per-kind definition
vectors, one target, function records) → `AbiDefinitionTable` (non-owning
id → fact index) → `Mangler` encodes the target → names joined with `\n`.
Every fact word is classified once in the reader; the encoder consumes only
the typed model and never re-parses rendered text.

Modules (`FRONTEND_OBJ_BASENAMES_abimangle`; PA15 links the two encoder
units and calls `mangle_target(target, function_records, definitions)`):

- `dev/src/abi_mangle.h`: the typed model (extended scaffold), the fixed
  vocabularies (`AbiBuiltinType`, `AbiSpecialFunctionKind`,
  `AbiOperatorKind`, `AbiTerminal`), `AbiDefinitionTable`, the nesting bound
  `ABI_MAXIMUM_NESTING_DEPTH`, and the reader/serializer/encoder API.
- `dev/src/abi_fact_parse.cpp`: line-oriented reader (compact `a:b:c` and
  multiword type grammar, definition, target and function-record forms, case
  completion that checks ids, requires one target and resolves bare path
  operands against the case's argument ids), the canonical serializer, and
  `mangle_fact_files`.
- `dev/src/abi_mangle_encoder.h` (internal): `KeyInterner` (one id per
  structural spelling whose children are ids), `SubstitutionTable` (dense
  slot vector indexed by key id, spelled `S_`, `S0_`, … base 36),
  `FunctionFacts`/`NamePiece` (the lowered function shape), `Mangler`, and
  `DepthScope`.
- `dev/src/abi_mangle_encode.cpp`: vocabulary-free name grammar: prefixes
  (longest registered prefix substituted, the rest emitted and registered,
  `std` spelled `St` and never registered), qualified and internal names,
  lowering of path/encoding/local/lambda/namespace-lambda targets into
  `FunctionFacts`, one function-name emitter plus the local-context emitter,
  terminals (unary/binary operator shape from parameter count and owner),
  contexts (`Z…E` with the shared table), entities (fresh table), special
  names and thunks, `AbiDefinitionTable`, `mangle_target`,
  `mangle_fact_case`.
- `dev/src/abi_mangle_types.cpp`: vocabulary tables, alias resolution
  (`resolve_type`: iterative, cv accumulated, bounded by the definition
  count), structural keys for types/arguments/expressions/entities (cached
  per fact address with an in-progress state), `<type>` emission with lookup
  before and registration after each candidate, member owners as
  `<prefix>` candidates, template arguments, integral values (unsigned
  negatives modulo the target width up to 128 bits), and expressions.

Substitution rules (all fixture-pinned): keys are the represented structure,
never the emitted text; a class, template name, local class or closure is one
component whether it appears as a type, a prefix, a member owner or a call
operator's owner; builtins and non-substitutable `template-param` are never
candidates but keep distinct identity inside composites; a function's own
template-id registers its template name before the arguments and never the
specialization; class-template prefixes register both; `member-template-
entity` owners are not registered; entity operands (`L_Z…E`) use a fresh
table; `Tn` precedes dependent value arguments; lambda discriminators are
emitted verbatim (`-` for the first closure), local-class discriminators as
`_<d-1>` for `d ≥ 1`.

Bounds: recursion in the reader and the encoder is limited to
`ABI_MAXIMUM_NESTING_DEPTH` (2048) units, measured to use under 2 MB of stack
on the worst family (4× margin under the 8 MB default); key spellings are
bounded by fan-out, so key work is linear in the facts; one case is resident
at a time.

## Checkpoint ledger

| checkpoint | commit | outcome |
| --- | --- | --- |
| plan | `a078a5fa0` | stage design, emission conventions, failure map |
| CP1 reader, model, substitution table, types and names | `dcc0dd59d` | 37/111; through-pa13 919/919 |
| CP2 function targets and thunks | `bd383931f` | 85/111 |
| CP3 local contexts and lambdas | `ddc3a8e70` | 96/111 |
| CP4 dependent expressions and decltypes | `563625135` | 111/111; through-pa13 919/919 |
| CP5 architecture audit and cleanup | final commit | 111/111; through-pa14 1030/1030; see audit.md |
