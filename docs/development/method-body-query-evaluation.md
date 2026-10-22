# Method-body query first slice — 2026-10-22

The new `experimentalMethodBodyQueries` flag remains **off by default**. It
implements independent method-sized binding for a restricted closed scalar
helper, reusing value snapshots of the enclosing query environment. It does not
make the complete mutable compilation or diagnostic analysis incremental.

## Implemented boundary

- Variable-length and multiline expression edits, preserving the method
  signature and local declaration tokens. Initializer expressions may change.
- Inline, ordinary instance functions in non-parameterized package classes;
  imports, inheritance and registration macros outside the function are allowed.
- Plain `int` arguments / locals / return values, initialized flat locals and
  a final return; closed arithmetic over locals and arguments.
- Lexical completion and definition at verified argument / local reference
  sites, plus deferred resolution. Hover remains native.
- Current position projection for declarations after the edited method;
  certificate, document-version and context-epoch fencing of completion handles.
- Coexistence with UVM / scope semantic owners. Their explicit fixed-dependency
  contract is preserved; manual rebuild replaces the query certificate too.

An existing current query context is required for preparation. Unknown or
incomplete expressions, calls, nonlocal references, control flow, macro-bearing
methods, out-of-block methods, parameterized classes and declaration edits use
native analysis. Each certificate accepts at most 64 edits; native analysis and
subsequent preparation remain available after that boundary. Dependencies and
other open documents are checked, and cancellation never applies an edit twice.

The certificate retains completion values and mutable input witnesses, not a
base AST or a semantic owner. Its temporary full source snapshot is discarded.
The new mode always uses native current diagnostics; it never replays the
legacy experiment's saved diagnostic snapshot.

## Correctness and parser evidence

The final build passed:

- **753 CTest cases**, including the SDK suite and 8 new body-query cases.
- **17 Python system / method-scope tests**.
- **43 diagnostic transport cases**.
- Large-package protocol oracle: 3 paired blocks, 12 edits per session, 12
  sessions / 144 edited states across plain and UVM-enabled fixtures. Ordered
  completions, selected resolves, definition ranges and diagnostic values match
  the native path after removing owner-specific opaque `data` handles.
- Separate counted executable: each accepted edit causes exactly one
  `mini-shape` foreground parse and no foreground full-unit parse. Native edits
  cause one full-unit foreground parse. The idle diagnostic snapshot in the new
  mode still parses the current unit; this is recorded separately. No timings
  from the counted executable are used.

The UVM fixture derives its class from `uvm_object`, uses UVM registration
macros and imports, and enables UVM and scope semantic caching. It is a synthetic
closed-helper workload, not evidence of arbitrary real-UVM method coverage.

## Mac measurements

Release executable, warm opened document, 5000 synthetic neighboring classes,
3 paired blocks. Each session has 12 edits. The first-edit cells contain 3
samples; continuous cells pool 33 later edits per configuration. These medians
are observations, not a production latency guarantee. A query sample includes
`didChange` plus the immediate completion response.

| Environment | Mechanism | First edit / certification | Continuous query P50 | Diagnostic P50 |
|---|---|---:|---:|---:|
| Plain package | Native | 9.99 ms | 7.79 ms | 78.77 ms |
| Plain package | Body query | 20.78 ms | 3.43 ms | 79.82 ms |
| UVM semantic owner | Native | 17.50 ms | 16.15 ms | 19.43 ms |
| UVM semantic owner | Body query | 54.45 ms | 7.13 ms | 22.62 ms |

Including first certification, the median sum of 12 query responses changes
from 98.27 to 57.97 ms in the plain case and 197.20 to 134.58 ms with UVM.
However, the sum of per-edit diagnostic wait windows changes from 950.82 to
970.50 ms and from 236.59 to 327.21 ms respectively. Faster foreground queries
therefore **do not establish an overall diagnostic-including speedup**. The
full parsing / analysis work still happens behind those queries.

Separate memory-only runs use the same UVM fixture and 12 edits, 3 paired
blocks, with process-family sampling every 20 ms. Median observed peaks:

| Mechanism | RSS | Physical footprint |
|---|---:|---:|
| Native | 172.5 MiB | 126.0 MiB |
| Body query | 184.8 MiB | 149.2 MiB |

This slice increases memory. It does not establish a bound for arbitrarily long
sessions; completion values and mutable witnesses still scale with the visible
environment. Independent mini compilations replace their previous generation.

## Real CVA6 fallback

The existing full CVA6 plus core-v-verif UVM scenario was rerun with UVM and the
selected DUT frozen, with the new flag off and on. Both arms match all six
Native diagnostic / hover reference states, including error injection and
repair. The entire selected DUT remains the retained semantic root:

`uvmt_cva6_tb.cva6_dut_wrap.cva6_tb_wrapper_i.i_cva6`

That scenario edits a class-level probe outside the eligible method body; its
expected result is `edit outside function`, zero accepted local edits and native
analysis. This validates coexistence and fallback, **not a CVA6 acceleration**.
The production UVM methods at those existing edit points are not claimed as
covered by this first slice.

## Reproduction and evidence

```sh
source/.venv/bin/python source/tests/method_scope/body_queries.py \
  --binary build-uvm/bin/slang-server --classes 5000 --blocks 3 \
  --output untracked/method-body-2026-10-22/paired.json
source/.venv/bin/python source/tests/method_scope/body_counts.py \
  --binary build-uvm/bin/slang_server_counted \
  --output untracked/method-body-2026-10-22/parser-counts.json
```

Local evidence is retained under `untracked/method-body-2026-10-22/`: paired
queries, parser counts, separate memory samples, CVA6 fallback, full test logs
and a binary/hash receipt. Generated fixture directories are automatically
removed. No external source, dependency or test corpus was downloaded.

The next extension should address common method statements and member-dependent
bindings, while keeping declaration identity explicit. Incremental diagnostics
and cheaper first certification remain separate work; this implementation does
not claim to solve them.
