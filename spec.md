# Compile-Time-First C++11 Compiler Architecture

## Purpose

This specification defines the production architecture of a self-contained,
self-hosting C++11 compiler for Linux x86-64.  `MUST`, `MUST NOT`, `SHOULD`, and
`MAY` are normative.

The compiler has two performance obligations:

1. compile programs with bounded time and memory; and
2. produce code of the quality promised by the selected optimization level.

Compile-time-first means minimizing compiler work within that code-quality
contract.  It does not permit missing language or ABI behavior, and it does not
justify making optimized output unnecessarily slow.

## 1. One production pipeline

The normal source-to-object path MUST be one forward flow:

```text
source buffers
    -> classified tokens
    -> integrated syntax and semantics
    -> canonical semantic facts
    -> typed LowIR
    -> optional typed optimization
    -> compact machine IR
    -> direct native object emission
```

- Each responsibility MUST have one production implementation.  Later stages
  extend the shared core; they MUST NOT introduce a parallel parser, semantic
  analyzer, IR model, or backend for the same responsibility.
- Standalone tools and serialized formats MUST be adapters around the shared
  models.  Tool requirements MUST NOT make ordinary compilation construct an
  unused representation.
- A source producer and an explicit textual-input adapter MUST resolve into the
  same canonical model.  They MUST NOT feed separate compatibility models.
- The production compiler MUST be self-contained.  It MUST NOT invoke a host
  compiler, reference implementation, previous solution, assembler, or linker
  to implement required compilation behavior.

## 2. Typed fact continuity

Once a source fact has been classified, resolved, or decoded, every integrated
downstream phase MUST receive that fact in typed form.

Rendering a typed fact to bytes for storage, hashing, comparison, sorting,
remapping, reclassification, or later recovery is a **textual downgrade**, even
when it happens in memory and no serializer or file is involved.  Production
textual downgrades are forbidden.

- A `StringId` or interned string pointer is presentation identity, not semantic
  identity.  It is appropriate for an arbitrary input spelling, but it MUST NOT
  replace a domain-specific type, declaration, operator, literal, value, block,
  generated entity, ABI, relocation, or object identity.
- Fixed vocabularies MUST use enums.  Generated identities SHOULD use a typed
  kind plus owner/source/ordinal tuple.  Numeric and literal facts MUST retain
  their decoded bits, type, suffix, or arena slice.
- Composite facts SHOULD use typed IDs, tuples, and `(begin, count)` arena
  ranges.  A qualified name MUST NOT be joined and later split when its
  components are already known.
- Each fact MUST have one canonical owner and representation.  Adding a typed
  side index while retaining the old rendering, ownership, hashing, comparison,
  or parse path does not satisfy this requirement.
- Presentation required for diagnostics or an exact dump MUST live in a cold
  sidecar keyed by typed identity, or be rendered on demand.  Object-only
  compilation MUST NOT populate unobserved presentation state.
- IDs crossing independent ownership domains MAY be remapped once per distinct
  identity.  They MUST NOT be rendered and reinterned for every occurrence.

Text remains legitimate at true boundaries: source input, include paths,
preprocessor paste and stringization, diagnostics, requested dumps, assembly
payloads, external symbols, debug strings, and object string tables.  Such a
boundary parses once into typed facts or renders once from them.  Its transient
text indexes MUST NOT become the production core.

## 3. Semantic identity, templates, and demand

- Identifiers, types, declarations, scopes, template arguments,
  specializations, layouts, constants, lifetimes, exception effects, linkage,
  and ABI entities MUST have stable typed identity.
- Equality of canonical types, declarations, specializations, and ABI entities
  MUST be O(1) on the hot path.
- Selected declarations, conversions, value categories, lifetime actions,
  layouts, effects, and ABI choices MUST be recorded at their semantic owner.
  Lowering and optimization MUST NOT reconstruct them through lookup or text.
- Non-dependent template structure and facts MUST be shared.  Each complete
  specialization key MUST expose explicit not-started, in-progress, complete,
  and failed states so recursion cannot duplicate work.
- Parsing, substitution, validation, layout, runtime demand, and emission demand
  are distinct operations.  Requesting one MUST NOT eagerly perform a broader
  operation.
- Runtime and emission demand MUST use typed roots, reasons, and edges.  A body
  may be semantically validated without becoming an emitted definition.
- Lookup MUST inspect only language-relevant scopes and candidates.  Small
  collections MAY use inline linear search; larger collections SHOULD use
  compact indexes.  Deterministic order MUST be independent of hash-table
  iteration.

## 4. Work scheduling, caches, and storage

- Deferred or cyclic facts MUST use deduplicated typed worklists and explicit
  in-progress states.  A changed fact may reconsider only consumers that can
  make progress.  Whole-program retry-until-stable loops are forbidden.
- Reverse dependency edges are appropriate only when a consumer can observe a
  fact before its final state.  Acyclic local work SHOULD be evaluated directly
  rather than recorded in a permanent dependency graph.
- Memoization required for semantic correctness or recursive completion is
  mandatory.  Acceleration caches are optional and MUST demonstrate that saved
  work exceeds keying, lookup, dependency, invalidation, code-size, and memory
  costs on representative inputs.
- Risky caches SHOULD provide a verification mode that recomputes hits and
  checks complete keys.  An incomplete key is a correctness defect.
- Dense tables are preferred when the canonical domain is already dense and
  enough eligible work exists to amortize construction.  Do not build a dense
  index from a textual model on every pass merely to accelerate that pass.
- Hot records SHOULD use compact IDs, enums, packed flags, arenas, slabs,
  trailing ranges, and small inline storage.  They MUST NOT use per-node owning
  strings, `shared_ptr`, node-based containers, or individual allocation unless
  measurement justifies the exception.
- Ordinary work SHOULD be O(n) or O(n log n) in the facts or IR it consumes.
  Every broader bound requires a documented semantic or ABI reason.

## 5. Typed lowering and native emission

- LowIR MUST be a single typed in-memory model.  Textual LowIR is an input/output
  adapter, never the transport used by the source-to-object path.
- LowIR operands, instructions, values, slots, blocks, symbols, types, and
  operators MUST use typed identities and payloads.  MIR control-flow labels,
  frame bindings, symbols, fixups, exception references, and operands MUST do
  the same.
- Canonicalization of explicit textual input belongs at its adapter boundary.
  Ordinary lowering MUST publish complete typed facts directly and MUST NOT
  erase and reconstruct them in a later whole-program pass.
- Compact translation-unit summaries and LowIR bodies MAY remain live through
  interprocedural optimization and final reachability.  Each surviving function
  SHOULD then be selected, machine-lowered, allocated, encoded, and reclaimed
  once.
- Register allocation and instruction selection MUST be linear or near-linear
  in machine-IR size.  Local fixed-point transforms MUST use dirty
  instruction/block worklists and a monotonic progress measure.
- Native object output MUST contain only sections required by the language,
  ABI, debugger, and native linker.  Private serialized compiler IR may appear
  only in an explicit compiler-object or LTO format, never silently in an
  ordinary native object.

## 6. Optimization levels and bounded inlining

Each optimization level MUST have explicit pass, growth, memory, and work
budgets.

- **O0** performs correct and efficient target lowering: direct ABI/value
  placement, compact encodings, canonical instruction selection, trivial
  fallthrough removal, necessary strength reduction, correct demand/COMDAT
  decisions, and compact exception metadata.  It does not intentionally emit
  poor machine code merely because optional IR optimization is disabled.
- **O1** performs bounded, high-value simplification, dead-code cleanup, scalar
  promotion, and conservative interprocedural work with code-neutral or clearly
  profitable growth.
- **O2** may spend more compile time on broader inlining, value and memory
  optimization, loop work, and placement when measured benefits justify it.
- **O3** may use larger but finite growth budgets and aggressive loop or vector
  transforms.  It MUST remain deterministic and bounded.

An effective inliner MUST obtain convergence without retrying the complete
program:

1. Build one typed direct-call graph and reverse graph in O(functions + edges).
2. Compute strongly connected components and a stable callee-first order.
3. Simplify each nonrecursive callee and publish compact versioned summaries
   before evaluating its callers.
4. When a summary changes, enqueue only affected reverse callers.  Newly exposed
   call sites are processed through the same bounded worklist.
5. Handle recursive components conservatively or through a separately bounded
   snapshot; cloned recursive calls MUST NOT recursively re-enter the queue.
6. Charge per-call cloning, per-caller growth, recursion depth, and total
   translation-unit growth.  Budget exhaustion skips work and is observable.
7. After a clone, simplify only the inserted or affected region when practical;
   do not rerun the complete optimization pipeline after every call.

Inlining profitability MUST use typed facts and account for removed call setup,
return movement, constants exposed, cleanup and exception regions removed, and
whether the last demand for a body disappears.  It MUST NOT recognize library
type names, demangled spellings, filenames, or benchmark text.

After optional inlining, reachability MUST be recomputed from typed language and
ABI roots.  Internal or discardable bodies with no remaining call, address,
lifecycle, virtual, exception, or export edge SHOULD be removed before machine
lowering.

## 7. Measurement and conformance

Performance work MUST measure both compiler cost and generated-program quality.

- Compiler measurements include phase time, wall/user/system time, peak RSS,
  allocation and retained bytes, worklist operations, cache maintenance, and IR
  sizes.
- Generated-code measurements include deterministic object hashes, section
  sizes, function and call counts, instruction counts, spills/movement,
  relocations, and exception metadata.  A self-hosted compiler is an important
  generated-program workload, not merely a correctness check.
- Performance comparisons MUST use immutable executables, equivalent inputs and
  environments, interleaved runs, and medians.  No performance claim may rely
  on one loaded-host wall-time sample.
- After a large improvement, the compiler MUST be reprofiled before selecting
  the next target.  Structural counters must corroborate timing.
- A representation-only change must preserve serialized and object output
  exactly.  An intentional optimizer or backend change must be deterministic,
  explained by its public contract, and tested behaviorally.
- Failures discovered late MUST receive a reduced regression at the earliest
  public layer that owns the broken fact.  Exact tests should enforce public
  representation contracts; structural or behavioral tests should avoid
  freezing private layout.
- Changes to lowering, ABI, lifetime, exception, or optimization facts MUST be
  self-hosted at both the baseline and maximum levels.  A stronger optimizer can
  hide defects in baseline-generated code.
- Architecture review MUST trace representative facts from their first typed
  owner through object emission and account for every render, cache, dependency,
  emitted body, relocation, cleanup, and metadata record.  Disabled counters
  MUST show zero production textual downgrades for facts already classified.
- Rejected experiments and their structural and timing evidence SHOULD be kept
  in a durable ledger so an attractive but losing design is not repeatedly
  rediscovered.

An unexplained textual downgrade, duplicate production model, whole-program
retry, incomplete cache key, broad invalidation, or unbounded optimization is
an architecture defect even when functional tests pass.
