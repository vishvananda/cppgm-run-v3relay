# PA14 Final Architecture Audit — abimangle

## Scope and method

Traced representative ABI facts from their first typed owner to the emitted
name and accounted for every classification, key, registration, cache, and
render on the way:

- **A builtin word** (`int`, `uint128`): classified once by the reader with
  `lookup_builtin_type` into `AbiType::builtin`; the encoder indexes the same
  table row for the Itanium code and for the width and signedness that decide
  how a negative value fact is spelled; the serializer renders the row's word.
  No unit compares the spelling again.
- **A class used three ways** (`ns::C` as a parameter type, as the owner of a
  `member` type, and as the prefix of `ns::C::f`): all three routes intern the
  same component key (`N:ns::C`), so the first emission registers one
  substitution slot and the others substitute it (`_Z1fN2ns1C6NestedENS0_5OtherE`).
- **A template argument named in a compact path** (`function path
  std::getline Char_arg`): the reader completes the case, resolves the bare
  operand against the case's argument ids, and publishes a
  `ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT`; the encoder lowers the path into name
  pieces, registers the template-name key before the argument list, and emits
  the return type because the function is a template.
- **A dependent expression under `decltype`**: each `let-expr` is keyed once
  by its kind, operator, literal, and the ids of its children (cached by the
  fact's address with an in-progress state), the `decltype` type interns
  `DT(<expression id>)`, and equal structure from separate definitions shares
  a slot (`_Z3useDTT_ES_`).
- **A closure call operator in a local context**: the context definition is
  encoded with the enclosing name's substitution table into `Z…E`, the
  closure is keyed exactly as the `lambda-closure` type fact and registered
  as the `<prefix>` it is, then the terminal, tags, template arguments and
  parameters follow.
- **A covariant thunk**: `this`, fixed result, and virtual result adjustments
  stay typed in `AbiTargetRecord` and are spelled by one call-offset rule
  (`_ZTch8_v16_n32_…`).
- **An operator terminal**: the reader maps the word to `AbiOperatorKind`; the
  emitter picks the unary or binary code for shape-dependent operators from
  the parameter count and whether the function has an owner, as the README
  specifies.

Compared the pre-cleanup executable (built from `563625135`) and the final
one on all 111 fixtures, on the README-derived probes below, and on generated
scaling families, with interleaved runs and medians of five; profiled both
with `perf`.

## Ownership (final)

| fact | owner | consumers |
| --- | --- | --- |
| fixed vocabularies (builtins, ctor/dtor kinds, operators) | one table per enum in `abi_mangle_types.cpp`, declared in `abi_mangle.h` | reader (word → enum), serializer (enum → word), encoder (enum → code, width) |
| fact text → typed case | `FactCaseReader` in `abi_fact_parse.cpp`, one case in memory at a time | `parse_fact_text`, `mangle_fact_files` |
| definition lookup | `AbiDefinitionTable`, a non-owning id → typed fact index over the case's per-kind vectors | `Mangler` accessors |
| component identity | `KeyInterner`: one id per distinct structural spelling whose children are ids | `SubstitutionTable` (dense slot vector indexed by id), key caches |
| structural keys | `key_of_node` (cached per fact address, in-progress state), `key_of_argument`, `key_of_expression`, `key_of_entity`, `name_key` | every candidate lookup and registration |
| qualified-name prefixes | `append_prefixes` (longest registered prefix, then emit and register) | named types, template names, variables, internal names, path-form and encoding-form functions, namespace closures |
| function encodings | lowering of every target form into `FunctionFacts`; `mangle_function_name` / `mangle_context_function_name`; `mangle_function_facts` for results, parameters, variadic | targets, thunks, contexts, function entities |
| recursion bound | `ABI_MAXIMUM_NESTING_DEPTH` (2048), `DepthScope` on every recursive encoder path, `require_depth` in the reader | reader, encoder |

## Findings and changes

1. **Two key spaces for one ABI component (material, fixed).** Named types
   were keyed `name:Q` and function prefixes `Q`, with a fallback lookup in
   one direction only, and member-type owners were emitted through the
   qualified-name path without ever being looked up or registered as a
   `<prefix>`.  Unfixtured but README-described inputs produced wrong names:
   `f(ns::C::Nested, ns::C::Other)` gave `…NS_1C5OtherE` instead of
   `…NS0_5OtherE`, and `f(ns::Outer<int>, ns::Outer<int>::Nested)` gave
   `…NS0_IiE6NestedE` instead of `…NS1_6NestedE`.  One interned key per
   component now serves types, prefixes, template names and closures.
2. **Key identity conflated with substitution candidacy (fixed).**
   Non-substitutable template parameters had an empty key, so
   `f(T0*, T1*)` collapsed to `_Z1fPT_S_` (correct: `_Z1fPT_PT0_`);
   `std-template` keys ignored their arguments, so `Sa<int>` and `Sa<char>`
   shared a slot.  Every node has an identity; candidacy is decided at
   emission.
3. **Typed thunk fact ignored (fixed).** The virtual covariant form always
   spelled `v0_`, dropping the fixed result adjustment the README defines.
4. **Crashes and quadratic keys on deep or cyclic facts (fixed).** A cyclic
   `let-expr` or `let-context` overflowed the stack; the 65536 depth guard
   could not be reached on an 8 MB stack; expression keys embedded their
   children's full keys (15 GB RSS for a 20k-deep chain); a `>256` pointer
   chain took a special path that skipped registering the intermediate
   types.  Keys now reference children by id (bounded by fan-out), every
   recursive path is bounded by one measured constant, key caches carry an
   in-progress state, and alias chains are resolved by a loop bounded by the
   definition count.
5. **Quadratic inline nesting (fixed).** The reader copied each subtree into
   its wrapper and the encoder recomputed uncached subtree keys at every
   level: 1.9 s at depth 5000, now 0.04 s at depth 4000.
6. **Fixed vocabularies classified below the owning layer (fixed).** The
   reader kept builtin, operator and constructor words as strings and the
   encoder re-classified them by string compare (two builtin tables, 40
   operator compares per terminal, target terminal words classified by
   prefix); README names `deref`, `remainder`, `index`, `member-pointer`,
   shifts, compound bit assignments and `constructor-allocating` were
   rejected and the unary/binary shape rule was unimplemented.  One enum and
   one table per vocabulary; `AbiTerminal` carries the typed terminal in both
   records and compact targets.
7. **Two fact readers (fixed).** `mangle_fact_files` had its own line and
   case loop beside `parse_fact_text`; both now use `FactCaseReader`, which
   streams one case at a time.
8. **Three function-name emitters (fixed).** Path, encoding and context forms
   each carried their own prefix search, qualifier codes and template-piece
   emission, and disagreed on details (the path form registered a template
   prefix after its arguments, the encoding form never looked prefixes up).
   Every form lowers into `FunctionFacts` and one emitter encodes it.
   Rendered text is no longer re-parsed: contexts and thunks used to strip
   `_Z` with `substr`, and member owners stripped `N…E` off a rendered
   type; emitters now return bodies and callers wrap them.
9. **Path operand classification (moved to the reader).** The encoder decided
   whether a bare operand was a template argument by probing the definition
   table at emission and rejected anything else; the reader resolves it when
   the case completes, and a bare word that names no definition is a class
   name, as the README's type-position rule says.
10. **Fat per-line record (fixed).** Every input line built a 4960-byte
    `AbiFactRecord` holding all alternatives, definitions were copied into an
    owning map, function records copied again, and each line went through an
    `istringstream`.  The case now stores definitions per kind, the table
    indexes them without owning, records are moved, and words are split in
    place.  Twenty thousand cases: 4.13 s → 1.25 s.
11. **Stale and duplicate state (removed).** `AbiType::substitution`,
    `lvalue_ref`/`rvalue_ref` and `tagged` (all derivable), expression
    `value_type`/`address_of` (never set), `has_value_type`, the
    `SubstitutionTable` key vector (stored every key twice), the duplicate
    `argument_refs`+`substitution` on template-argument records, the
    never-produced `ABI_TYPE_CV` kind, the declared-but-undefined
    `mangle_target(target, definitions)` that was meant to be PA15's entry
    point, and helper copies across units (`source_name`, signed numbers,
    scope stripping, tag canonicalization, nested-name tests).
12. **Small contract gaps (fixed).** A `-` lambda discriminator rendered
    `E-_`; unsigned 128-bit modulo used 64-bit arithmetic; the serializer
    emitted compact `ptr:`/`array:` around structured children (unparseable)
    and silently merged non-first unlabeled cases (now multiword `ptr T` /
    `array N T`, accepted by the reader, and a rejection).

Accepted at stage scale, with measurements: the scaffold node shape
(`AbiType` is 320 bytes with eight owning strings and four vectors, an
argument definition 1232 bytes), which leaves string compare, `strlen` and
allocation at about 35 % of a 20k-case batch profile (62 µs per case); the
fact file's definition ids, which are the text boundary's binders and are
resolved by one map lookup per occurrence; reading a whole input file before
streaming its cases (peak RSS is the file size plus one case).  The file
audit's header-weight warning on `abi_mangle.h` is a declaration-count
heuristic (242 declaration lines, 27 of them the vocabularies); it predates
this cleanup and no implementation body remains in the header.

PA15 handoff: the compiler constructs `AbiTargetRecord`, `AbiFunctionRecord`
and the typed facts an `AbiDefinitionTable` indexes, then calls
`mangle_target(target, function_records, definitions)`; the reader unit is
not linked.  Template arguments, expressions and contexts are still reached
through string ids because that is the fact vocabulary; the lowering stage
that owns semantic identities should decide whether to keep binder ids or
embed facts inline.

## Behaviour on checked-in and probe inputs

All 111 fixtures: identical exit status and byte-identical names between the
pre-cleanup and final executables.  The serializer round trip
(parse → serialize → parse → serialize, then mangle both) is stable and
name-identical on all 123 cases.  README-derived probes not pinned by
fixtures, final executable:

| facts | name |
| --- | --- |
| `function path f member named:ns::C Nested member named:ns::C Other` | `_Z1fN2ns1C6NestedENS0_5OtherE` |
| `function path f named:C member named:C Nested` | `_Z1f1CNS_6NestedE` |
| `S = template ns::Outer <int>`; `function path f S member S Nested` | `_Z1fN2ns5OuterIiEENS1_6NestedE` |
| `function path ns::Outer::f S` | `_ZN2ns5Outer1fENS0_IiEE` |
| `function f ptr:T0 ptr:T1` (non-substitutable params) | `_Z1fPT_PT0_` |
| `Sa<int>`, `Sa<char>`, both again | `_Z1fSaIiESaIcES_S0_` |
| `L = local-type F L 0`; `function path g member L M` | `_Z1gZ1fvEN1L1ME` |
| `thunk 8 virtual-result 16 -32 function path ::C::f` | `_ZTch8_v16_n32_N1C1fEv` |
| `C::operator` + `minus`, no params / namespace `operator minus` 1 param / 2 params | `_ZN1CngEv` / `_Zng1C` / `_Zmi1CS_` |
| `deref`, `index`, `shift-left`, `constructor-allocating` | `de`, `ix`, `ls`, `C3` |
| `lambda-closure make -` / `function lambda make - operator-call int` | `ZN2ns4makeEvEUlvE_` / `_ZZN2ns4makeEvENUliE_clEv` |
| cyclic `let-expr`, `let-context`, `let-type`, `let-arg` | exit failure with a diagnostic (previously two segfaults) |

## Performance evidence

Immutable executables, interleaved, medians of five, wall seconds and peak
RSS in MB.  `wide N` is N cases of the `300-std-vector-string-substitution`
shape; the chains are one case with N nested definitions.

| probe | before | after | RSS before → after |
| --- | --- | --- | --- |
| wide 10k / 20k / 40k | 2.16 / 4.13 / 8.08 | 0.63 / 1.25 / 2.49 | 5 / 7 / 10 → 13 / 23 / 43 (file held in memory) |
| 20k-parameter substitution reuse | 0.13 | 0.04 | 36 → 16 |
| 1000-deep pointer / expression / function-type / template / inline chain | 0.02 / 0.07 / 0.06 / 0.14 / 0.08 | 0.01 each | 12 / 49 / 36 / 75 / 6 → 4–7 |
| 5000-deep expression chain | 1.38 s, 1.0 GB | rejected at the bound | — |
| 20000-deep expression chain | 33 s, 15.4 GB | rejected at the bound | — |
| 111-fixture suite (`make test`, process spawn dominated) | 0.28 | 0.28 | — |

Scaling is linear in both cases and definitions after the change; before it,
memory was quadratic in expression depth and time quadratic in inline
nesting.  Stack margin at the bound (`ABI_MAXIMUM_NESTING_DEPTH` = 2048):
pointer, cv, inline, expression, function-type, member, template, context
and entity chains at 2000 and 2100 levels all either complete or fail with
"nests too deeply" on a 2 MB stack; the first overflow appears at 1.5 MB for
the inline and context families, so the default 8 MB stack has a 4× margin.
Profile of the final executable on `wide 20k`: `memcmp` 9 %, `strlen` 8 %,
`malloc`/`free` 14 %, `AbiType` move and destroy 8 %, case reading 9 %,
interning and key building under 4 %; before it, `AbiType` moves and string
moves alone were 27 %.

## Conformance validation

- `make test-pa14`: 111/111.  `make test-report-through-pa14`: 1030/1030
  (919 through pa13 unchanged plus 111 pa14).
- `perl scripts/cppgm_file_audit.pl --stage pa14 --paths dev/src`: passes
  with the pre-existing `recog_parser.h` warning and the header-weight
  heuristic on `abi_mangle.h` discussed above; the duplication warning
  between the two encoder units is gone.
- README contract: every listed operator name and both ambiguous-shape rules
  are implemented; the thunk forms spell all three call-offset shapes;
  unsigned modulo values honour the target width up to 128 bits; adjacent cv
  words are one canonical node; bare words in type and operand positions
  resolve definition → builtin → compact → class; definitions are
  case-scoped and order-free; duplicate ids and negative indices are reader
  rejections; ABI tags attach to the unqualified component of named and
  template types and flow into special names.
- Fixture-pinned quirks kept deliberately: `Tn` before dependent value
  arguments, `member-template-entity` owners not registered, entity operands
  encoded with a fresh substitution table, lambda discriminators emitted
  verbatim, non-substitutable `template-param` never registered.

## Checkpoint ledger

| checkpoint | commit | outcome |
| --- | --- | --- |
| plan | `a078a5fa0` | stage design, emission conventions, failure map |
| CP1 reader, model, substitution table, types and names | `dcc0dd59d` | 37/111; through-pa13 919/919 |
| CP2 function targets and thunks | `bd383931f` | 85/111 |
| CP3 local contexts and lambdas | `ddc3a8e70` | 96/111 |
| CP4 dependent expressions and decltypes | `563625135` | 111/111; through-pa13 919/919 |
| CP5 architecture audit and cleanup (this document) | final commit | 111/111; through-pa14 1030/1030; findings 1–12 |
