# PA2 Plan — posttoken (pp-token → token classification)

Target: 26/26 pa2 fixtures (16 `pa2/tests` + 10 `cppgm.tests/course/pa2`) and a
clean `make test-report-through-pa2`, with through-pa1 (53/53) and the file
audit green at every checkpoint.

## Stage Design

Owning boundary: the `classified tokens` stage of the one production pipeline
(spec.md §1). PA2 owns the conversion of `preprocessing-tokens` into analyzed
`tokens`: keyword/operator mapping, integer/floating classification and
valuation, character/string literal decoding and encoding, string-sequence
concatenation, and user-defined-literal splitting. One production
implementation in `dev/src/`; `dev/posttoken.cpp` stays a thin adapter
(stdin → `PPTokenize` → `PostTokenStream` → `DebugPostTokenOutputStream`).

- `dev/src/posttoken_types.h` + `dev/src/posttoken_tables.cpp` — move
  `EFundamentalType`, `ETokenType`, and the three starter maps out of the tool
  file so later stages reuse them (typed vocabularies as enums, spec.md §2).
- `dev/src/posttoken_stream.h/.cpp` — `IPostTokenOutputStream` (pure-virtual
  mirror of the starter Debug emitters: invalid/simple/identifier/literal/
  literal-array/udl-{integer,floating,character,string}/eof) and
  `PostTokenStream : IPPTokenStream`. Streaming, O(1) state plus one pending
  string-literal sequence buffer. Value semantics (PA2Decode float scanning,
  integer accumulation, escape decoding) live here; hexdump rendering stays in
  the tool's Debug emitter (presentation at the boundary).
- `dev/src/unicode.h/.cpp` (CP2) — single shared UTF-8 decode/encode,
  UTF-16/32 encode, and code-point validity check, extracted from
  `pptoken_lexer.cpp` so there is one production Unicode implementation.
- `dev/frontend_source_sets.mk` — `FRONTEND_OBJ_BASENAMES_posttoken :=
  pptoken_lexer posttoken_stream posttoken_tables` (+ `unicode` in CP2, also
  added to the pptoken list when extracted).

Data flow inside `PostTokenStream`:
1. `emit_whitespace_sequence`/`emit_new_line` are no-ops. `emit_header_name`,
   `emit_non_whitespace_char`, and op-or-punc `#`/`##`/`%:`/`%:%:` → invalid.
2. Identifier → keyword table hit = simple, miss = identifier. Op-or-punc →
   table hit = simple, miss = invalid.
3. `emit_pp_number` → grammar classifier (below) → literal / udl / invalid.
4. Character literals decode immediately (one pp-token = one token).
5. (ud-)string-literals accumulate into the pending sequence; any non-string
   token or eof flushes the sequence first, then processes itself. eof then
   emits `eof`. Exit status: EXIT_SUCCESS always; only a phase 1–3 lex error
   (`PPTokenize` throw) yields EXIT_FAILURE, as in pptoken.
6. The classifier tracks whether the token emitted immediately before the
   pending sequence began was `KW_OPERATOR` (for the reserved-suffix split).

## Fixture-proven behavior facts (oracle, verified against refs/ref binary)

- Course reserved-suffix rule: a ud-suffix on any literal must begin with `_`,
  else the whole token (or whole string sequence) is invalid: `123abc`,
  `"abc"yyy`, `'u'da`, `"abc"é` → invalid; `1.2_`, `123_e3`, `0x12_p3`,
  `"abc"_é` → valid.
- `operator ""` split: when the previous token is `KW_OPERATOR` and the string
  sequence is exactly one empty ordinary `""` with a non-underscore suffix,
  emit `literal "" array of 1 char 00` then `identifier <suffix>` (even for
  keyword-spelled suffixes: `operator""if` → `identifier if`). `operator"a"sv`,
  `operator u8""sv`, and standalone `""sv` → invalid.
- UDL grammar is suffix-exclusive: integer-suffix then ud-suffix is invalid
  (`2147483647l_ud1` invalid, `2147483647_ud1l` valid with suffix `_ud1l`).
  Integer/floating UDLs emit the prefix as text, no value scan, no range check
  (`29990000000000000000000_id1`, `3.14e+20000_id2` valid).
- pp-number classification: try integer-literal then floating-literal grammar;
  the remainder after the literal body must be a complete integer/floating
  suffix or an identifier-shaped ud-suffix starting `_` (no `.`, no exponent
  sign). Otherwise invalid: `2.2.2`, `1e-`, `1..`, `1_e+`, `164e._`,
  `1e_8e3`, `1.0_foo.bar`.
- Integer types (2.14.2): decimal unsuffixed → int, long, long long (signed
  only; > LLONG_MAX invalid); octal/hex also try unsigned at each level;
  `u` restricts to unsigned, `l`/`ll` set the floor; overflow past the
  candidate list → invalid. int=32, long=long long=64 bits, little-endian.
- Floating (2.14.4): `f/F` float, `l/L` long double, none double, scanned via
  PA2Decode_* (istringstream); overflow to inf is accepted (`1e309` valid).
  long double hexdumps as 16 bytes.
- Character literals: value is a code point; must lie in [0,0xD800) ∪
  [0xE000,0x110000) — numeric escapes included (`'\xD800'`, `u'\xD800'`,
  `'\x110000'` invalid). Ordinary: value ≤ 127 → char, else int
  (`'π'` → int C0030000, `'\xFF'` → int FF000000). u/U/L → char16_t/char32_t/
  wchar_t, invalid if the value doesn't fit one code unit (`u'𝄞'`,
  `u'\x1D11E'` invalid). Multi-code-point (`'abc'`) and empty (`''`, from the
  PA1 lexer's deliberate pass-through) → invalid.
- String sequences: 0 or 1 distinct encoding prefix (repeats fine, `u8` merges
  with ordinary at n≥1... `u8"a" "b" u8"c"` valid; two distinct → invalid) and
  0 or 1 distinct ud-suffix, else the whole sequence is one invalid with
  space-joined source. Encoding is deferred until the sequence prefix is
  known: code points re-encode per target (`"é" u""` → E900 char16_t); a
  numeric escape contributes exactly one code unit checked against the target
  width after prefix resolution (`"\x3C0" u""` valid C003; `"\x100"` alone
  invalid; `u"\xD800"` valid — surrogates allowed as raw code units in
  strings, unlike char literals).
- Raw strings: no escape processing; body code points encode per target
  prefix; `u8R`/`uR`/`UR`/`LR` carry their prefix; source prints verbatim
  including embedded newlines.
- Array type prints code units including the appended terminator:
  `array of <n> <elem>`.
- Sources are PA1 token data: UCNs already decoded to UTF-8 (`'π'`
  prints as `'π'`), escapes not decoded.

## Failure Map (all 26 currently EXIT_NOT_IMPLEMENTED from the stub)

| group | owning sub-feature | fixtures |
|-------|--------------------|----------|
| A | simple/identifier/invalid routing | 100-simple |
| B | integer classification + valuation | 100-integer-zero, 200-basic-integer-suffix, 200-octal-limits, 300-hex-limits, 300-integer-limits |
| C | pp-number grammar incl. floating + UDL split | 200-basic-floating, 300-floating-suffix, 500-plus-ud-suffix, 200-exponent-like-integer-ud-suffix, 300-incomplete-floating-exponent-bad, 300-invalid-floating-literal-shapes, 300-multiple-decimal-points-bad, 300-user-defined-literal-range |
| D | character literals | 200-character-literal, 200-unicode-character-literals |
| E | string sequences, encoding, concat | 250-string-literal, 250-ud-strchar, 400-raw-string, 450-string-literal-concat, 700-hard-string-concat, 200-string-numeric-escape-code-units, 300-string-numeric-escape-out-of-range, 300-invalid-character-string-boundary, 200-unicode-user-defined-literal-suffix |
| F | operator"" reserved-suffix split | 750-reserved-literal-operator-suffix |

## Performance Risks

- Integer valuation must be bounded: accumulate into `unsigned long long`
  with `__builtin_mul_overflow`/`__builtin_add_overflow` (or pre-checks);
  overflow → invalid/next-candidate. No bignum, no retry-per-type rescans.
- String concat must be linear: buffer sequence elements, resolve prefix once,
  encode each element once into one output buffer. No re-encode per element
  pair, no quadratic string appends.
- Classifier is streaming: no whole-input token vector; only the pending
  string sequence buffers (bounded by input size).
- Do not disturb the PA1 lexer's linear scaling; the CP2 unicode extraction
  must be behavior- and complexity-neutral (through-pa1 gate proves it).
- Table lookups are single hash-map hits per token; keep maps `static const`.

## Checkpoint Ledger

| id  | scope | proof of progress | status |
|-----|-------|-------------------|--------|
| CP1 | skeleton: dev/src classifier + tool adapter + build wiring; routing (A) + full pp-number path (B, C) | `make test-pa2`: 15/26 (11 failures, down from 26); `make test-report-through-pa1`: 53/53; file audit: 16 files passed; linearity probe: 2.45s/4.88s for 200k/400k lines | complete |
| CP2 | shared `unicode.h/.cpp` extraction + character literals (D) | `make -C pa2 check` passes for `200-character-literal` and `200-unicode-character-literals`; `make test-pa2`: 17/26 (9 failures, down from 11); `make test-report-through-pa1`: 53/53; file audit: 18 files passed; reference probes confirm empty `''x` invalid, malformed cooked-UCN input fails in phase 3, and the `u` BMP-maximum probe is valid | complete |
| CP3 | string sequences: deferred encoding, concat, operator"" split (E, F) | named local string/raw/concat/operator checks and four supplemental string checks pass; `make -C dev posttoken`; `make test-pa2`: 26/26; `make test-report-through-pa1`: 53/53; `make test-report-through-pa2`: 79/79; file audit: 18 files passed; 100k-element sequence probe: 0.05s, 5580 KB RSS; probes confirm singleton-only `operator""` split and state reset | complete |
| CP4 | architecture cleanup + divergence audit vs ref outside corpus | all green stays green; audit notes recorded | pending |

## Active Checkpoint: CP4 — architecture cleanup and divergence audit

Audit the completed PA2 classifier for structural or specification divergences
outside the checked-in corpus while preserving the green through-stage gates.
Record only evidence-backed cleanup or behavior corrections.

### Implementation Packet

Files/symbols to inspect or touch only when evidence requires it:
- `dev/src/posttoken_stream.cpp` and `dev/src/posttoken_stream.h` — audit
  helper boundaries, pending-sequence ownership, and public protocol stability.
- `pa2/tests` and `cppgm.tests/course/pa2` — use existing fixtures for any
  divergence found; add coverage only for a real semantic boundary.

Required facts:
- Keep the one-pipeline architecture, bounded integer valuation, and linear
  pending-string behavior intact.
- Prefer the handout and standard over an imperfect reference outside checked
  fixtures; error text remains non-semantic.
- Re-run `make test-report-through-pa2` and
  `perl scripts/cppgm_file_audit.pl --stage pa2 --paths dev/src` after any
  source change.
