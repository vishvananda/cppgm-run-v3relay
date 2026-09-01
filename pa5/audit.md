# PA5 Final Architecture Audit — preproc

## Scope and method

Traced representative facts through their full ownership paths: srcfile →
`PPTokenize` (+`IPPTokenPositionSink` physical-line reporting) →
`PPTokenCollector` (typed position stamps) → NEW_LINE-delimited carve with
physical→presumed conversion → directive dispatch on the conditional group
stack and include frames (`PreprocEngine::ProcessTokens`) → PA4
`MacroTable`/`MacroExpander` (head stamping, dynamic-predefined hook) →
streaming `_Pragma` filter → one `PostTokenStream` per srcfile → PA2 writer.
Measured scaling on the course repeated-argument fixture, a 100k-line
invocation-heavy input, and 200/400/800-deep paint chains; A/B-measured the
paint hash-consing memo; ran 28 directed differential probes against
`preproc-ref` (output bytes + exit status).

## Ownership trace (clean)

- **Position facts**: `SourceDecoder` counts every raw newline once (spliced
  and in-comment included) and reports each token's first-character physical
  line through the optional sink; existing two-arg `PPTokenize` callers are
  untouched. `PPToken` carries typed `(src_file, src_line, presumed_file,
  presumed_line)` ints; file names are interned per srcfile
  (`RegisterFileName` ordinals) — no per-token strings. Presumed values are
  assigned exactly once, at the carve: per-line offsets handle intra-line
  splices, `#line` overrides take effect on the following physical line, and
  blank lines advance via the next-physical-line delta. Expansion stamping:
  replacement copies take the invocation head's stamp, argument tokens keep
  their own, paste results take the non-placemarker operand's stamp.
  `__FILE__`/`__LINE__`/`__COUNTER__` resolve through the `MacroTable`
  dynamic hook from the candidate token's own stamp. The include, `#line`,
  and pragma-once machinery reads presumed state as typed ints — the
  `__FILE__` macro is a projection of that state, not its owner (which is
  why `#undef __LINE__` can disable the macro without touching line
  control).
- **Directive layer**: `ProcessTokens` is the only judge of line-initial
  `#`/`%:`. Text lines append straight from the token buffer with stamps
  applied in place; only directive lines materialize a stamped copy. Group
  stack frame = `{parent_active, taken, in_else, active}`; inactive sections
  interpret only the six conditional names; `#elif`-after-`#else` ordering
  is enforced even when inactive; controlling expressions are evaluated only
  when reachable; junk-token rules are reference-pinned (`#endif`: error iff
  the parent context is active; `#else`: iff the branch becomes active). An
  open group at end of any file errors. `#define`/`#undef` delegate to the
  PA4 `MacroTable` (single authority); `#line` macro-replaces then parses a
  digit-only pp-number plus optional string (interned); `#include` uses a
  sole header-name directly, otherwise macro-replaces to an ordinary
  string-literal; search is presumed-`__FILE__`-relative (`pathrel`) then
  cwd-relative; the succeeding path becomes the new frame's presumed
  `__FILE__`; frames share srcfile state and a depth cap of 256 turns
  unguarded self-inclusion into an error instead of memory exhaustion.
- **Pragma once**: the README's "add the file id of `__FILE__`" is
  implemented literally — the presumed `__FILE__` string is looked up
  as-is, a failed id lookup is an error, and the id set is consulted per
  resolved include target. `#pragma once junk` and `_Pragma("once junk")`
  error; unknown pragmas (including `#pragma` alone and
  `cppgm_mock_unknown`) are ignored. `_Pragma` is recognized only after all
  replacement, in a streaming four-token window that removes the invocation
  tokens; the destringized pragma executes against the presumed file of the
  flushed batch (all text between flushes shares one presumed `__FILE__`
  because `#line` forces a flush).
- **Emission**: one `PostTokenStream` per srcfile spans all its includes, so
  phase-6 string concatenation crosses include boundaries; `sof`/`eof`
  envelope and the `preproc N` header are engine-owned;
  `PreprocPostTokenOutputStream::emit_invalid` throws (PA5 tightens PA4's
  emit-and-continue). `dev/preproc.cpp` stays a thin adapter: argv, one
  `asctime` snapshot at entry for `__DATE__`/`__TIME__`, `PA5GetFileId`
  injection, exception → `EXIT_FAILURE`.

## Paint representation (shared-core change, measured)

pa4 recorded sorted `vector<string>` hide-sets as a measured exception with
an upgrade obligation "when the shared core faces preproc-scale inputs".
CP1 delivered that upgrade: persistent AVL trees
(`shared_ptr<const PaintTreeNode>`, structural sharing, O(1) token copy)
plus a hash-consing memo on `PaintAdd` keyed by (parent node identity,
name), so equal paint sets stay pointer-identical and
`PaintUnion`/`PaintIntersect` hit identity fast paths.

- Paint chains (n-deep object chain invoked 500×), n=200/400/800:
  0.06/0.14/0.32 s vs pa4's vectors 1.34/4.93/18.7 s and the reference's
  0.27/0.76/2.90 s — the recorded O(n²)-constant pathology is now ~9×
  faster than the reference and scales near-linearly.
- Memo A/B (cache removed, rebuilt): course repeated-argument fixture
  0.11 s → 164.5 s (~1500×), chains 2.3× slower with 3–9× the RSS. The memo
  is load-bearing, its keys are immutable value identities (entries can
  never go stale), and lookups never observe iteration order. Its
  process-lifetime growth is accepted for a tool that runs once per
  invocation; the recorded upgrade before multi-TU compiler use is arena
  nodes + interned name ids with per-run reset.
- These measurements are the spec §4 justification for `shared_ptr` nodes
  and per-node strings in this hot record.

## Findings and changes (this cleanup)

All conformance findings were surfaced by directed differential probes and
are pinned by fixtures regenerated from `preproc-ref` via `make ref-test`.

1. **Pragma once marked the wrong owner (material, fixed)**: the id of the
   physical frame file was recorded and a failed lookup silently skipped.
   The README and reference pin the presumed model: mark presumed
   `__FILE__`, error when its id lookup fails (a `#line` rename to a
   missing path fails; a rename to another real file marks *that* file, so
   a later include of it is skipped while the physical file is not marked
   — reference output `A A` on the decisive probe). Fixtures
   530-pragma-once-rename-error, 540-pragma-once-presumed.
2. **`once` with trailing tokens accepted (material, fixed)**:
   `#pragma once junk` and `_Pragma("once junk")` were ignored as unknown
   pragmas; the reference errors on both while still ignoring genuinely
   unknown pragmas. Fixtures 545-pragma-once-junk, 550-pragma-op-once-junk.
3. **`#endif` junk rule checked the wrong flag (material, fixed)**: extra
   tokens errored iff the just-closed branch was active; the reference
   errors iff the group's *parent* context is active (`#if 0 … #endif junk`
   at top level errors; the same nested inside an inactive region does
   not). Fixture 520-endif-junk.
4. **`#undef` of a dynamic predefined was inert (fixed)**: the dynamic hook
   resolved `__LINE__`/`__FILE__`/`__COUNTER__` regardless of `#undef`
   (standard-UB, course-defined by the reference: undef disables them).
   The engine now records such undefs per srcfile and the hook goes silent;
   typed line/include state is unaffected. Fixture 560-undef-dynamic.
5. **Structure and allocation cleanups**: text lines no longer materialize
   a stamped per-line vector before a second copy into the text buffer
   (append + stamp in place); the `_Pragma` filter streams through a
   four-token window instead of buffering the entire expanded text
   (100k-line probe peak RSS 292 → 206 MB, 1.61 → 1.51 s); a dead
   `elif` re-check, an unused two-arg `MacroFlushText` overload, and an
   unreachable trailing-text fallback (the lexer guarantees a final
   NEW_LINE) were removed; dispatch indentation normalized.

## Performance evidence

Release build, output to `/dev/null`, `/usr/bin/time`, final binary:

- course 600-repeated-argument-expansion: 0.11 s / 28 MB (plan budget ≤2 s;
  reference 0.68 s).
- 100k-line invocation-heavy input (pa4 probe shape, `#`/`##`/stringize per
  line): 1.51 s / 206 MB vs the pa4 macro baseline 1.85 s / 176 MB — inside
  the ±35% budget and faster despite added position stamping and directive
  carving. The reference streams at 0.99 s / 8 MB; whole-file token
  buffering (~2 KB/line here) remains the accepted stage-scale design, and
  a streaming carve is the recorded upgrade if later stages need flat
  memory.
- Paint chains as above (near-linear; envelope strictly better than the
  reference).
- No accidental quadratics: the carve is a single pass with per-line-linear
  work, expansion prepends to the deque, text accumulates by append, one
  `PostTokenStream` per srcfile keeps phase-6 concatenation linear across
  includes.

## Conformance validation

- `make test-report-through-pa5`: 260/260 (53 pa1 + 28 pa2 + 23 pa3 +
  47 + 33 pa4 + 69 pa5 local including the six new boundary fixtures +
  7 pa5 course); pa5 file audit: 25 files pass.
- 28 differential probes against `preproc-ref` — `#line`/`__FILE__`/
  `__LINE__` interplay (splices, blank lines), pragma-once rename/presumed/
  junk shapes, conditional junk-token placement across
  active/taken/inactive groups, `defined` produced by expansion,
  `__COUNTER__` across includes, macro-formed includes, `_Pragma` operator
  forms, and `#undef` of predefineds — all match output bytes and exit
  status after the fixes.
- Unguarded self-inclusion errors at depth 256 instead of exhausting
  memory.
- Divergence envelope: only pa4 audit item 5 (a directive line during
  active function-like argument collection), unchanged and still
  README-faithful.

## Checkpoint ledger

| id  | scope                                                    | proof | status |
|-----|----------------------------------------------------------|-------|--------|
| CP1 | engine skeleton: position sink, token stamps, carve, PA4 delegation, predefineds, envelope, persistent paint tree + memo | 47/70 pa5; 184/184 through-pa4; audit pass | DONE |
| CP2 | conditional group stack, `defined` pre-resolution, PA3 wiring, `#line`, dynamic `__FILE__`/`__LINE__`/`__COUNTER__` hook | 63/70 pa5; probes in budget | DONE |
| CP3 | include frames + path search, pragma once, `_Pragma`, `%:%:`, independent multi-srcfile state | 70/70 pa5; 184/184 through-pa4 | DONE |
| CP4 | final audit: pragma-once presumed model, junk rules, undef-dynamic, streaming `_Pragma`, copy elimination, paint memo A/B | 260/260 with 6 new fixtures; 28 differential probes clean; probes in budget | DONE |
