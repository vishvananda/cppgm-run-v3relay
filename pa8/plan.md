# PA8 Plan — nsinit (initialization, linking, mock program image) [COMPLETE]

Final state: 414/414 fixtures pass through `make test-report-through-pa8`
(354 through-pa7 + 41 pa8 local + 19 pa8 course); pa8 file audit passes.
Architecture review, findings, the 72-probe differential matrix,
performance evidence, and the divergence envelope are consolidated in
`pa8/audit.md`.

`nsinit -o <outfile> <srcfiles...>` runs the PA5 pipeline per srcfile,
parses each TU against `pa8.gram` in strict mode (pa7 grammar +
`= expression` initializers, static_assert, empty-body function
definitions, constexpr/inline, live storage-class semantics), links the
TU models per 3.5, evaluates initializers with 8.5 + clause 4
conversions, lays out and relocates, and writes the binary mock image.
Ill-formed-but-grammatical inputs EXIT_FAILURE (diagnostics mandatory,
unlike pa7 UB). Exit status always graded; outfile compared byte-wise
when the ref succeeds; 10 s/test timeout.

## Stage Design (as built)

Owning boundaries (pa7 reused; pa8 adds two source pairs):

- `dev/src/parser/nsdecl_model.h/.cpp` — additive, default-inert
  extension (nsdecl dump stays byte-stable): `Pa7Variable` gains
  storage/linkage/constexpr/defined/initially_defined/order, the
  initializer expression, and per-declaration `declared_types` (strict
  mode) for post-evaluation bound agreement; `Pa7Function` gains
  inline/defined/order. Redeclaration agreement (types, storage,
  thread_local, linkage, one definition per TU) lives here; 3.5
  signature = name + parameter types, so a same-TU redeclaration that
  matches a signature without matching the type is rejected.
  `IsConstQualified` (top const or const-element array) is the object-
  const authority shared with the parser and sema.
- `dev/src/parser/nsdecl_parser.h/.cpp` — grammar extension in place:
  init-declarator, static_assert-declaration, function-definition (`{}`
  body only), `inline`/`KW_CONSTEXPR`, and expressions exactly per
  pa8.gram (literal, id-expression, parens — no operator layer).
  Expressions are single nodes recording lookup scope, token index, and
  a `decl_epoch` declaration-count snapshot; no evaluation at parse
  time. Strict-mode-only diagnostics: initialization required for
  defining const-qualified objects, references, and constexpr;
  Table-10 sequence validation; constexpr applies top-level const to
  the declarator type (type identity with `T* const`). The pa7 fixtures
  keep passing in non-strict mode (nsdecl unchanged).
- `dev/src/parser/nsinit_sema.h/.cpp` — program-wide identity and
  values. Linking: internal keys are per-unit, external keys per
  (namespace path, name[, full function type]); a first-declaration-
  defining variable binds only to a still-undefined entity (duplicate
  plain definitions coexist, reference-pinned), functions merge
  cross-TU. Values are `Pa8Value` bytes + symbolic relocations
  (entity | temporary | string literal, addend), position-independent
  until layout. Every defined variable's image value is readable during
  translation (memoized, cycle-checked); `is_constant` separately
  tracks 5.19 constancy (constexpr or const-qualified) for
  static_assert/array-bound/constexpr contexts. Conversions: clause 4
  with a qualification-conversion checker for pointers, exact function
  signatures, 8.5.2 string element-kind rules, null-pointer constants;
  pointer-from-pointer-lvalue realizes the source's address
  (reference-pinned). Identifier resolution filters by `decl_epoch`
  and rejects overloaded names.
- `dev/src/parser/nsinit_image.h/.cpp` — magic + BLOCK1 (defined
  variables and declared functions, first-declaration order, one stable
  sort) + BLOCK2 (temporaries, registration order) + BLOCK3 (string
  literals, registration order, no dedup), alignment padding, one
  relocation fix-up pass; symbols with no image object resolve to 0.
- `dev/nsinit.cpp` — envelope mirroring `dev/nsdecl.cpp`: argv `-o`
  contract, batch-stdin not-implemented loop, fresh pipeline per
  srcfile, strict parser mode, link + binary write, exception →
  EXIT_FAILURE.
- `dev/frontend_source_sets.mk` — nsinit = nsdecl set +
  `parser/nsinit_sema` + `parser/nsinit_image`.

Image format (fixture-pinned): magic `'P','A','8','\0'`; fundamentals
align = size (long double 16), pointers/references 8, arrays = element
alignment, function stubs `'f','u','n','\0'` at align 4; pointers are
u64 LE file offsets (image loaded at 0); references bind through to the
underlying entity; `true`/`false` are 0x01/0x00, `nullptr` 8 zero
bytes; constant-initialized bytes else zeros; static_assert message
*and condition* literals are never emitted.

## Checkpoint Ledger

- CP1 (COMPLETE) — semantic core end-to-end: grammar extension,
  annotated expressions, constant evaluation, scalar/pointer
  initialization + conversions, 3.5 linking, BLOCK1 layout + writer,
  static_assert. Evidence: `make test-pa8` 57/60,
  `make test-report-through-pa7` 354/354, audit pass.
- CP2 (COMPLETE) — references + lifetime-extended temporaries (BLOCK2),
  unknown-bound character arrays + string literals (BLOCK3), chained
  reference lvalues, aligned relocations. Evidence: 60/60 pa8, 354/354
  through-pa7, three focused images byte-match their refs.
- CP3 (COMPLETE) — final architecture cleanup: relaxed initializer
  folding with 5.19 separation, pointer-conversion checking,
  declaration-epoch visibility, reference linking-identity rules,
  post-evaluation bound agreement, overload rejection, static_assert
  string suppression, removal of the non-grammar operator layer,
  symbol-index maps. Evidence: 414/414 through-pa8, 72 differential
  probes byte-identical, linear 20k→40k and 5k→10k scaling
  (79× lead on the reference at 20k); details in `pa8/audit.md`.
