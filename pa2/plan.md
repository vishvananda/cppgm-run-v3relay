# PA2 Plan — posttoken (pp-token → token classification) [COMPLETE]

Final state: 81/81 fixtures pass through `make test-report-through-pa2`
(53 pa1 + 18 pa2 local + 10 course, including two boundary fixtures added at
CP4); file audit passes. Architecture review, findings, performance evidence,
and conformance validation are consolidated in `pa2/audit.md`.

## Stage Design (as built)

Owning boundary: the `classified tokens` stage of the one production pipeline
(spec.md §1). PA2 owns conversion of `preprocessing-tokens` into analyzed
`tokens`: keyword/operator mapping, integer/floating classification and
valuation, character/string literal decoding and encoding, string-sequence
concatenation, and user-defined-literal splitting. One production
implementation in `dev/src/`; `dev/posttoken.cpp` is a thin adapter
(stdin → `PPTokenize` → `PostTokenStream` → `DebugPostTokenOutputStream`).

- `dev/src/posttoken_types.h` + `dev/src/posttoken_tables.cpp` —
  `EFundamentalType`, `ETokenType`, and the three `static const` maps (typed
  vocabularies as enums, spec.md §2).
- `dev/src/posttoken_stream.h/.cpp` — `IPostTokenOutputStream` (pure-virtual
  emitter mirror) and `PostTokenStream : IPPTokenStream`. Streaming, O(1)
  state plus one pending string-sequence buffer
  (`vector<StringSequenceElement>`: code point or numeric-escape code unit).
  Grammar classification, bounded `uint64_t` integer valuation against static
  rank-indexed candidate lists, one `ParseEscape` scanner, one
  `EncodeScalar`/`AppendCodeUnit` value→ABI-bytes owner, `PA2Decode_*` via
  `strtof/strtod/strtold`. Hexdump rendering stays in the tool's Debug
  emitter (presentation at the boundary).
- `dev/src/unicode.h/.cpp` — single shared UTF-8 decode/encode and
  scalar-value check, extracted from the lexer at CP2.
- `dev/frontend_source_sets.mk` — posttoken links
  `pptoken_lexer posttoken_stream posttoken_tables unicode`.

Data flow: whitespace/new-line are no-ops; header-names,
non-whitespace-chars, and `#`/`##`/`%:`/`%:%:` are invalid; identifiers and
op-or-punc route through the keyword/operator table; pp-numbers classify once
then emit; character literals decode immediately; (ud-)string-literals
accumulate into the pending sequence, flushed by any non-string token or eof.
Exit status: EXIT_SUCCESS always; only a phase 1–3 lex error yields
EXIT_FAILURE.

## Behavior facts (oracle, fixture- or probe-proven against the reference)

- A ud-suffix is any identifier starting `_`, including a lone `_`
  (`1.2_`, `123_`, `"abc"_`, `'a'_`, `""_` valid); a non-`_` suffix makes the
  whole token or sequence invalid (`123abc`, `"abc"yyy`, `'u'da`).
- `operator ""` split: only a singleton empty ordinary `""` with a non-`_`
  suffix directly after `operator` splits into `literal ""` + identifier
  (`operator""if`); `operator ""_` is a user-defined literal.
- UDL grammar is suffix-exclusive (`2147483647l_ud1` invalid,
  `2147483647_ud1l` valid); integer/floating UDLs emit prefix text, no value
  scan or range check.
- Integer suffixes per 2.14.2: `u` once, `ll` two adjacent same-case chars
  (`1lul`, `1LuL`, `1Ll` invalid; `1llu`, `1uLL` valid); decimal unsuffixed
  stays signed (> LLONG_MAX invalid), octal/hex also try unsigned per rank.
- Floating scans via `PA2Decode_*`; overflow yields infinity (`1e309` →
  `F07F` bytes), underflow denormals; long double hexdumps 16 bytes with the
  6 padding bytes zeroed.
- Character literals: one code point in [0,0xD800) ∪ [0xE000,0x110000),
  numeric escapes included; ordinary ≤ 127 → char else int; u/U/L →
  char16_t/char32_t/wchar_t, invalid if it exceeds one code unit; `''` and
  multi-code-point invalid.
- String sequences: 0/1 distinct encoding prefix (u8 merges with ordinary)
  and 0/1 distinct ud-suffix, else one invalid with space-joined source;
  encoding is deferred until the sequence prefix is known; a numeric escape
  contributes exactly one width-checked code unit (`u"\xD800"` valid,
  `"\x100"` invalid); raw strings encode verbatim per target prefix.

## Checkpoint Ledger

| id  | scope                                              | proof of progress | status |
|-----|----------------------------------------------------|-------------------|--------|
| CP1 | classifier skeleton + routing + full pp-number path | 15/26 pa2; 53/53 through-pa1; file audit pass | DONE |
| CP2 | shared unicode extraction + character literals      | 17/26 pa2; 53/53 through-pa1; file audit pass | DONE |
| CP3 | string sequences, deferred encoding, operator split | 26/26 pa2; 79/79 through-pa2; 100k-concat probe 0.05s | DONE |
| CP4 | final architecture cleanup + divergence audit (`pa2/audit.md`) | 81/81; flush, long-double padding, overflow-to-inf, `_`-suffix, `ll`-adjacency fixes; 2,200-trial fuzz + 1.8M-token bulk diff clean | DONE |

## Handoff

PA2 is complete. Next: packetize PA3 (ctrlexpr) from its assignment contract
before implementation.
