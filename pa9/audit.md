# PA9 Final Architecture Audit — cy86

## Scope and method

Traced representative facts through their full ownership paths: srcfile →
PA5 `PreprocEngine::RunSingleFile` → `Cy86TokenCollector` (PA2 bytes
preserved; UDLs, keywords, and invalid tokens rejected at collection) →
`Cy86Parser` (statements, operands, label table) → `BuildCy86Layout`
(two-pass sizing with placeholder labels) → `Cy86ToX86Translator` (fixed
per-opcode sequences) → `X86Instruction::Encode` → single-PT_LOAD ELF via
`BuildProgramImage` → outfile + chmod in the `dev/cy86.cpp` envelope.
Ran 45 directed differential probes against `cy86-ref` (impl exit status
compared always; program stdout bytes + exit status compared when both
compile), using runtime label-difference programs wherever absolute
addresses would differ by code expansion. Probes covered label
arithmetic, byte-mechanical width conversion, negation of every literal
kind, odd-typed immediates in every operand category, syscall operand
shapes, NaN comparisons, data operands, and literal-statement sizes.
Measured compile scaling at 25k/50k/100k four-statement lines and timed
the three heavy graded programs.

## Ownership trace (clean)

- **Token facts**: the collector owns literal identity — type, raw PA2
  bytes, element count. Long double literals arrive as 16-byte objects
  whose 80-bit payload puts the sign bit in byte 9; the reference also
  emits 16 bytes for a long-double literal statement (probe s14), so the
  bytes pass through unmodified.
- **Syntax facts**: the parser owns the grammar, the label table
  (reserved-spelling clashes, duplicates, forward references, undefined
  references), and operand structure. Post-parse validation owns only
  the structural rules the reference enforces: operand count, `w` not
  immediate (s10), `I` immediate-only, register width exactly equal to
  operand width (no 80-bit registers), 64-bit memory address registers,
  negation only of arithmetic scalars (r9), and no immediates in plain
  width-80 operands (q6/q7). Everything else is deliberately
  unconstrained — the reference is byte-mechanical about types.
- **Value facts**: one byte-level authority, `ConvertLiteralBytes`,
  implements the immediate width conversion for every literal kind:
  negation first in the literal's own width (two's complement for
  integrals, sign-bit flip for floats — byte 9 for long double), then
  truncate-keep-low-bytes or extend (sign-fill only for signed integral
  scalars, zero-fill for floats, arrays, and unsigned). `ConvertLiteral`
  packs those bytes into a u64 for register-width contexts;
  `ImmediateBytes` produces arbitrary byte counts (x87 loads, data
  operands) and zero-extends 64-bit label values. Labels resolve
  through one map; `(label ± literal)` extends the addend to 64 bits by
  its own type and then adds or subtracts (probe q2 pinned
  extend-then-subtract over negate-then-extend).
- **Layout facts**: two passes, sizes label-independent by
  construction — 64-bit label-dependent immediates always emit the
  fixed 10-byte `mov r64, imm64`; all other immediates encode from
  values identical in both passes. Label-based memory bases keep the
  value-compact form: both passes stay below 2^32 while images stay
  under 4 GB, unreachable at any buildable scale. Data alignment:
  element size for arrays, fundamental size for scalars, 16 for long
  double, N/8 for dataN. The pass-2 size-equality assertion is
  retained as a cross-check.
- **Emission facts**: the translator owns register discipline — operand
  values in rax/rbx/rcx (shift count in cl), rdx reserved for
  widening/mul/div, addresses in rsi/rdi, r12–r15/rsp/rbp never
  scratch; red zone slots: bounce buffers at [rsp-16/-32/-48], syscall
  staging at [rsp-64..-120]. The assembler owns encodings (REX, ModRM,
  SIB, prefix rules) and chooses compact immediate forms only from
  pass-stable values.
- **Envelope**: `dev/cy86.cpp` owns argv contract, per-TU preproc, token
  concatenation (per-TU EOF dropped), binary write, chmod. The image
  writer in `cy86_codegen.cpp` is the only ELF authority — the driver's
  duplicate header structs were dead and are removed.

## Findings and changes (this cleanup)

All findings were surfaced by differential probes (p/q/r/s numbers)
against `cy86-ref`; every probe now matches in impl exit status and,
where both compile, in program stdout and exit status.

1. **Two-pass size divergence on label immediates (material, fixed)**:
   `EncodeMov` picks value-dependent compact encodings for 64-bit
   immediates, and layout pass 1 uses placeholder labels of 0, so
   `move64 x64 (lbl - 8)` (q1) and `(lbl + 4294967040u)` (q3) sized
   differently across passes and the compiler rejected valid programs
   with "pass sizes diverged". Label-dependent 64-bit immediates now
   always use `X86ImmFullWidth` (the plan's original design contract).
2. **Syscall scratch clobber (material, fixed)**: memory-operand
   parameters loaded through rsi (address scratch) after the rsi
   parameter slot was filled, and memory offsets clobbered rbx holding
   the syscall number — `syscall3 x64 1 [fd] (msg) 6` wrote from the
   wrong buffer (r1) and `[fd + 0]` invoked a garbage syscall (r2).
   All values now stage into red-zone slots first; parameter registers
   fill afterwards with plain loads.
3. **Immediate typing was restrictive; the reference is byte-mechanical
   (material, fixed)**: the reference accepts floats, arrays, and labels
   in every ≤64-bit operand context and converts bytes at the operand
   width — float bits in integer moves/arithmetic (q8, s7, s9), long
   char arrays truncated to width (q10), wide-char arrays (q11), floats
   in dataN (q9), labels and label+addend in dataN (jump tables — s11,
   s12), float literals as memory bases/offsets and label addends
   (r11/r12/r13, s2), and sign-extension of signed integrals into float
   contexts (s13). The `data` statement path now stores its operand and
   converts at emission through the same authority.
4. **Float negation (material, fixed)**: negated float immediates were
   silently un-negated in x87 loads (p4, p6c) and float literal
   statements were negated by two's complement (p5: `-1.5;` emitted
   -3.0's bytes). Negation now flips the sign bit in the literal's own
   encoding — byte 9 for the 16-byte long double object (p6b).
5. **Float operand shapes (material, fixed)**: the reference loads
   f-category immediates by converting bytes to the *operand* width and
   FLDing that width (denormal round-trips s3b/s4/s5 pinned this over
   own-literal-width loading), accepts registers as float sources
   (bit-pattern bounce, s1/r5) and destinations (FSTP to bounce, s6),
   and rejects immediates in plain width-80 operands (move80, q6/q7).
   All three now match.
6. **NaN comparisons (material, fixed)**: FCOMIP + sete/setb treated
   unordered as equal/less (q12, r14). Now: fgt/fge test op1 vs op2
   with the above-conditions (false on unordered); flt/fle swap the
   load order and use above-conditions; feq = ZF∧¬PF, fne = ¬ZF∨PF via
   a second SETcc on the parity flag. Matches IEEE and the reference on
   all six comparisons (r14).
7. **Structure cleanups**: removed the driver's dead duplicate
   ELF-header structs, the assembler's unreachable `mov r64, imm`
   branch, the tautological `IsAddressOperand`, the duplicated
   `IsLiteralArray` (now the shared `Cy86IsLiteralArray`), the stale
   "integer checkpoint" error message, and the double opcode lookup per
   statement; `Cy86FindOpcode` is a map lookup.

## Performance evidence

Release build, `/usr/bin/time`:

- Graded heavy programs (10 s cap), outputs byte-identical to the
  checked-in reference fixtures: 300-binary-calculator (16 MB stdin)
  0.50 s / 24 MB RSS; 500-to-float80 (12 MB stdin) 0.44 s / 35 MB;
  600-float-calculator (35 MB stdin) 2.01 s / 41 MB.
- Compile scaling (30 s cap; graded inputs are ≤ 323 lines): generated
  label-heavy programs of 25k/50k/100k four-statement lines
  (≈100k/200k/400k statements with data64 label immediates, label
  arithmetic, and 64-bit umod chains) compile in 1.77 s / 3.56 s /
  7.18 s — 2.0× per doubling, linear. RSS ≈ 3.4 KB per statement
  (retained tokens + statements + two translate passes), the same
  order as the pa6–pa8 envelope and accepted at stage scale.
- A 100k-line comment-only file preprocesses in 0.25 s: compile cost is
  dominated by dense tokenization plus the two translate passes with
  per-instruction encode allocations — constant per statement, no
  superlinear term.

## Conformance validation

- `make test-report-through-pa9`: 432/432 (414 through-pa8 unchanged +
  11 pa9 local + 7 pa9 course); pa9 file audit passes (one pre-existing
  non-fatal header-division warning on `recog_parser.h`).
- 45 differential probes match `cy86-ref`: label arithmetic including
  unsigned subtraction (q2 pinned extend-then-subtract), all negation
  forms, byte-mechanical conversions across every operand category,
  move80 shapes, syscall parameter shapes, NaN semantics on all six
  float comparisons, data-operand labels, and literal statement sizes
  (float 4 / long double 16, s15/s14).
- **Divergence envelope (recorded, deliberate)**:
  - *Absolute label values* differ from the reference (code expansions
    differ); grading compares only program output, and probes use
    runtime label differences.
  - *Label-based memory bases* use the value-compact `mov` form; sizes
    could only diverge across layout passes if an image exceeded 4 GB,
    which cannot build within any realistic constraint.
  - *x87 environment*: default rounding/precision assumed, as the
    reference does; conversions are graded only on exactly-representable
    values per the spec's UB clause.

## Checkpoint ledger

| id  | scope | proof | status |
|-----|-------|-------|--------|
| CP1 | integer core end-to-end: full parse/sema, layout, ELF, stub/epilogue, assembler, data/move/jump/call/ret/logic/shift/add/sub/compare/syscall | pa9 14/18 from 0/18; through-pa8 414/414; audit pass | DONE |
| CP2 | multiply/divide/modulus with widening idioms, character-array and compact literal immediates | pa9 15/18; 400-integer-calculator byte-identical; calculator probe 0.479 s | DONE |
| CP3 | x87 float80: conversions, arithmetic, comparisons, move80, red-zone bounces, unsigned split/offset idioms | pa9 18/18; through-pa8 414/414; calculator probe 0.333 s | DONE |
| CP4 | final audit: two-pass label-immediate stability, syscall staging, byte-mechanical immediate model, float negation, float operand shapes, NaN comparisons, structure cleanups | 432/432; 45 probes match ref; 2.0× linear compile doubling; heavy programs ≤ 2.01 s of 10 s cap | DONE |
