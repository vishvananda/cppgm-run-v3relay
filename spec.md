# Compile-Time-First C++11 Compiler Architecture

## Purpose

This specification defines the production architecture for a self-contained
Linux x86-64 C++11 compiler. It is normative and intended for implementation
reviews and architecture audits.

Compile-time performance is a property of semantic design, not a mode that may
skip required language or ABI behavior. Every required result must be produced
by the compiler's own front end, semantic engine, lowering pipeline, backend,
and object writer.

## Production pipeline

The source-to-object path MUST have one forward data flow:

```text
source buffers
    -> streaming preprocessor and token cursor
    -> integrated parser and semantic construction
    -> canonical typed semantic graph
    -> direct typed LowIR
    -> bounded per-function machine IR
    -> direct ELF object writer
```

Textual token, syntax-tree, LowIR, assembly, and diagnostic forms are views or
tool outputs. They MUST NOT be transport formats between production phases.

The compiler MAY expose preprocessing, token, syntax, semantic, LowIR, and
machine-code tools. Each tool must stop at, or render a view of, the relevant
shared phase. Tool requirements MUST NOT force the object compiler to construct
unused representations.

## 1. Source, preprocessing, and parsing

- Source files MUST be retained as immutable byte buffers with compact file and
  offset identities. Tokens and nodes SHOULD refer to source ranges rather than
  copying source spellings.
- The preprocessor MUST expose a streaming token cursor. It MAY retain compact
  tokens that are genuinely needed for deferred language behavior, but ordinary
  compilation MUST NOT build successive owning vectors of preprocessing
  tokens, post-tokens, recognition tokens, and parse tokens.
- Identifier spellings MUST be interned as they enter the front end. Tokens
  SHOULD carry identifier IDs or interned pointers, not owning strings.
- Each grammatical source region MUST be parsed at most once. Template bodies
  MUST retain a compact parsed representation; later instantiation may
  substitute and check dependent nodes but MUST NOT replay the grammar from
  token positions.
- Parsing and semantic analysis MUST cooperate while constructing one typed,
  source-faithful semantic graph. The production path MUST NOT construct a
  complete syntax tree and then copy it into a second complete semantic tree.
- Source faithfulness MUST be represented by compact locations and minimal
  syntax wrappers around semantic nodes. It MUST NOT require a parallel owning
  tree solely for diagnostics or rendering.
- Error recovery MAY use temporary parser checkpoints. Checkpoints MUST have
  bounded scope and MUST NOT clone the token stream or retain abandoned trees.

## 2. Canonical semantic identity

- Identifiers, canonical types, declaration identities, scopes, template
  argument lists, specializations, layouts, constants, and ABI entities MUST
  have stable interned pointer or compact integer identity.
- Equality for canonical types, declarations, specializations, and ABI entities
  MUST be O(1) on the hot path.
- Strings are presentation data. A rendered type, qualified name, signature,
  mangled name, or serialized node MUST NOT be the primary key for semantic
  equality, lookup, substitution, lowering, or invalidation.
- Published types, expressions, declarations, template patterns, and
  environments SHOULD be immutable. Mutability required by the language MUST
  be isolated in explicit fact/state records rather than spread through nodes.
- Non-dependent template types, expressions, statements, and declarations MUST
  be shared across instantiations. A specialization stores only substituted or
  newly established facts.
- Qualifiers, value categories, dependence flags, and common source-location
  data SHOULD be packed values or compact side data. They MUST NOT cause
  duplicate heap nodes for otherwise identical semantics.
- Semantic facts required by later phases—selected declaration, conversion
  sequence, object identity, value category, lifetime action, layout, ABI entry
  point, and linkage—MUST be recorded once and consumed by identity.

## 3. Scopes, lookup, and overload resolution

- Every scope MUST maintain direct indexes by interned name and relevant
  declaration kind. Lookup MUST visit lexical parents and language-required
  associated scopes without scanning unrelated declarations.
- Overload sets MUST be stored as compact candidate sequences. Candidate
  filtering SHOULD use declaration kind, arity, member/static category,
  template shape, and other semantics-preserving indexes before expensive
  substitution or conversion analysis.
- The implementation MUST inspect every candidate required by the language. It
  MUST NOT inspect declarations that cannot belong to the lookup result merely
  because they share a global registry.
- Using-directive, argument-dependent lookup, hidden-friend, and base-class
  relationships MUST have explicit indexed edges. They MUST NOT be rediscovered
  by repeated whole-program searches.
- A completed overload result MUST retain the chosen declaration and conversion
  facts. Lowering MUST NOT rerun lookup or overload resolution.
- Expected candidate rejection MUST return a compact result code and optional
  lazy diagnostic. It MUST NOT eagerly render diagnostic strings or use C++
  exception unwinding as ordinary control flow.
- Negative results MAY be cached only when the key contains every semantic
  input that can affect the result. An incomplete key is a correctness defect,
  not a performance optimization.

## 4. Templates and demand

- A specialization key MUST contain the canonical template identity, canonical
  arguments, enclosing substitution environment, and every context component
  that can change semantic behavior.
- Each specialization MUST expose separate monotonic states for declaration,
  definition completion, layout, default arguments, exception specifications,
  member bodies, vtable/RTTI facts, and emission demand.
- A fact for one complete specialization key MUST be computed at most once.
  Its states MUST distinguish not-started, in-progress, success, and expected
  failure. Recursive demand observes the in-progress state rather than starting
  duplicate work.
- Successful and expected-failure results MUST be memoized at the narrowest
  correct owner. Failure caching MUST retain enough structured diagnostic
  information to explain a selected final error without rebuilding every
  rejected candidate.
- Template environments MUST be immutable parent-linked frames or compact
  overlays. Instantiation MUST NOT copy a vector or map of every visible
  binding at each nesting level.
- Instantiation MUST transform and recheck only dependent nodes. Non-dependent
  nodes and facts MUST be reused directly.
- Only language-required declarations, definitions, layouts, initializers,
  support objects, and bodies may be demanded. Completing a class MUST NOT
  eagerly instantiate unrelated member bodies.
- Parsing a template body, substituting it, validating it, completing its
  containing class, and emitting code are different operations. No operation
  may invoke a broader one merely because the implementation stores them
  together.
- Demand MUST be represented by explicit typed reasons and dependency edges.
  Source order and deterministic output order MUST be separate from semantic
  demand identity.

## 5. Dependency scheduling and caches

- New semantic facts MUST propagate through an explicit worklist. Producers
  MUST record reverse dependencies so that only consumers of a changed fact are
  reconsidered.
- Worklist items MUST be keyed by stable entity/fact identity and deduplicated.
  Each monotonic state transition may enqueue a dependent edge only when the
  dependent can make progress.
- Adding or restoring one fact MUST NOT trigger a retry of every pending class,
  function, specialization, or declaration.
- Caches MUST have explicit owners, complete typed keys, and a lifetime no
  broader than those keys. A cache entry MUST state whether its result is valid
  across declaration insertion, class completion, substitution, and emission.
- Invalidations MUST be precise. A local mutation MUST NOT clear unrelated
  translation-unit caches or increment a global generation that makes every
  lookup cold.
- Cycles MUST be handled by explicit in-progress states and strongly connected
  dependency handling where required. Retry-until-nothing-changes loops over
  the complete program are forbidden.
- Deterministic output MUST be obtained by stable IDs, source ordinals, or a
  final ordering step. Hot semantic containers MUST NOT be ordered trees solely
  to make incidental iteration deterministic.

## 6. Typed lowering

- Lowering MUST construct typed LowIR objects directly from recorded semantic
  facts. The normal source-to-object path MUST NOT serialize and reparse LowIR.
- A textual LowIR parser and serializer MAY exist for staged tools, debugging,
  and explicit LowIR input/output. They MUST be adapters around the typed model,
  not the model used to communicate between in-process phases.
- LowIR constructors MUST enforce local structural validity. Typed references
  MUST make invalid cross-links difficult to represent.
- Every emitted function, variable initializer, support object, thunk, and ABI
  entry point MUST have a stable emission identity and MUST be lowered once.
  Distinct ABI entry points are distinct emission units even when they share a
  body.
- Lowering MUST consume chosen declarations, conversions, layouts, lifetime
  actions, and ABI facts directly. It MUST NOT reconstruct them from names,
  type spellings, mangled strings, or a second semantic search.
- Lowering MUST NOT clone semantic trees or synthesize fake semantic nodes to
  reuse front-end code. Lowering-specific state belongs in typed lowering
  records.
- A missing required semantic fact is an invariant violation. Production code
  MUST NOT contain a slower textual, name-based, or whole-program fallback that
  hides the missing fact.
- Complete-program validation MAY be used for explicit LowIR input and audit
  builds. An unchanged in-memory program MUST NOT be fully revalidated between
  ordinary production passes.

## 7. Backend and object generation

- The default object path MUST use a compact per-function machine IR and direct
  ELF emission. It MUST NOT emit assembly text, parse assembly text, or invoke
  an external assembler.
- Function lowering, instruction selection, register allocation, local
  relaxation, and encoding SHOULD complete before advancing to the next large
  function. Function-local transient state SHOULD then be released.
- The default backend MUST use a deliberately small set of high-value passes.
  Each pass MUST have a documented scope and complexity bound.
- Register allocation MUST be linear or near-linear in machine-IR size.
  Instruction selection and encoding MUST be linear in the selected function.
- Fixed-point transforms MUST use a dirty instruction/block worklist and a
  monotonic progress measure. They MUST NOT rescan every instruction after each
  local change.
- Analyses MUST be cached at their natural IR unit. A function or loop pass MUST
  NOT recompute a translation-unit analysis.
- Whole-program state is permitted only for behavior that crosses functions,
  including linkage/COMDAT decisions, global initialization order, demanded
  symbols, and final section/relocation layout.
- Optimization levels MUST select explicit pass budgets. Expensive global
  transforms MUST NOT be an implicit prerequisite of ordinary object emission.

## 8. Allocation, containers, and lifetimes

- Long-lived front-end nodes MUST use translation-unit arenas or slabs.
  Temporary parser, substitution, overload, lowering, and backend work MUST use
  shorter-lived arenas that can be discarded in bulk.
- Hot nodes MUST NOT use owning `shared_ptr`, individual `new`/`delete`,
  deep copy, or recursive destruction.
- Variable-size children SHOULD use trailing arrays, small inline vectors, or
  arena slices. Stable relationships SHOULD use compact IDs or non-owning
  arena pointers.
- Dominant maps and sets MUST use dense or flat storage keyed by compact
  identity. They MUST NOT require one allocation per entry. Ordered node-based
  containers are allowed only when observable ordering requires them and the
  cost is isolated from hot lookup.
- Common temporary collections SHOULD keep their small case inline. Large
  collections MUST grow geometrically and MUST NOT repeatedly copy their full
  contents.
- Ownership boundaries MUST be explicit for source buffers, retained template
  patterns, semantic facts, typed LowIR, machine IR, and ELF buffers.
- Data crossing a phase boundary MUST be the minimal typed fact set. It MUST
  NOT be an owning pointer into a phase that should otherwise be dead.
- The complete token graph, duplicate syntax graph, semantic graph, textual
  LowIR, typed LowIR, and machine IR MUST NOT coexist. Textual LowIR is absent
  from production, and function-local IR is reclaimed incrementally.
- Process-global mutable caches are forbidden. Any read-only global tables MUST
  be bounded compiler metadata rather than accumulated translation-unit state.

## 9. Complexity and observability

- Preprocessing MUST be O(source bytes plus produced expansion tokens), apart
  from work intrinsic to producing repeated macro expansions.
- Parsing MUST be O(tokens produced).
- Semantic work MUST be proportional to declarations actually introduced,
  candidates actually belonging to lookup sets, demanded specialization facts,
  and dependency edges. It MUST NOT be proportional to Cartesian products of
  unrelated global collections.
- Canonical identity comparison and completed-fact lookup MUST be O(1) average
  time.
- Typed lowering, ordinary optimization, machine selection, allocation, and
  object writing MUST be O(n) or O(n log n) in the IR they consume. A broader
  bound requires a documented language or ABI reason.
- Release builds MUST expose low-overhead counters and timers for phase time,
  peak live bytes, node counts, candidate counts, specialization transitions,
  cache hits/misses, worklist pushes, dependency edges, and IR sizes.
- Telemetry MUST observe existing work rather than introduce alternate semantic
  behavior. Its overhead MUST be separable from ordinary compilation.
- Performance-sensitive subsystems MUST expose work counters that allow an
  audit to distinguish expensive required work from accidental repeated work.

## 10. Self-contained implementation

- Required output MUST be produced entirely by this compiler. It MUST NOT invoke
  an external compiler, previous compiler stage, reference implementation, or
  cached answer to implement preprocessing, semantics, lowering, code
  generation, or object writing.
- The implementation MUST NOT recognize particular filenames, source text,
  tests, library type spellings, or expected outputs.
- Library and language behavior MUST arise from general semantic and ABI rules.
  A type-name shortcut is forbidden even when a particular header makes it
  appear reliable.
- Optional persistent caches, precompiled headers, modules, or daemon modes MAY
  be designed later. Correctness and the architecture above MUST NOT depend on
  them.

## Architecture audit checklist

An audit should trace one nontrivial declaration and one demanded template from
source bytes to ELF and answer the following questions.

### Representation and ownership

- How many owning representations of the same tokens, syntax, semantics, and IR
  are live at each boundary?
- Can every retained object name its owner and destruction phase?
- Does any later phase keep pointers that prevent an earlier phase from being
  released?
- Is any text rendered only to be parsed back into structured data?

### Identity and lookup

- Are hot keys compact canonical IDs, or are strings and composite rendered
  signatures being hashed and copied?
- Does type/entity equality reduce to identity, or recursively compare
  structure on common paths?
- Can a lookup visit declarations that cannot possibly be in its result?
- Are deterministic ordering concerns forcing ordered containers into semantic
  hot paths?

### Templates and repeated work

- Is each template body parsed once and each dependent fact computed once per
  complete key?
- Are environments shared as overlays, or copied at every nested
  instantiation?
- Are expected failures returned as results, or thrown as exceptions?
- Does one new fact enqueue its dependents, or cause a global retry?
- Are failed completion or substitution attempts cached with a correct key?
- Are non-dependent nodes reused, or cloned into every specialization?

### Lowering and backend

- Does lowering consume typed semantic facts without lookup, string parsing, or
  recovery fallbacks?
- Is each emission unit lowered and optimized once?
- Does any per-function operation scan all functions, globals, classes, or
  specializations?
- Are complete-program validators or serializers present on the ordinary object
  path?
- Is machine code and ELF data emitted directly?

### Allocation and scaling

- Do hot nodes allocate or free individually?
- Do maps allocate per entry or use strings as keys?
- Can phase-local arenas and per-function IR be reclaimed promptly?
- Does every fixed-point loop have a dirty worklist and monotonic progress
  measure?
- Do counters show visits, retries, candidates, or invalidations growing faster
  than the semantic input and produced output require?

Any unexplained text round trip, whole-program retry, global invalidation,
semantic reconstruction, repeated identical specialization work, per-node hot
allocation, or fallback lookup is an architecture defect even if functional
tests currently pass.
