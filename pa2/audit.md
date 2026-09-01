# PA2 Final Architecture Audit — posttoken

## Scope and method

Traced representative facts through their full ownership paths: pp-token →
`PostTokenStream` classification → typed value/bytes → `IPostTokenOutputStream`
emission → tool presentation (`DebugPostTokenOutputStream`). Measured scaling
on 100k/200k-line mixed-token and 100k/200k-element string-sequence inputs,
counted syscalls, and ran differential conformance (fixtures, a 1.8M-token
bulk diff, and 2,200 seeded randomized trials) against `posttoken-ref`.

## Ownership trace (clean)

- **Routing**: `PostTokenStream : IPPTokenStream` is the single owner of
  pp-token → token conversion. Identifier/op-or-punc route through one
  `static const` keyword/operator map (`posttoken_tables.cpp`); `#`, `##`,
  `%:`, `%:%:`, header-names, and non-whitespace-chars fall out as invalid by
  table miss or direct emission. Vocabularies are enums (`ETokenType`,
  `EFundamentalType` in `posttoken_types.h`, spec.md §2).
- **pp-numbers**: `ClassifyPPNumber` owns the grammar split once — integer
  body then integer-suffix/ud-suffix, else floating body then
  floating-suffix/ud-suffix — and records `body_end` plus the parsed
  `IntegerSuffix`; emission never re-derives them. Integer valuation
  accumulates into `uint64_t` with a pre-multiply overflow check (no bignum,
  no rescan); candidate types come from static rank-indexed lists (2.14.2);
  value bytes render once through `EncodeScalar`. Floating scans through
  `PA2Decode_*` only.
- **Character literals**: `ParseCharacterLiteral` (prefix, one escape or one
  UTF-8 code point, closing quote) → `SelectCharacterType` (course
  scalar-range and one-code-unit rules) → `EncodeScalar` → emit; ud-form
  splits the suffix first and validates identifier-shape starting `_`.
- **String sequences**: `append_string_token` accumulates the space-joined
  source, the 0-or-1 distinct encoding prefix, the 0-or-1 distinct ud-suffix,
  and one `vector<StringSequenceElement>` (code point or numeric-escape code
  unit); any non-string token or eof flushes. `EncodeStringElements` resolves
  the prefix to an element width once and encodes in one pass; numeric escapes
  are width-checked against the element type, code points re-encode per
  target. The `operator ""` reserved-suffix split applies only to a singleton
  empty ordinary `""` with a non-`_` suffix immediately after `operator`.
- **Value → bytes**: one owner, `EncodeScalar`/`AppendCodeUnit`, renders
  scalar values byte-wise little-endian per the ABI; the sole exception is
  the x87 long double, copied from the scanned value with its 6 undefined
  padding bytes zeroed.
- **Presentation**: hexdump and line formatting live only in the tool's
  `DebugPostTokenOutputStream`; the core never touches iostreams. Static
  state is const tables only, so in-process batch re-entry is safe.
- **Unicode**: `unicode.cpp` (extracted at CP2) is the one production UTF-8
  decode/encode + scalar-value check, shared by lexer and classifier.

## Findings and changes (this cleanup)

1. **Per-token flush in the tool emitter (material, fixed)**: `endl` after
   every token forced one `write(2)` per token — 1,800,001 writes for a
   100k-line input, as in the pa1 pre-cleanup lexer tool. Changed to `"\n"`
   (byte-identical; `cout` flushes at exit): 12,156 writes, 4.11s → 1.62s.
2. **Nondeterministic long double hexdump (material, fixed)**: the 16-byte
   long double object carries 6 ABI-undefined padding bytes after the 80-bit
   x87 value; they were emitted as uninitialized stack bytes. Fixtures never
   hit dirty stacks, but randomized trials mismatched 45/600 (e.g. `3.14L` →
   `…0040966A0000`, varying run to run). Padding is now zeroed, matching the
   reference's constant `…0040000000000000`.
3. **Floating overflow scanned to max-finite instead of infinity (material,
   fixed)**: C++11 `num_get` stores the largest finite value on overflow, so
   the starter-kit istringstream decode produced `DBL_MAX`/`FLT_MAX` where
   the reference produces ±inf (`1e309` → `000000000000F07F`). `PA2Decode_*`
   now use `strtof/strtod/strtold`, bit-identical in range and infinity on
   overflow. New fixture `300-floating-overflow-to-infinity` pins overflow,
   denormal-underflow, and max-finite bytes.
4. **Lone `_` ud-suffix rejected (fixed)**: `IsUDSuffix` required length ≥ 2;
   the reference accepts `1.2_`, `123_`, `0x1_`, `"abc"_`, `'a'_`, `""_` (a
   ud-suffix is any identifier starting `_`), and `operator ""_` is therefore
   a user-defined literal, not a reserved-suffix split.
5. **Non-adjacent `ll` accepted (fixed)**: `1lul`/`1LuL` classified as
   `unsigned long long`; the 2.14.2 ll-suffix is two adjacent same-case
   characters and the reference rejects. Both 4 and 5 are pinned by new
   fixture `300-underscore-suffix-ll-adjacency`.
6. **Parallel-array sequence state (fixed)**: the pending string sequence
   held `vector<uint32_t>` code points plus a `vector<unsigned char>` numeric
   flags shadow; one `vector<StringSequenceElement>` now owns the fact and
   the defensive size-equality check is gone.
7. **Per-token heap allocation in integer classification (fixed)**: each
   integer literal built a `vector<IntegerCandidate>`; candidates are now
   pointers into static ordered lists (a rank suffix of the signed-decimal,
   unsigned, or either-sign list). Emission also no longer re-parses the
   integer suffix or copies the body substring.
8. **Duplicated escape scanners and value-render switches (fixed)**:
   character- and string-literal paths each carried a hex/octal scanner
   differing only in overflow policy — unified into one `ParseEscape` with
   the policy at the owning call sites — and three type→bytes dispatch
   switches/templates collapsed into `EncodeScalar`. Net −225 lines in
   `posttoken_stream.cpp` with behavior byte-identical on the full corpus.

## Performance evidence

Tool wall time (release build, output to /dev/null): 100k-line mixed-token
input (1.8M tokens, 18 MB) 1.62s / 200k lines 3.24s — linear, was 4.11s/8.31s
before the flush fix. String concatenation: one 100k-element sequence 0.06s,
7.4 MB RSS / 200k elements 0.13s, 11.4 MB RSS — linear, no re-encode per
element pair. RSS on bulk input (21.6/39.6 MB for 18/37 MB inputs) is the
buffered source plus bounded per-token state; the only unbounded buffer is
the pending string sequence, proportional to its own source. Integer path
does no per-token heap allocation; table lookups are single hash-map hits.

## Conformance validation

- `make test-report-through-pa2`: 81/81 (53 pa1 + 18 pa2 local + 10 course);
  file audit: 18 files pass.
- Bulk differential: 100k-line/1.8M-token input byte-identical to
  `posttoken-ref` (stdout and exit status).
- Randomized differential: 2,200 seeded trials across three fragment pools
  (numeric limits, octal/hex escapes, UCNs, raw strings, concat sequences,
  `operator ""` shapes, malformed pp-numbers) — 0 mismatches after the fixes
  above (before: 45/600 long-double padding, 153/800 overflow-to-infinity).
- Reference probes recorded at CP2/CP3 (empty `''x`, singleton-only
  `operator ""` split, state reset) remain valid; no reference quirks were
  found that conflict with the handout or standard, so no divergence envelope
  is carried for pa2.

## Checkpoint ledger

| id  | scope                                              | proof | status |
|-----|----------------------------------------------------|-------|--------|
| CP1 | classifier skeleton, routing, full pp-number path  | 15/26 pa2; 53/53 through-pa1; file audit pass | DONE |
| CP2 | shared unicode extraction + character literals     | 17/26 pa2; 53/53 through-pa1; file audit pass | DONE |
| CP3 | string sequences, deferred encoding, operator split | 26/26 pa2; 79/79 through-pa2; 100k-concat probe linear | DONE |
| CP4 | final architecture cleanup + divergence audit      | 81/81 with 2 new boundary fixtures; flush, long-double, overflow, `_`-suffix, `ll`-adjacency fixes; fuzz + bulk diff clean | DONE |
