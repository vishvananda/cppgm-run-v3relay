# PA9 Plan — cy86 (CY86 mock IL → x86-64 ELF executable)

Contract: `cy86 -o <out> <src1..srcN>` runs the PA5 preprocessor+tokenizer per
TU, concatenates the token sequences in argv order, parses the regular CY86
grammar (pa9/pa9.gram), semantically checks operands against
pa9/cy86-opcode.desc, translates each CY86 instruction to a fixed x86-64
sequence, assembles machine code, and writes a single-PT_LOAD ELF executable
(chmod 0755). Grading: impl exit status must match ref (`0` well-formed, `1`
ill-formed); when 0, the generated program is executed with the test's
`.stdin` and its stdout + exit status are compared to text fixtures. 18
fixtures: 11 pa9/tests + 7 pa9/course/pa9. Timeouts: build 30 s
(CPPGM_BUILD_TEST_TIMEOUT_SEC), program run 10 s
(CPPGM_PROGRAM_TEST_TIMEOUT_SEC). No fixture or ref regeneration is needed
(all `.ref.*` fixtures are checked in); course `.ref.program` binaries are
Mach-O (fixtures came from macOS) — only stdout/exit text fixtures gate.

## Stage Design

Owning boundaries (new code; PA5 pipeline reused as-is):

- `dev/src/cy86_parse.h/.cpp` — token acquisition + parse + sema.
  `Cy86TokenCollector : IPostTokenOutputStream` (posttoken_stream.h)
  preserving raw PA2 bytes: literals keep `EFundamentalType`, byte vector,
  num_elements (Pa6Token is unusable here: it folds literal data to a u64,
  losing 10-byte long doubles and arrays). emit_invalid, any UDL emit, and
  any KW_* simple token → ill-formed. `Cy86Opcode` table transcribed
  statically from pa9/cy86-opcode.desc (171 entries; descriptor chars
  w/r/a/b/i/s/u/f/I + width 8..80). `Cy86Parser` → `vector<Cy86Statement>`
  (labels attached; operand model: Register{reg,width} | Immediate{literal
  bytes/type, negate, label, label±lit} | Memory{base reg | label | lit,
  ±offset}); label table (spelling clash with opcode/register/label →
  ill-formed; undefined label reference → ill-formed). Operand checks:
  count, `w` not immediate, `I` immediate only, register width == operand
  width exactly (no 80-bit registers ⇒ w80 is memory-only), memory address
  register must be 64-bit.
- `dev/src/x86_assembler.h/.cpp` — reusable x86-64 object model + encoder
  (kept small, extended in later PAs): `X86Instruction` (mnemonic form, ops)
  → encoded bytes via generic {legacy prefix 66, REX, opcode, modrm, sib,
  disp, imm}. Mnemonic forms needed: MOV (r/m↔r 8/16/32/64, r64←imm64,
  r/m←imm32), ADD SUB AND OR XOR CMP (r/m,r), NOT NEG (r/m), SHL SHR SAR
  (r/m,cl), MUL IMUL DIV IDIV (one-operand), CBW/CWDE/CDQE CWD/CDQ/CQO,
  SETcc (X86Condition in x86_register_model.h), TEST (r/m8,r8), JMP/CALL
  (r64), Jcc rel8, RET, PUSH r64, SYSCALL, UD2, and x87: FLD/FSTP
  (m32/m64/m80), FILD/FISTP (m16/m32/m64), FADDP FSUBP FMULP FDIVP,
  FCOMIP/FUCOMIP, FSTP st(0). Register enums already exist in
  `dev/src/x86_register_model.h` (X64Register, X86Condition).
- `dev/src/cy86_codegen.h/.cpp` — `Cy86ToX86Translator` (per-opcode fixed
  sequences, register discipline below) + layout + ELF writer.
  Deterministic two-pass layout: label-dependent immediates always emit
  `mov r64, imm64` (10 bytes) so statement sizes are label-independent;
  pass 1 walks statements computing alignment padding + addresses + label
  values, pass 2 emits with final values (assert sizes match). O(n), no
  relaxation loop.
- `dev/cy86.cpp` — driver rewrite mirroring dev/nsinit.cpp envelope: keep
  HasBatchStdinArg guard, arg loop (keep `--target` tolerance), per-TU
  fresh PreprocEngine (`RunSingleFile(src, collector)` with PA5GetFileId +
  build stamp exactly as nsinit's AnalyzeTranslationUnit), token concat
  across TUs dropping per-TU EOF, compile, binary write (trunc), chmod via
  existing PA9SetFileExecutable (syscall 90). Reuse the stub's ElfHeader /
  ProgramSegmentHeader structs. NotImplementedException throw removed; any
  error → EXIT_FAILURE.
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_cy86 :=
  cy86_parse cy86_codegen x86_assembler preproc_engine macro_replace
  ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode`.

Program image (pinned by ref disassembly + fixtures):

- ELF: ehdr(64) + one phdr(56, PT_LOAD RWX, vaddr 0x400000, offset 0),
  body at vaddr 0x400078; filesz = memsz = 120 + body. ELF entry = 0x400078.
- Body = stub, statements, epilogue. Stub (27 B): xor r12..r15 (zeroed
  registers — ref does this), `mov rbp,rsp`, `mov rax, <entry>; jmp rax`
  where <entry> = label `start` if defined, else first statement, else
  epilogue. Epilogue (fall-through off program end): `mov rax,60; mov
  rdi,0; syscall; ud2` — the empty program (course 100-empty-program) must
  build AND exit 0.
- Alignment on virtual addresses (≡ file offsets here): scalars align =
  PA2 size except long double = 16 (payload is 10 bytes —
  posttoken_stream.cpp emits FT_LONG_DOUBLE as 10; course
  500-long-double-label-alignment asserts addr%16==0); arrays align =
  element size (course 500-string-literal-element-alignment: char[] align
  1, char32_t[] align 4); dataN aligns to N/8. Zero padding.

Semantic rules the tests pin:

- Signed integral (sign-extends): FT_SIGNED_CHAR/SHORT/INT/LONG/LONG_LONG,
  FT_CHAR, FT_WCHAR_T (course 400-negated-wchar-sign-extension: ref
  program exit 255 ⇒ `(-L'A')` = sign-extended 0xFFFFFFFFFFFFFFBF).
  Unsigned: unsigned family, FT_CHAR16_T, FT_CHAR32_T, FT_BOOL. Floats:
  FT_FLOAT/DOUBLE/LONG_DOUBLE. Arrays: non-arithmetic (negation
  ill-formed), widen by zero-extension.
- Immediate width conversion (byte-mechanical, little-endian): too long →
  drop highest-order bytes (keep low `width/8`); too short → sign-extend
  iff signed integral else zero-extend. Negation only for arithmetic
  types, applied in the literal's own width before widening. Bare label =
  u64; `(label±intlit)`: lit must be integral, extended to 64, then
  added/subtracted; result u64.
- Grammar strictness (course 300-*: all impl exit 1): `-lit` and
  `label±lit` immediates require parens outside statement-literal position;
  memory `[lit]` has no minus form (`[-1]` ill-formed).
- Register mapping: sp→rsp, bp→rbp, x→r12, y→r13, z→r14, t→r15. 32-bit
  writes zero upper 32 (use 32-bit mov forms — matches x86); 8/16-bit
  writes preserve upper bits.

Translator register discipline (per CY86 instruction, fixed sequence):
operand values load into rax/rbx/rcx (op3 shift count in cl), rdx reserved
for widening/mul/div high half; memory addresses computed into rsi/rdi
(recomputed for writeback; never live across the central op); r12–r15/
rsp/rbp are never scratch. syscallN: load params op3..opN into
rdi,rsi,rdx,r10,r8,r9 (values first, one at a time), syscall number into
rax last, then `syscall` (clobbers rcx/r11 — both scratch), store rax to
op1. call: load target into rbx, then `call rbx` as final instruction
(pushed x86 return address == next CY86 statement address); ret: `ret`;
jumpif: cond byte → al, target → rbx, `test al,al; je +2; jmp rbx`. x87
ops: fld/fild operands (via memory or red zone [rsp-8]/[rsp-16] for
register/immediate sources), f-op, fstp/fistp to destination (red zone
bounce for register destinations). u64convf80: fild + add 2^64 correction
when top bit set; f80convu64: subtract 2^63 bias, fistp, re-add.

## Failure Map

All 18 tests currently fail with impl exit 86 (driver stub throws
NotImplementedException; nothing exists yet). By unblocking component:

- Integer core end-to-end (CP1): 100-noop, 100-ret42, 110-hello-world,
  200-duplicator, 210-reverser (move/jump/jumpif/call/ret/syscall/
  iadd/isub/ieq/data/literal statements); course 100-empty-program
  (epilogue), 3× course 300-*-bad (parse strictness),
  400-negated-wchar-sign-extension (negation + urshift64), 2× course 500-*
  alignment (layout). 12 tests.
- mul/div/mod (CP2): 220-hexdump (umod64, urshift8), 300-binary-calculator
  (umul64), 400-integer-calculator (umul64/udiv64/umod64/not). 3 tests.
  (Only unsigned variants appear in fixtures; signed still implemented.)
- x87 float80 (CP3): 500-to-float80 (all [su]NconvF80 + f32/f64convf80),
  501-from-float80 (all f80conv*), 600-float-calculator (f{add,sub,mul,
  div}{32,64,80} + all float comparisons). 3 tests.

## Performance Risks

- Program-run 10 s timeout with large IO: 300-binary-calculator (16 MB
  stdin, 8.5 MB stdout), 500-to-float80 (12 MB in / 24 MB out, 1M cases).
  Test programs buffer IO themselves (iobegin reads stdin up front), so
  syscall count is low; the risk is per-instruction expansion. The naive
  load/op/store scheme (~10–20 x86 instructions per CY86 instruction) is
  what the ref uses and it passes; keep expansions fixed and small, avoid
  anything super-constant. No relaxation/iteration in layout (fixed imm64
  encodings ⇒ single sizing pass).
- Compile side trivial (inputs ≤ 323 lines); reuse of PreprocEngine per TU
  matches nsinit and is not hot.

## Checkpoint Ledger

- CP1 — integer core end-to-end (ACTIVE). Full parse/sema for the entire
  opcode table + layout + ELF + stub/epilogue + assembler + translation
  for data/literal, move8..64, jump/jumpif/call/ret, not/and/or/xor,
  shifts, iadd/isub, all integer comparisons, syscall0..6. Floats and
  mul/div/mod parse+validate but translate → internal "later-CP" error.
  Progress proof: failing tests drop 18 → 6 (the 12 CP1 tests above pass;
  make test-pa9 shows only 220/300/400/500/501/600 failing).
- CP2 — multiply/divide/modulus: one-operand MUL/IMUL/DIV/IDIV with
  widening (xor rdx / cqo family; 8-bit uses ax with al/ah split).
  Progress proof: 6 → 3 failing (220-hexdump, 300-binary-calculator,
  400-integer-calculator pass).
- CP3 — x87 float80: conversions, arithmetic, comparisons (FCOMIP flag
  idiom: order operands so flt→setb, fle→setbe, fgt/fge by swap; feq/fne
  → sete/setne), move80 (10-byte copy), red-zone bounce buffers.
  Progress proof: 3 → 0 failing; `make test-pa9` clean.
- CP4 — stage close: `make test-report-through-pa9` clean, file audit
  clean, perf probe recorded, architecture notes appended here.

## Active Checkpoint: CP1 — integer core end-to-end

Implementation Packet:

- Files/symbols to create/modify: `dev/src/cy86_parse.h/.cpp`
  (Cy86TokenCollector, Cy86Literal{type,bytes,num_elements},
  Cy86Opcode table + descriptor parser, Cy86Parser, Cy86Statement,
  Cy86Operand, Cy86Error), `dev/src/x86_assembler.h/.cpp`
  (X86Instruction, Encode → vector<unsigned char>),
  `dev/src/cy86_codegen.h/.cpp` (Cy86ToX86Translator, Cy86Layout,
  BuildProgramImage returning full file bytes), rewrite `dev/cy86.cpp`
  driver (mirror dev/nsinit.cpp envelope; keep batch guard, ElfHeader/
  ProgramSegmentHeader structs, PA9SetFileExecutable), update
  `FRONTEND_OBJ_BASENAMES_cy86` in `dev/frontend_source_sets.mk` (list
  above). Reuse untouched: preproc_engine.h (RunSingleFile),
  posttoken_stream.h (IPostTokenOutputStream), posttoken_types.h
  (EFundamentalType, ETokenType OP_SEMICOLON/OP_COLON/OP_MINUS/OP_PLUS/
  OP_LPAREN/OP_RPAREN/OP_LSQUARE/OP_RSQUARE), x86_register_model.h,
  exceptions.h.
- Fixture groups: pa9/tests/100-*,110-*,200-*,210-* and pa9/course/pa9/*
  (7 files) must pass; 220/300/400/500/501/600 remain failing with impl
  exit 86/1 — acceptable only for these six.
- Required spec facts: all in "Stage Design"/"Semantic rules" above
  (image layout + stub/epilogue bytes, alignment table, width-conversion
  and signedness rules, register mapping/discipline, error taxonomy,
  entry-point rule, two-pass layout invariant). Opcode operand
  descriptors: transcribe pa9/cy86-opcode.desc verbatim into the static
  table; grammar: pa9/pa9.gram (regular; statements end at OP_SEMICOLON;
  labels recursive via `label: statement`).
- Focused commands: build `make -C dev cy86`; single case
  `cd pa9 && ../dev/cy86 -o /tmp/t.prog tests/100-ret42.t.1 && /tmp/t.prog;
  echo $?` (expect 42); suite `make test-pa9` (root). Broad command:
  `make test-report-through-pa8` must stay clean (nothing in the shared
  pipeline may change behaviorally); `make test-report-through-pa9` is the
  stage exit gate (CP4).
- Performance probe: `time ./210-reverser.my.program < 210-reverser.stdin`
  style manual run is enough at CP1; from CP2 on, run 300-binary-calculator
  and at CP3 500-to-float80 via `time` and require < 5 s wall (half the
  10 s CPPGM_PROGRAM_TEST_TIMEOUT_SEC budget).
- File audit: `perl scripts/cppgm_file_audit.pl --stage pa9 --paths
  dev/src` — limits: source ≤ 3000 lines, function ≤ 240 lines, nesting
  ≤ 8; split per-opcode-family translate helpers rather than one giant
  switch. (One pre-existing warning on recog_parser.h is expected.)
- Known uncertainties: (1) ref inserts a 16-byte `jmp`+pad before
  instructions that follow ≥4-aligned literal data (seen in the
  500-string Mach-O at body offset 0x30) — hello-world's 15-byte stdout
  proves no padding after align-1 data, and no graded fixture depends on
  the quirk; do not replicate, revisit only if a label-arithmetic
  mismatch appears. (2) String-array immediates at integer widths and
  float literals at mismatched float widths: apply the mechanical byte
  truncate/extend rule; fixtures appear never to exercise these. (3)
  Whether unused-but-clashing or undefined-but-unreferenced labels error:
  follow the spec text (clash always ill-formed; only referenced labels
  need definitions) unless a fixture disagrees. (4) x87 default control
  word (round-nearest, 64-bit precision) is assumed for conversions —
  "exactly representable" UB clause covers rounding differences.
