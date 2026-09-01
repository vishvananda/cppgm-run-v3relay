# PA5 Plan — preproc (phases 1–6 + phase-7 tokenization)

Target: 70/70 pa5 fixtures (63 `pa5/tests` + 7 `pa5/course/pa5`) via root
`make test-pa5`, then clean `make test-report-through-pa5` (184 prior + 70).
Current: 0/70 — `dev/preproc.cpp` is the untouched starter stub; every fixture
fails with EXIT_NOT_IMPLEMENTED. Through-pa4 is green (184/184).

## Stage Design

Owning boundary: a new engine `dev/src/preproc_engine.h/.cpp` owns source
file/line tracking, logical-line directive dispatch (conditional group stack,
#include frames, #define/#undef delegation, #line, #error, #pragma,
null/non-directive), predefined macros, `_Pragma`, and the per-srcfile output
envelope. It reuses PA1 `PPTokenize` (extended with an optional position
sink), PA4 `MacroTable`/`MacroExpander` (extended with token stamps and a
dynamic-macro hook), PA3 `EvaluateControllingExpression` +
`CtrlExprTokenCollector`, and PA2 `PostTokenStream`. `dev/preproc.cpp` stays a
thin adapter (argv, `PA5GetFileId` injection, exception → exit status). Per
spec.md §1, the engine is the single production implementation of directive
processing; the tool is an adapter around shared models.

Pipeline per srcfile (fresh table/counter/once-set/output per srcfile):

    read file → PPTokenize(+line sink) → PPTokenCollector, tokens stamped
      (file ordinal, presumed line) → NEW_LINE-delimited logical-line carve
      (`%:` ≡ `#`) → directive dispatch on group-stack | text accumulation
      → MacroExpander (head stamping + dynamic hook) → _Pragma filter
      → one PostTokenStream per srcfile → PA2 writer to outfile
    "preproc N" header; "sof <argv name>" per srcfile; emit_eof → "eof".

Reference-pinned decisions:

- Position model: `PPToken` gains typed (src_file ordinal, src_line) fields.
  `PPTokenize` gains an optional `IPPTokenPositionSink*` parameter reporting
  each token's physical line (first character; count raw newlines including
  spliced and in-comment ones). Existing two-arg callers are unchanged. The
  engine converts physical→presumed at collection using per-frame `#line`
  offset + presumed-name state, so tokens carry final presumed values.
- `#line N ["file"]`: line is macro-replaced first; sets the presumed line of
  the physical line after the directive's terminating newline (NEW_LINE
  token's own physical line + 1) — pinned by 660-line-directive and course
  300-line-new-line (splices inside the directive).
- Expansion stamping: tokens produced by replacing an invocation take the head
  token's (file,line); argument tokens keep their own stamps and pre-expand
  with themselves as head — pinned by 600-line-macro (`line2(…\nline)` → 15,
  not the head line 11) and 610-line-macro.
- `__FILE__`/`__LINE__`/`__COUNTER__` resolve via a `MacroExpander` hook
  consulted when a candidate identifier is examined, using the candidate's own
  stamp; `__COUNTER__` increments per replacement, resets per srcfile
  (400-counter-macro: value0 value1). Fixed predefineds — `__CPPGM__ 201303L`,
  `__cplusplus 201103L`, `__STDC_HOSTED__ 1`, `__CPPGM_AUTHOR__` (author
  string), `__DATE__`/`__TIME__` from one `asctime` call at main entry — are
  ordinary table defines installed per srcfile. Fixtures only #ifdef-test
  DATE/TIME/AUTHOR (no value comparison).
- `#if`/`#elif`: resolve `defined X` / `defined(X)` to 0/1 pp-numbers BEFORE
  macro expansion; the operand may be an identifier-like alternative token
  (`and`, `or_eq`, … — course 200-defined-identifier-like-operator). Then
  expand, posttokenize via `CtrlExprTokenCollector`, evaluate with
  is_defined = table ∪ {__FILE__,__LINE__,__COUNTER__}. PA3 error result in an
  active context → EXIT_FAILURE (170-nondir6).
- Group stack (170-nondir1/4/5/6): frame = {parent_active, taken, in_else,
  active}. Inactive sections interpret only the six conditional names and
  ignore everything after them; `#elif` after `#else` errors even when
  inactive; skipped controlling expressions are never evaluated; `#`
  non-directives error only when active (170-nondir2/3 vs 170-nondir1).
  An open group at end of any file is an error — pinned by 400-bad-include
  (bad-include.h is exactly `#if 1`).
- `#include`: if the sole token is a header-name, use it; otherwise
  macro-replace and posttokenize to an ordinary string-literal → `nextf`.
  `pathrel` = presumed `__FILE__` up to last '/' + `nextf` (only when
  `__FILE__` contains '/'); try pathrel then nextf (cwd-relative; preproc
  runs from pa5/). Presumed `__FILE__` of the new frame = the path that
  succeeded; neither exists → EXIT_FAILURE. Recurse into a new frame sharing
  srcfile state (table, counter, once-set, PostTokenStream) — pinned by
  200-include (pathrel precedence) and 400-header-guarded.
- `#pragma once`: record file id (injected `PA5GetFileId` callback) of the
  current frame's file; each #include first resolves its target path and
  skips if its id is in the set — pinned by 800-pragma-once (same file via
  different spellings). `#pragma cppgm_mock_unknown …` and unknown pragmas
  are ignored.
- `_Pragma`: filter on the expander's output stream (text sequences, after
  all replacement): identifier `_Pragma` must be followed by `(`
  string-literal `)` (wide accepted — 501-pragma-op-ignore) else error;
  invocation tokens removed; destringized content executed as pragma in the
  current frame (600-pragma-op: `_Pragma("once")` built by `#`/`##` inside
  the include marks the include file).
- Digraphs: carve treats `%:` as `#`; the expander treats `%:%:` as `##`
  (500-alt: `%:define foo ba %:%: z` → `baz`). `%:` occurs nowhere in
  macro_replace.cpp today — new equivalence at the IsData/paste layer.
- Errors → EXIT_FAILURE by exception (`MacroError` + new `PreprocError`):
  invalid posttoken in output (120-invalid; writer subclass overrides
  emit_invalid to throw — PA5 tightens PA4's emit-and-continue), active
  `#error`, active non-directive, ctrl-expr error, include failure, `#undef`
  extra tokens, `_Pragma` misuse. Outfile state on failure is undefined.
- Output writer: parameterize `DebugPostTokenOutputStream` with
  `std::ostream&` (default `std::cout`; posttoken/macro tools unchanged).
- Harness facts: cwd = pa5/; args are `-o <out>` then all files matching
  `<test>.t*` sorted (course 400-multiple-source-files passes .t + .t2);
  `.ref` compares the outfile; failing tests compare exit status only;
  `--batch-stdin` stub stays as-is (harness default path is non-batch).

Files: new `dev/src/preproc_engine.h/.cpp`; edits: `dev/preproc.cpp`,
`dev/src/macro_replace.h/.cpp`, `dev/src/pptoken_lexer.h/.cpp`,
`dev/src/posttoken_debug.h`, `dev/frontend_source_sets.mk`
(`FRONTEND_OBJ_BASENAMES_preproc := preproc_engine macro_replace
ctrlexpr_eval pptoken_lexer posttoken_stream posttoken_tables unicode`).

## Failure Map

At CP1 start all 70 fixtures failed with EXIT_NOT_IMPLEMENTED (stub throw);
after CP1, 47/70 pass. By owning layer:

- Envelope + PA4 replay (~43): 100-empty, 100-nodefs, 120-invalid, 150-max,
  150-null-directive, 170-nondir2/3, 200-fnlike, 200-onedef, 250-badvargs1–4,
  250-join, 300-badhash1-1..3, 300-badhash2-1..4, 300-identifier-missing,
  300-undef-extra, 500-tricky-join, 600/650/900-recurse, 910-recurse2,
  700-redef-a/q/2, 700-redeferr1–4, 700-strlit-a/a2/q, 800-placemarker-a/q,
  850-varargs-a/q, course 600-repeated-argument-expansion.
- Conditionals + predefined + #line (~17): 200-if, 150-error, 150-no-error,
  170-nondir1/4/5/6, 400-predefined-macros, 500-predefined-macros,
  600/610-line-macro, 660-line-directive, 400-counter-macro, course
  200-conditional-exclusion, 200-defined-identifier-like-operator,
  300-defined-operator-misuse, 300-line-new-line,
  600-predefined-macro-argument-expansion.
- Includes + pragmas + digraphs + multi-file (~10): 200-include,
  400-bad-include, 400-header-guarded, 800-pragma-once, 500-alt,
  500-pragma-ignore, 501-pragma-op-ignore, 600-pragma-op, course
  400-multiple-source-files.

## Performance Risks

- Repeated argument expansion (course 600, ref 0.68s): keep PA4's memoized
  lazy per-parameter pre-expansion; never re-expand an argument per use and
  never route pre-expansion through a fresh engine pass.
- Stamps are two ints per token; file names interned as ordinals in a
  per-run table — no per-token strings, no textual downgrades (spec.md §2).
- Include recursion: one collector per include frame; cap depth (256) so a
  self-including header without a guard errors instead of exhausting memory.
- One PostTokenStream per srcfile keeps phase-6 concatenation linear and
  cross-include.
- Probes: `time ./preproc -o /dev/null course/pa5/600-repeated-argument-expansion.t`
  (budget ≤2s, ref 0.68s) and pa4/audit.md's 100k-expansion input through
  `pa5/preproc-stdin` (budget ≈ pa4 baseline 1.85s ±35%).

## Checkpoint Ledger

- CP1 (COMPLETE) — engine skeleton at the carve/flush boundary: position sink,
  token stamps, logical-line carve, PA4 define/undef/text delegation, static
  predefineds, null directive + non-directive error, `%:` directive start,
  outfile envelope, invalid→error, adapter rewrite. Progress proof: the ~43
  envelope+replay fixtures flip from EXIT_NOT_IMPLEMENTED to pass;
  `make test-pa5` is 47/70 with the remaining 23 in CP2/CP3 scope;
  through-pa4 is 184/184; file audit is 25/25; the repeated-argument probe is
  0.10s / 26640 KB.
- CP2 (COMPLETE) — conditional group stack, defined pre-resolution, PA3
  evaluation wiring, #line, dynamic __FILE__/__LINE__/__COUNTER__ hook +
  head stamping. Proof: 63/70 `make test-pa5` with only the seven CP3
  include/pragma fixtures failing; through-pa4 is 184/184; file audit is
  25/25; the repeated-argument probe is 0.11s / 27980 KB (within the ≤2s
  budget).
- CP3 — #include frames + path search, pragma once + #pragma, _Pragma
  filter, `%:%:` paste, multi-srcfile loop. Proof: remaining ~10 fixtures;
  70/70 `make test-pa5`; clean `make test-report-through-pa5`.
- CP4 — architecture audit + cleanup, perf probes, differential spot-checks
  against preproc-ref, optional boundary fixtures under
  `cppgm.tests/course/pa5` (ref regeneration per TESTING_AND_REFERENCES.md),
  findings consolidated in `pa5/audit.md`. Proof: 70/70 + through-pa5 clean +
  audit + probes in budget.

## Active Checkpoint — CP3

Deliverable: include frames and path search, pragma-once identity, `_Pragma`
filtering, `%:%:` handling, and independent command-line source-file state,
while preserving the completed CP2 location and conditional semantics.

### Implementation Packet

1. `dev/src/preproc_engine.{h,cpp}`: add include frames, quoted/header path
   search, recursive token processing, pragma-once file identity, and
   command-line source-file isolation around `ProcessSourceFile` and
   `ProcessTokens`.
2. `dev/src/macro_replace.{h,cpp}`: preserve the CP2 expansion/location
   contract while handling `%:%:` and filtering `_Pragma` tokens at the
   macro-rescan boundary.
3. Prove the current failures flip: `200-include`, `400-header-guarded`,
   `500-alt`, `500-pragma-ignore`, `501-pragma-op-ignore`,
   `600-pragma-op`, and `800-pragma-once`, plus the CP2 and CP1 sets.

Proof target: 70/70 `make test-pa5`, clean through-pa5, file audit, and the
CP2 repeated-argument probe within its existing budget.
