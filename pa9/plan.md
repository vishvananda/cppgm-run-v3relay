# PA9 Plan — cy86 (CY86 mock IL → x86-64 ELF executable) [COMPLETE]

Final state: 432/432 fixtures pass through `make test-report-through-pa9`
(414 through-pa8 + 11 pa9 local + 7 pa9 course); pa9 file audit passes.
The architecture review, findings, the 45-probe differential matrix,
performance evidence, and the divergence envelope are consolidated in
`pa9/audit.md`.

`cy86 -o <out> <src1..srcN>` runs the PA5 preprocessor+tokenizer per TU,
concatenates the token sequences in argv order, parses the regular CY86
grammar (pa9/pa9.gram), checks operands against pa9/cy86-opcode.desc,
translates each CY86 instruction to a fixed x86-64 sequence, and writes a
single-PT_LOAD ELF executable (chmod 0755). Grading: impl exit status
must match ref (0 well-formed, 1 ill-formed); when 0, the generated
program runs with the test's `.stdin` and its stdout + exit status are
compared to text fixtures. Timeouts: build 30 s, program run 10 s.

## Stage Design (as built)

Owning boundaries (PA5 pipeline reused as-is; three new source pairs):

- `dev/src/cy86_parse.h/.cpp` — token acquisition + parse + structural
  sema. `Cy86TokenCollector : IPostTokenOutputStream` preserves raw PA2
  literal bytes (long double = 16-byte object, sign in byte 9); any
  invalid token, UDL, or keyword is ill-formed. Opcode table (170
  entries) transcribed from cy86-opcode.desc, map-indexed. `Cy86Parser`
  → `vector<Cy86Statement>` (labels attached; operands: register |
  immediate {literal ± negation | label ± addend} | memory {register |
  label | literal base, ± offset}); label table rejects reserved
  spellings, duplicates, and undefined references. Validation enforces
  only the structural rules the reference has: operand count, `w` not
  immediate, `I` immediate-only, exact register width (no 80-bit
  registers), 64-bit address registers, negation of arithmetic scalars
  only. Types are otherwise unconstrained — immediates convert
  byte-mechanically in every category (ref-pinned; see audit.md).
- `dev/src/x86_assembler.h/.cpp` — reusable x86-64 object model +
  encoder, extended in later PAs: generic {66 prefix, REX, opcode,
  modrm, sib, disp, imm} emission for MOV, ADD/SUB/AND/OR/XOR/CMP
  (register and immediate forms), NOT/NEG, SHL/SHR/SAR by cl,
  MUL/IMUL/DIV/IDIV, sign-extension family, SETcc, TEST, JMP/CALL r64,
  Jcc rel8, RET, PUSH/POP, SYSCALL, UD2, and the x87 set (FLD/FSTP
  m32/m64/m80 and st(i), FILD/FISTP m16/m32/m64, F{ADD,SUB,MUL,DIV}P,
  FCOMIP, FCHS). `X86ImmFullWidth` pins the 10-byte mov form where
  encoding size must not depend on the value.
- `dev/src/cy86_codegen.h/.cpp` — one byte-level literal authority
  (`ConvertLiteralBytes`: negation in own width — two's complement
  integral / sign-flip float — then truncate-keep-low or sign/zero
  extend by literal type) feeding u64 immediates, x87 operand bytes,
  and data statements. Deterministic two-pass layout: label-dependent
  64-bit immediates always emit `mov r64, imm64` so statement sizes are
  label-independent; pass 1 sizes with placeholder labels, pass 2 emits
  with final values under a size-equality assertion. O(n), no
  relaxation. Translator register discipline: values in rax/rbx/rcx
  (shift count cl), rdx for widening/mul/div, addresses in rsi/rdi,
  r12–r15/rsp/rbp never scratch; red zone holds bounce slots
  ([rsp-16/-32/-48]) and syscall staging ([rsp-64..-120]) so operand
  loads can never clobber staged values. Float operands are byte
  patterns loaded/stored at operand width (immediates converted,
  registers bounced); comparisons use FCOMIP with above-conditions and
  parity handling for IEEE NaN semantics. Image: ehdr + one RWX phdr at
  0x400000, stub (zero r12–r15, mov rbp,rsp, jmp entry) at 0x400078,
  statements with alignment padding (arrays: element size; scalars:
  fundamental size, long double 16; dataN: N/8), exit-0 epilogue.
  Entry = label `start`, else first statement, else epilogue.
- `dev/cy86.cpp` — driver envelope mirroring nsinit: batch-stdin guard,
  arg loop, per-TU fresh PreprocEngine, token concat dropping per-TU
  EOF, compile, binary write, chmod via syscall 90; any error →
  EXIT_FAILURE.
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_cy86 :=
  cy86_parse cy86_codegen x86_assembler preproc_engine macro_replace
  ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables
  unicode`.

## Checkpoint Ledger

- CP1 — integer core end-to-end (COMPLETED): 14/18 pa9 from 0/18;
  through-pa8 414/414.
- CP2 — multiply/divide/modulus (COMPLETED): 15/18; 400-integer-
  calculator byte-identical to its reference fixture.
- CP3 — x87 float80 (COMPLETED): 18/18; calculator probe 0.333 s.
- CP4 — final architecture audit (COMPLETED): label-immediate size
  stability, syscall staging, byte-mechanical immediate model, float
  negation, float operand shapes, NaN comparisons, structure cleanups;
  432/432, 45 differential probes match cy86-ref, linear compile
  scaling, heavy programs ≤ 2.01 s of the 10 s cap. Details in
  `pa9/audit.md`.
