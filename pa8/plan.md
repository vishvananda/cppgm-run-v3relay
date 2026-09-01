# PA8 Plan — nsinit (initialization, linking, mock program image)

State: CP2 complete — 60/60 pa8 fixtures pass; through-pa7 is green (354/354)
and the pa8 file audit passes with the pre-existing `recog_parser.h` warning.
Harness: `text_t1` mode — all
`NNN-name.t.*` files are passed to
`nsinit -o <out>` in sorted order; exit status always graded; outfile compared
byte-wise only when the ref exits EXIT_SUCCESS; 10 s/test timeout.

`nsinit` extends pa7 `nsdecl`: parse each TU against `pa8.gram` (adds
expressions over literals/id-expressions, `= expression` initializers,
static_assert, empty-body function definitions, `constexpr`/`inline` and live
storage-class semantics), annotate types/value categories/constant values,
apply 8.5 initialization + clause 4 conversions, link TUs per 3.5, lay out and
relocate, and write the binary mock image. Ill-formed-but-grammatical inputs
must EXIT_FAILURE (diagnostics are now mandatory, unlike pa7 UB).

## Stage Design

Owning boundaries (reuse pa7; new pa8 code in two new source pairs):

- `dev/src/parser/nsdecl_model.h/.cpp` — additive extension, default-inert so
  the nsdecl dump is byte-stable: `Pa7Variable` gains storage
  (static/thread_local), linkage, constexpr/const, defined flag, program-order
  index, annotated initializer value; `Pa7Function` gains inline flag,
  linkage, defined flag, signature identity. Type factories gain the
  pa8-mandatory rejections that pa7 canonicalized or ignored (direct `& &`,
  pointer-to-reference, reference-to-void) — typedef-mediated ref collapse
  (pa7 370) must keep working, so rejection keys off direct-declarator
  composition, not canonicalization.
- `dev/src/parser/nsdecl_parser.h/.cpp` — grammar extension in place:
  init-declarator (`= expression`), static_assert-declaration,
  function-definition (`{}` body), function-specifier `inline`,
  `KW_CONSTEXPR`, expression parsing (KW_TRUE/KW_FALSE/KW_NULLPTR/TT_LITERAL/
  parens/qualified id-expression), 3.4.3p3 declarator-id scope change for the
  initializer. A parser mode flag (pa7 vs pa8-strict) gates
  initialization-required diagnostics (uninit const/reference, void object):
  pa7 fixtures (e.g. 140-declarators) must keep passing; nsdecl runs pa7
  mode, nsinit runs strict.
- `dev/src/parser/nsinit_sema.h/.cpp` (new) — expression annotation {type,
  value category, value}, constant-expression evaluation (5.19), standard
  conversions (4.x), initialization semantics (8.5, 8.5.2 char arrays, 8.5.3
  references). Values are position-independent: bytes + relocations
  (symbol = entity | temporary | string-literal, addend), so constexpr
  pointers work before layout.
- `dev/src/parser/nsinit_image.h/.cpp` (new) — program-wide linking (3.5 +
  ODR 3.2), entity ordering, layout (alignment/padding), relocation
  resolution, image byte emission.
- `dev/nsinit.cpp` — envelope mirroring `dev/nsdecl.cpp`: argv `-o` contract,
  batch-stdin loop, PA5 preproc per srcfile, parse+annotate each TU in
  command-line order, link, write outfile in binary mode; any diagnosis →
  EXIT_FAILURE.
- `dev/frontend_source_sets.mk` — nsinit = nsdecl set + `parser/nsinit_sema`
  + `parser/nsinit_image`.

Image format (fixture-pinned):

1. Magic 4 bytes `'P','A','8','\0'`; then BLOCK1, BLOCK2, BLOCK3, each object
   zero-padded to its alignment (fundamental align = size; pointer/reference
   8; array = element align; function mock 4).
2. BLOCK1: defined variables + declared functions (definition not required
   for functions), program first-declaration order, TUs in command-line
   order; functions emit `'f','u','n','\0'`; variables emit constant-initialized
   bytes else zeros (110, 130, 200-char-before-function-alignment pins the
   char-then-pad-to-4 case).
3. BLOCK2: lifetime-extended temporaries in first-use order (450: `const
   int& i = 3` → pointer at 8 → temp int 3 after BLOCK1).
4. BLOCK3: string-literal objects in program token order, aligned to element
   size (310); static_assert message literals are NOT emitted (500-static-
   assert2, course 120-constexpr-pointer-cross-tu emit no `""`/`"x"`).
5. Pointers/references are u64 LE file offsets (image loaded at 0);
   references bind to the underlying entity through reference lvalues (700:
   `y = x` where x is a ref to v yields &v).

## Failure Map

All 60 failures are EXIT_NOT_IMPLEMENTED, blocked on the missing pipeline.
By owning boundary once the envelope exists:

- Scalar init, conversions, linkage (nsinit_sema + nsinit_image): 110-*,
  120-linkedvar, 130-staticvar, 150-thread-local, 200-fundamental,
  250-nullptr, 600-deep-namespace-initialization; course 120-constant-
  conversion-linkage (2→bool, 2.0→int, const/constexpr internal-by-default,
  extern-constexpr definition), 120-constexpr-pointer-cross-tu,
  120-constexpr-qualified-pointer, 150-static-thread-local-cross-tu,
  150-thread-local-redeclaration-agreement, 300-cv-through-typedef-constant.
- static_assert + constant expressions (nsinit_sema): 500-static-assert
  (non-constexpr operand → fail), 500-static-assert2 (const int is constant),
  500-static-assert3 (symbolic pointer → bool true); course 300-constexpr-
  missing-initializer-bad, 300-constexpr-nonconstant-initializer-bad.
- Functions, signatures, ODR (model + nsinit_image): 200-function,
  300-inline-function (multi-TU inline def = one entity), 400-double-func-def,
  400-linked-function; course 200-function-typedef-declarations,
  300-function-typedef-definition-bad, 200-char-before-function-alignment.
- References + temporaries (nsinit_sema BLOCK2): 450-reference,
  700-reference-to-reference (const-ref lvalue-to-rvalue feeds an array
  bound), 300-uninit-ref, 300-void-ref, 300-bad-ref1/2/3 (direct-declarator
  type rejections); course 450-cv-dropping-reference-bad,
  450-lvalue-to-rvalue-reference-bad.
- Arrays + string literals (nsinit_sema BLOCK3): 300-array,
  310-array-str-lit (copy into array AND emit literal), 340-array-const
  (const array uninit), 350-function-to-pointer; course
  300-scalar-array-initializer-bad, 350-integer-to-pointer-bad,
  350-string-to-mutable-pointer-bad.
- Mandatory namespace/entity diagnostics (parser/model, pa7 rules now
  graded): 120-const-default, 150-inline-namespace(2), 400-namespace-alias-
  misuse, 400-namespace-alias-to-self, 400-using-decl-to-namespace,
  410-namespace-conflict1..6, 300-nonenclosing-qualified-decl,
  600-qualified-redeclaration(2); course 300-invalid-fundamental-
  specifiers-bad, 300-thread-local-redeclaration-mismatch-bad.

## Performance Risks

- 600-deep-namespace-initialization: 1800 lines, ~hundreds of nested
  `namespace a {` levels with initialized ints inside. Recursive-descent
  depth and per-name scope-chain lookup must stay O(depth); reuse pa7's
  lookup (0.012 s on a 200-chain probe) — no per-lookup recomputation of
  transitive using/inline sets beyond pa7's DFS-with-visited.
- Layout/link must be one pass each: first-declaration order via a global
  monotonic counter at declaration time (no post-hoc sorts); relocation is a
  single fix-up pass after offsets are final (values carry symbol+addend, no
  fixed-point iteration).
- Array sizes come from u64 literal bounds: check `N*sizeof(T)` overflow and
  emit zero runs without materializing per-entity temporaries beyond the
  image buffer (fixture images are ≤ a few KB; a single vector<char> image is
  fine).

## Checkpoint Ledger

- CP1 (COMPLETE) — semantic core end-to-end: grammar extension, annotated
	 expressions, constant evaluation, scalar/pointer initialization +
	 conversions, 3.5 linking, layout + writer (BLOCK1 + static_assert; BLOCK2/3
	 remain intentionally deferred). Evidence: `make test-pa8` is 57/60,
	 `make test-report-through-pa7` is 354/354, and the pa8 source audit passes
	 (one pre-existing warning in `recog_parser.h`).
- CP2 (COMPLETE) — references + lifetime-extended temporaries (BLOCK2),
  unknown-bound character-array initialization + string literals (BLOCK3),
  chained reference lvalues, and aligned relocations. Evidence: all three
  focused images match their checked-in refs; `make test-pa8` is 60/60,
  `make test-report-through-pa7` is 354/354, and the pa8 source audit passes
  with one pre-existing warning in `recog_parser.h`.
- CP3 — mandatory-diagnostic hardening (namespace conflicts, alias misuse,
  qualified-decl scope, thread_local agreement, ODR) + perf probes + audit.
  Progress proof: 60/60 `make test-pa8`, clean
  `make test-report-through-pa8`, audit notes in `pa8/audit.md`.

## Completed Checkpoint — CP1: semantic core end-to-end

Build the full nsinit pipeline (parse → annotate → link → layout → write) with
initialization/conversion/linking semantics for fundamentals and pointers,
function entities, and static_assert. References, arrays-from-strings, and the
conflict-diagnostic sweep stay stubbed-but-structured for CP2/CP3 (a stub must
still fail closed: unimplemented ill-formed paths must not exit success on
graded fixtures; if a CP1-group fixture needs the path, implement it now).

Done when: `make test-pa8` shows the scalar/linkage/function/static_assert
groups passing (≥25/60, from 0) and `make test-report-through-pa7` is still
354/354.

### Implementation Packet

Files/symbols to touch:
- `dev/src/parser/nsdecl_model.h/.cpp`: extend `Pa7Variable`/`Pa7Function`
  (storage, linkage enum, constexpr/const, defined, order index, `Pa8Value`
  initializer); keep `PrintTranslationUnit` output byte-identical;
  `AddOrMergeVariable`/`AddOrMergeFunction` learn redeclaration-agreement
  checks (type merge already exists; add storage/thread_local/linkage
  agreement gated on strict mode). Functions match by signature (name +
  parameter types, 3.5) — model must allow declaration-vs-definition state.
- `dev/src/parser/nsdecl_parser.h/.cpp`: `Pa7Parser` gains strict-mode flag +
  productions: init-declarator, initializer, static_assert-declaration,
  function-definition (empty body; body distinguishes definition),
  function-specifier, KW_CONSTEXPR, expression/constant-expression.
  Expressions resolve id-expressions with the existing lookup machinery
  (qualified + unqualified); 3.4.3p3: after a qualified declarator-id the
  initializer looks up in the declarator's namespace scope.
- `dev/src/parser/nsinit_sema.h/.cpp` (new): `Pa8Value` (bytes + relocs
  {offset, symbol, addend} + is_constant), expression annotation, 5.19
  evaluation (lvalue-to-rvalue of const integral with preceding constant
  initializer; through references), clause 4 conversions
  (integral/floating/boolean/pointer, null pointer constant), 8.5 scalar
  initialization driver.
- `dev/src/parser/nsinit_image.h/.cpp` (new): link entities across TU models
  keyed by (namespace path, name[, signature]); internal-linkage and
  unnamed-namespace entities stay per-TU; BLOCK1/2/3 layout with alignment
  padding; relocation; byte emission.
- `dev/nsinit.cpp`: replace stub with the nsdecl envelope pattern (binary
  ofstream, strict parser mode, link + write, catch → EXIT_FAILURE).
- `dev/frontend_source_sets.mk`: `FRONTEND_OBJ_BASENAMES_nsinit :=` nsdecl's
  list + `parser/nsinit_sema parser/nsinit_image`.

Fixture groups to drive CP1 (in order): pa8/tests 100-empty-decl, 110-*,
120-linkedvar, 120-const-default, 130-staticvar, 150-*, 200-*, 250-nullptr,
400-double-func-def, 400-linked-function, 500-static-assert*,
600-qualified-redeclaration*; course 120-*, 150-*,
200-char-before-function-alignment, 200-function-typedef-declarations,
300-constexpr-*-bad, 300-cv-through-typedef-constant,
300-invalid-fundamental-specifiers-bad, 600-deep-namespace-initialization.

Required spec facts (verified against fixtures):
- Magic is `"PA8"` as array of 4 char (`50 41 38 00`); function stub likewise
  `"fun"` → `66 75 6e 00`; all multi-byte values little-endian; alignment =
  size for fundamentals (table in pa8/README.md lines 101–123), 8 for
  pointers/references, 16 for long double; `true`=0x01, `false`=0x00,
  `nullptr` = 8 zero bytes (unique type, distinct from any pointer type until
  converted).
- BLOCK1 admits *defined* variables but merely *declared* functions
  (README "Variables/Functions"); 120-linkedvar pins the position at first
  declaration (extern decl in TU1) with the value from the TU2 definition.
- Linkage (3.5/7.1.1): `static` → internal; const/constexpr namespace-scope
  variable → internal unless declared `extern` (course
  120-constant-conversion-linkage: two internal `x` entities emitted, one per
  TU); `extern constexpr T v = init;` is an external definition (course
  120-constexpr-pointer-cross-tu); unnamed-namespace members internal (130);
  constexpr on a pointer variable implies top-level const for type identity
  (`extern const char* const p;` matches `extern constexpr const char* p`).
- thread_local must agree across redeclarations of one entity (course
  150-thread-local-redeclaration-agreement vs
  300-thread-local-redeclaration-mismatch-bad); storage duration does not
  change BLOCK1 ordering.
- ODR: one definition per variable/non-inline function program-wide
  (400-double-func-def fails, same TU; 400-linked-function succeeds);
  `main` is not required (several course fixtures omit it).
- static_assert: operand must be a constant expression (500-static-assert
  fails on non-const `int x = 3`), converted to bool; a constexpr pointer
  with symbolic value (symbol+addend, e.g. to a string literal or array) is
  a non-null constant → true (500-static-assert3, course
  120-constexpr-pointer-cross-tu). Message literal is not emitted to BLOCK3.
- Conversions pinned by course 120-constant-conversion-linkage: `bool b = 2`
  → 0x01; `int i = 2.0` → 2; course 350-integer-to-pointer-bad: nonzero
  integer literal to pointer is ill-formed (only null pointer constants
  convert).
- 120-const-default: const object without initializer → EXIT_FAILURE (strict
  mode only — pa7 fixtures contain uninitialized non-const declarations and
  must keep passing in pa7 mode).
- Empty-body `{}` is the only function body; `int main(){};` includes a
  trailing empty-declaration (grammar allows it; 300-bad-ref1).

Commands:
- Build + focused: `make -C dev nsinit && ./dev/nsinit -o /tmp/o.img
  pa8/tests/110-vardef.t.1; echo $?; xxd /tmp/o.img` (multi-TU: pass `.t.1
  .t.2` in order). Oracle: `./dev/nsinit-ref -o /tmp/r.img <same args>` then
  `cmp`/`xxd` (observation/probing only — never shell out from the tool).
- Stage: `make test-pa8`.
- Broad gates: `make test-report-through-pa7` (must stay 354/354), then
  `make test-report-through-pa8`.
- Audit: `perl scripts/cppgm_file_audit.pl --stage pa8 --paths dev/src`.
- Performance probe: `time ./dev/nsinit -o /tmp/o.img
  pa8/course/pa8/600-deep-namespace-initialization.t.1` (expect well under
  1 s; 10 s harness timeout), plus a generated ~20k-declaration flat file to
  confirm linear scaling before CP3.

Known uncertainties (probe with nsinit-ref, then pin in code + audit.md):
- Whether duplicate string-literal tokens are deduplicated in BLOCK3, and
  whether a literal in a static_assert *condition* (as opposed to message) is
  emitted — README says "in order of their tokens", but message strings are
  provably excluded; CP2 concern, structure `Pa8Value` reloc targets now.
- Whether an odr-used (or merely declared) extern variable with no definition
  anywhere is diagnosed at link time or silently omitted from BLOCK1 — README
  says only "defined" variables are appended; probe
  `extern int x; int main(){}` single-TU.
- Whether redeclaration after the first definition (e.g. `int x = 1; extern
  int x;`) affects ordering or agreement checks; probe before hardening
  AddOrMergeVariable.
- Exact diagnosis point for `signed float` and friends: pa7 built Table 10
  combination validation already; verify it rejects in pa8 mode rather than
  silently defaulting (course 300-invalid-fundamental-specifiers-bad).

## Completed Checkpoint — CP2: references, arrays, and remaining image data

Extended the semantic and image ownership path for lifetime-extended reference
temporaries (BLOCK2), array/string initialization (BLOCK3), and chained
reference lvalue-to-rvalue conversion. Unknown-bound arrays now complete from
the string element width, and BLOCK2/BLOCK3 objects are aligned and relocated
in their specified order.

### Implementation Packet

Files/symbols to touch:
- `dev/src/parser/nsdecl_parser.h/.cpp`: initializer lists, array bounds and
  reference-binding expressions; retain qualified/unqualified lookup scopes.
- `dev/src/parser/nsinit_sema.h/.cpp`: reference binding and temporary
  entities, array/string value construction, cv-safe conversions, and literal
  ordering.
- `dev/src/parser/nsinit_image.h/.cpp`: BLOCK2/BLOCK3 placement and
  relocations, including alignment and lifetime-extended temporary identity.

Focused fixtures: `pa8/tests/310-array-str-lit.t.1`,
`pa8/tests/450-reference.t.1`, `pa8/tests/700-reference-to-reference.t.1`,
all matching their checked-in reference images. Exit evidence is
`make test-pa8` at 60/60, `make test-report-through-pa7` at 354/354, and the
pa8 source audit passing.

## Active Checkpoint — CP3: mandatory diagnostics and final audit

Harden the existing parser/model/sema ownership path for the remaining
diagnostic-required namespace conflicts, alias and using misuse, non-enclosing
qualified declarations, thread-local/redeclaration agreement, and ODR cases.
Preserve the 60/60 pa8 baseline while validating the deep-namespace path and
completing the final audit.

### Implementation Packet

Files/symbols to touch:
- `dev/src/parser/nsdecl_model.h/.cpp`: declaration agreement and ODR
  ownership in `AddOrMergeVariable`/`AddOrMergeFunction`, including storage,
  linkage, and thread-local attributes.
- `dev/src/parser/nsdecl_parser.h/.cpp`: namespace/alias/using conflict
  diagnostics and qualified-declarator scope checks in the strict parser.
- `dev/src/parser/nsinit_sema.h/.cpp`: linked-entity agreement and diagnostic
  propagation without weakening the completed BLOCK1–3 image path.

Focused fixtures: the `410-namespace-conflict*`, namespace-alias misuse,
`300-nonenclosing-qualified-decl`, `600-qualified-redeclaration`, and
thread-local agreement cases named in the failure map, followed by the deep
namespace performance probe. Exit evidence is 60/60 `make test-pa8`,
`make test-report-through-pa8`, the pa8 source audit, and the recorded probe
result.
