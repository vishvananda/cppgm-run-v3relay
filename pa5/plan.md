# PA5 Plan — preproc (phases 1–6 + phase-7 tokenization) [COMPLETE]

Final state: 260/260 fixtures pass through `make test-report-through-pa5`
(53 pa1 + 28 pa2 + 23 pa3 + 47 + 33 pa4 + 69 pa5 local including six
boundary fixtures added at CP4 + 7 pa5 course); pa5 file audit passes
(25 files). Architecture review, findings, performance evidence, and
conformance validation are consolidated in `pa5/audit.md`.

## Stage Design (as built)

Owning boundary: `dev/src/preproc_engine.h/.cpp` owns source file/line
tracking, logical-line directive dispatch (conditional group stack,
#include frames, #define/#undef delegation, #line, #error, #pragma,
null/non-directive), predefined macros, `_Pragma`, and the per-srcfile
output envelope. It reuses PA1 `PPTokenize` (extended with an optional
`IPPTokenPositionSink`), PA4 `MacroTable`/`MacroExpander` (extended with
token position stamps and a dynamic-macro hook), PA3
`EvaluateControllingExpression`, and PA2 `PostTokenStream`.
`dev/preproc.cpp` is a thin adapter (argv, one `asctime` snapshot,
`PA5GetFileId` injection, exception → exit status). Per spec.md §1 the
engine is the single production implementation of directive processing.

Pipeline per srcfile (fresh table/counter/once-set/output per srcfile):

    read file → PPTokenize(+line sink) → PPTokenCollector, tokens stamped
      (file ordinal, physical line) → NEW_LINE-delimited carve assigns
      presumed (file, line) once (`%:` ≡ `#`) → directive dispatch on the
      group stack | text accumulation → MacroExpander (head stamping +
      dynamic __FILE__/__LINE__/__COUNTER__ hook) → streaming _Pragma
      filter → one PostTokenStream per srcfile → PA2 writer to outfile

Key decisions (reference-pinned; details and probe evidence in audit.md):

- Typed position stamps on `PPToken`; presumed values assigned exactly once
  at the carve; `#line` takes effect on the following physical line;
  replacement tokens take the invocation head's stamp, argument tokens keep
  their own; dynamic predefineds resolve from the candidate's own stamp and
  are disabled by `#undef`.
- Group stack frame {parent_active, taken, in_else, active}; inactive
  sections interpret only the six conditional names but enforce ordering;
  junk-token rules: `#endif` errors iff parent context active, `#else` iff
  the branch becomes active; open group at end of file errors.
- `#include`: sole header-name used directly, else macro-replaced to an
  ordinary string-literal; search presumed-`__FILE__`-relative (pathrel)
  then cwd-relative; the succeeding path becomes the new frame's presumed
  `__FILE__`; frames share srcfile state; depth cap 256.
- Pragma once on the presumed `__FILE__` id (lookup failure errors; `once`
  with trailing tokens errors; unknown pragmas ignored), for both the
  directive and `_Pragma("once")` after all replacement.
- Paint hide-sets (shared core): persistent AVL trees with structural
  sharing plus a hash-consing `PaintAdd` memo — the pa4-recorded upgrade,
  justified by the audit's chain and repeated-argument measurements.
- Errors → EXIT_FAILURE by exception (`PreprocError`/`MacroError`),
  including invalid posttokens (writer override throws).

Files: `dev/src/preproc_engine.h/.cpp` (new); `dev/preproc.cpp`,
`dev/src/macro_replace.h/.cpp`, `dev/src/pptoken_lexer.h/.cpp`,
`dev/src/ctrlexpr_eval.h/.cpp`, `dev/src/posttoken_debug.h`,
`dev/frontend_source_sets.mk` (edited).

## Checkpoint Ledger

- CP1 (DONE) — engine skeleton at the carve/flush boundary: position sink,
  token stamps, logical-line carve, PA4 delegation, static predefineds,
  envelope, persistent paint tree + memo. 47/70 pa5; 184/184 through-pa4.
- CP2 (DONE) — conditional group stack, `defined` pre-resolution, PA3
  wiring, #line, dynamic predefined hook + head stamping. 63/70 pa5.
- CP3 (DONE) — include frames + path search, pragma once, `_Pragma`,
  `%:%:` paste, independent multi-srcfile state. 70/70 pa5.
- CP4 (DONE) — final architecture audit: pragma-once presumed-file model,
  junk-token rules, undef-of-dynamic, streaming `_Pragma` window, text-path
  copy elimination, paint memo A/B, 28 differential probes; six new
  reference-regenerated boundary fixtures. 260/260 through-pa5; probes in
  budget; findings consolidated in `pa5/audit.md`.
