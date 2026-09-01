# PA6 Final Architecture Audit — recog

## Scope and method

Traced representative facts through their full ownership paths: srcfile →
PA5 `PreprocEngine::RunSingleFile` (new per-srcfile seam feeding a
caller-supplied `IPostTokenOutputStream`) → `Pa6TokenCollector` (typed
`Pa6Token` classification at construction, OP_RSHIFT split, ST_EOF) →
`Pa6Parser` recursive descent over `vector<Pa6Token>` (backtracking with
position+bracket restore, bracket/angle context, keyed memo, hard-failure
commitment) → OK/BAD line in `dev/recog.cpp`. Measured scaling on
20k-line declaration/statement and 10k-member inputs, nested-class depth
probes, and the course deep-template canary; ran ~150 directed
differential probes against `recog-ref` (outfile bytes + exit status).

## Ownership trace (clean)

- **Token facts**: `Pa6Token` carries a typed kind, `ETokenType` for
  simple tokens, and a `flags` bitmask computed once at construction:
  mock name-lookup categories (C/T/Y/E/N letters via `NameCategoryMask`,
  the single classification authority), final/override, empty-string and
  zero literal facts. The parser queries only kind/type/flags; the
  spelling is retained for diagnostics and never re-derived from
  (spec §2 typed continuity). The collector splits OP_RSHIFT into
  ST_RSHIFT_1/ST_RSHIFT_2 at emission, throws on `emit_invalid`, and
  appends ST_EOF, so the parser sees exactly the PA6 terminal alphabet.
- **Bracket/angle facts**: one bracket stack owns 14.2.3. `(`/`[`/`{`
  push via `enter_bracket`; `<` pushes BRACKET_ANGLE only at committed
  template points (T-category name + `<`, cast/template keywords).
  `has_angle_boundary` (innermost non-angle wins) gates `>` as relational
  and `>>` halves as shift; `parse_close_angle_bracket` consumes
  OP_GT/either RSHIFT half and pops one level, so `TC1<TC2<6>>` closes
  inner-then-outer. Angle pushes are deterministic per position because a
  T-name followed by `<` commits (hard failure on mismatch, pinned by
  course 500-template-name-angle-commit-bad), which is what makes the
  memo key sound (below).
- **Memo**: `try_memoized` wraps six net-zero-bracket rules
  (simple-template-id, expression, constant-expression, type-id,
  template-argument, decl-specifier-seq) keyed
  `(rule_id, pos, angle_refusal)`; rule ids live in one enum in
  `recog_parser.h` (a duplicated cp2-local constant was removed). The
  angle-refusal bit distinguishes template-argument dynamic extent;
  bracket-stack contents per position are otherwise deterministic (angle
  commitment above), so entries can never go stale. Entries are not
  recorded while `hard_failure_` is set. Memoizing the two backtracking
  families is semantically load-bearing (deep-template and nested-class
  bounds below), which spec §4 classes as mandatory memoization.
- **Envelope**: `dev/recog.cpp` stays a thin adapter — argv, one
  `asctime` snapshot, a fresh `PreprocEngine` + collector + parser per
  srcfile (state cannot leak across files), per-file `catch` → BAD,
  always EXIT_SUCCESS. Five unused handout mock-lookup wrappers were
  removed. The PA5 text-output path is untouched (`RunSingleFile` shares
  `ResetSourceFileState` + the token pipeline with the PA5 writer).

## Findings and changes (this cleanup)

Conformance findings were surfaced by directed differential probes; each
fix is pinned by a fixture generated from `recog-ref` via `make ref-test`.

1. **decl-specifier-seq type commitment missing (material, fixed)**: the
   README-required 7.1.6.2p2 rule (a type-name joins the seq only if no
   previous non-cv type-specifier) was planned but never enforced — the
   greedy specifier loop consumed second type-names, so `Y1 Y2 = 0;`,
   `unsigned Y1 = 0;`, `N1::Y1 Y2 = 0;`, `struct C2 { } C3 = {};` all
   went BAD. The seq loop now stops at an identifier or `::` once a
   committing specifier (anything except cv/storage/function-specifier/
   friend/typedef/constexpr) is present. Fixture 255-decl-type-commit.
2. **declaration-statement dropped most of block-declaration (material,
   fixed)**: block scope rejected `using YB = int;`, `using N1::x;`,
   `using namespace N1;`, `namespace QN2 = N1;`, `asm("nop");`.
   `parse_declaration_statement` now is `parse_block_declaration`, which
   gained an extracted `parse_namespace_alias_definition` (the alias path
   was buried in namespace-definition, unreachable from block scope);
   `parse_declaration` was deduplicated to try each alternative once.
   Two latent restore bugs fixed on the way: alias-declaration and
   linkage-specification returned false with tokens consumed, corrupting
   the position for sibling alternatives (`using namespace N1;` failed
   even at namespace scope because alias ate the `using`).
   Fixture 256-block-declarations.
3. **trailing-return-type never parsed (material, fixed)**:
   `parse_declarator` tried ptr-declarator first and returned on its
   prefix match, so the `noptr-declarator trailing-return-type`
   alternative was unreachable — `auto f() -> int { }` went BAD. The
   merged declarator parse now takes `->` after a ptr-operator-free
   noptr-declarator as a trailing return type (the grammar permits no
   other continuation); `parse_abstract_declarator` gained the analogous
   `noptr-abstract-declarator? trailing-return-type` alternative
   (`using YF = auto (int) -> int;`). Fixture 257-trailing-return.
4. **statement attributes missing (fixed)**: the grammar prefixes every
   non-labeled, non-declaration statement alternative with
   `attribute-specifier*`; `parse_statement` never consumed them, so
   `[[foo]] if (y) ;` went BAD. Fixture 258-statement-attributes.
5. **array-new unparseable (material, fixed)**: `parse_new_declarator`
   called the *declarator* noptr parse (declarator-id-rooted) instead of
   `noptr-new-declarator`, so `new int[5]` went BAD while `new int x`
   was wrongly accepted. Implemented
   `[expr] attr* ([const-expr] attr*)*` per grammar. Fixture
   259-array-new.
6. **Exponential nested-class parse (material, fixed)**: declaration and
   member parsing try function-definition first and re-parse the whole
   decl-specifier-seq (including class bodies) on fallback, doubling each
   nesting level: depth 16/20/22 took 0.55/8.8/35.3 s (×16 per +4 —
   2^depth), so a ~600-byte depth-24 file would blow the 10 s runner cap.
   decl-specifier-seq is now the sixth memoized rule; the fallback
   re-parse is a memo hit and the family is O(positions). Depth 40
   fixture 261-deep-class-nesting runs in 0.00 s (depth 60 probe likewise).
7. **Structure cleanups**: name-category classification moved into token
   construction (was 5 × `string::find` per parser query on the hot
   backtracking path; the parser now tests flag bits); memo rule-id enum
   unified in the header; dead `hard_before` in `try_memoized`, dead
   `is_type_start`, an unreachable member bitfield disjunct, and the
   unused `seen_type` out-parameter removed; obfuscated
   restore-and-reconsume shapes in postfix `.`/`->`, sizeof, return, and
   `operator new/delete` rewritten as straight-line optional parses.

## Performance evidence

Release build, `/usr/bin/time`, interleaved runs, medians of 3, final
binary vs `recog-ref` on identical inputs:

- 20k mixed declarations: 0.41 s / 100 MB (ref 0.27 s / 26 MB).
- 20k-statement function: 0.59 s / 134 MB (ref 0.34 s / 32 MB).
- 10k-member class: 0.19 s / 46 MB (ref 0.17 s / 14 MB).
- Scaling is linear: 2× input → 2.0–2.05× time on all three families.
- Deep-template canary (course 500, ~96 nested failing `TC1<`): 0.00 s.
  Nested-class depth 60: 0.00 s / 4 MB (was unreachable pre-fix).
- Time stays within ~1.7× of the reference; peak RSS is ~4× driven by
  the per-file memo hash map and per-token `std::string` spellings
  (~5 KB/input line). Graded fixtures are < 2 KB, every test finishes in
  well under 1% of the 10 s runner cap, and both structures are
  per-parser-instance (freed per file). Accepted at stage scale; the
  recorded upgrade before compiler-scale inputs is an arena/dense memo
  keyed by (rule, pos) pages and interned spellings (spec §4 measured
  exception).

## Conformance validation

- `make test-report-through-pa6`: 313/313 (260 through-pa5 unchanged +
  38 pa6 local including the six new fixtures + 15 pa6 course);
  pa6 file audit passes (one pre-existing non-fatal header-division
  warning on the parser class declaration).
- ~150 directed differential probes against `recog-ref`: decl-specifier
  commitment shapes, block-scope declarations, trailing-return forms,
  statement attributes, member declarators/bitfields, base clauses,
  templates/specializations, angle-bracket forms, new/delete, lambdas,
  exception specs — all match except the documented envelope below.
- **Divergence envelope (recorded, deliberate)**: `recog-ref` applies an
  undocumented junk-tolerant recovery and accepts many inputs its own
  published grammar rejects — e.g. `foo bar baz;`, `int c = ;`,
  `if (x) ;` at namespace scope, `int c : 2;`/`int c = x : 2;` outside
  members, unbalanced `int c ) ;`, T-only template parameters used as
  type-names (`template <class TP> TP f(TP p)`), and `C1<int>` heads
  without the T category. Our recognizer stays faithful to `pa6.gram` +
  the README's mock-lookup and 14.2.3/7.1.6.2 rules and reports BAD;
  conversely we accept the grammar-listed `[[foo]] { }` statement that
  the reference rejects. The README's matching obligation is limited to
  the provided suite, which passes in full; replicating recovery
  behavior would pollute the parser that pa7+ extends.

## Checkpoint ledger

| id  | scope                                                    | proof | status |
|-----|----------------------------------------------------------|-------|--------|
| CP1 | engine seam, typed terminal layer, recog envelope, parser infra (backtracking, bracket/angle context, memo), expression/statement/simple-declaration spine | 34/47 pa6 from 0/47; through-pa5 260/260; deep-template 0.009 s; audit pass | DONE |
| CP2 | declaration breadth: class/enum/namespace/using/linkage/asm/template/operator-ids/exceptions/lambda | 47/47 pa6; through-pa5 260/260; deep-template 0.07 s | DONE |
| CP3 | final audit: decl-specifier commitment, block-declaration statements, trailing return, statement attributes, array-new, decl-specifier-seq memo (exponential fix), token-flag classification, dedup/dead-code cleanups | 313/313 with 6 new fixtures; ~150 probes clean modulo recorded envelope; linear scaling evidence | DONE |
