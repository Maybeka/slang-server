# Experimental UVM prefix reuse

Set `experimentalUvmPrefixCache` to `true` to evaluate reuse of an unchanged UVM
library in exploration mode. It defaults to `false`. This is an in-process
parsed-library cache; it does not create a portable or persistent UVM database.

The ordered compilation must start with `uvm_pkg.sv` or `uvm.sv`, containing only
the `uvm_pkg` package, followed by the design roots in the same compilation unit.
Keep external macros that affect UVM in the ordered command-line/configuration
flags. UVM's own definitions and ordinary invocations remain supported. Source
overrides, changed dependencies, incompatible parse options and unsupported
preprocessor state decline reuse and preserve native parsing.

The native initial parse captures the prefix's macro/include-once checkpoint and
parser metadata. Subsequent compatible edits parse the suffix and compose one
logical compilation unit, borrowing immutable prefix nodes and cloning only the
suffix. Borrowed nodes retain their physical ancestry and enum-preview links;
their arena and raw source storage stay alive with the composed tree. Existing
SourceManager buffer invalidation rules still apply. A SourceManager must outlive
all of its syntax views.

Macro and type completion resolution share compatible library parses. The cache
checks source freshness, source library, exact lexer/preprocessor/parser options
and working directory. It holds at most 64 entries and conservatively charges
retained syntax and source bytes against a 64 MiB budget. Oversized inputs,
callbacks and observable line/pragma effects bypass it. Open/close, watched files
and changed options invalidate entries. A sole same-directory literal include
from `uvm.sv` to `uvm_pkg.sv` can reuse the captured package for these resolution
queries, after checking native include metadata and wrapper tokens. Additional
macro/conditional/token state, comment diagnostic effects or parse errors reject
that equivalence. This does not change compilation ancestry or diagnostic input.

Source-location tables can grow during repeated macro expansion. At a serialized
edit boundary, the experiment replaces a context once source storage and table
capacity exceed the larger of 16 MiB and twice its seeded size. Replacement
preserves open unsaved text, versions, configuration and include selections. It
releases recomputable semantic models and delays their reconstruction until after
the incoming edit. Failed construction retains the old context and can retry.
Completion handles carry a generation: expired lexical handles use fresh logical
lookup; expired receiver/offset handles are rejected. A live SourceManager never
recycles its old IDs.

The retirement threshold bounds retained source generations, not total process
RSS. Syntax, semantic models, allocators and independent diagnostic workers also
consume memory. Diagnostics continue to use full native worker parsing; the
experiment does not suppress or skip them. Retirement itself can add a latency
spike, and short sessions can retain more memory than native parsing.

See the [Mac acceptance and performance report](../../validation/macos-uvm-prefix-2026-10-19.md)
for measured first-query, continuous-edit, full-resolve, fallback and memory
tradeoffs, including negative results. The setting remains opt-in.
