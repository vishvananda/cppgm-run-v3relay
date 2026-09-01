# PA11 CP1 audit

CP1 implements the type table, scope/entity model, lookup, declarator type
construction, declaration walk, and `--emit-types` driver. Semantic state is
fresh for each input file and dump order is owned by scope/binding vectors.

Decisions recorded from the CP1 packet:

- A class entity keeps the latest class key for lookup, while each declaration
  binding retains its own type spelling. Thus `struct C; class C {}` prints
  both type lines and creates one class scope.
- An unnamed namespace is named `<unnamed>` and contributes an implicit using
  directive to its enclosing scope.
- Parameter cv-qualification and array parameter extents are preserved as
  declared. `(void)` is normalized to an empty parameter list, and
  `constexpr` adds top-level const only to object types.
- Block-scope function declarations and linkage blocks use the current scope.
  Anonymous structs derive their name from the first declarator; anonymous
  unions remain deferred to CP2 because PA10 does not preserve the required
  declaration extent.

Verification evidence:

- The packet’s 35 CP1 fixtures pass byte-for-byte or with the expected exit
  status.
- `make test-pa11` reports 45/68 tests passing. The 23 remaining failures
  are deferred enum/constant-evaluation/sizeof/static-assert/template cases;
  no CP1 fixture remains failing.
- `make test-report-through-pa10` reports 589/589.
- `perl scripts/cppgm_file_audit.pl --stage pa11 --paths dev/src` passes with
  the existing `recog_parser.h` substantial-body warning.
- On the packet probe (200 namespaces × 100 declarations plus 10 nested
  blocks), one input measured `0.23 s, 57116 KB`; the same workload supplied
  twice as two argv inputs measured `0.41 s, 58972 KB`.
