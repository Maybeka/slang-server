# Experimental UVM prefix reuse

Set `experimentalUvmPrefixCache` to `true` to evaluate reuse of an unchanged UVM
library in exploration mode. It defaults to `false`. Each process owns its
parsed-library cache in memory; this does not create a portable or persistent UVM
database.

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

Source-location tables can grow during repeated macro expansion. After synchronizing an incoming edit, the experiment replaces a context once source storage and table
capacity exceed the larger of 16 MiB and twice its seeded size. Replacement
preserves open unsaved text, versions, configuration and include selections. It
releases recomputable semantic models and seeds the replacement with the latest
unsaved text, avoiding a parse of the previous version immediately before it is
invalidated. Failed construction retains the old context and can retry.
Completion handles carry a generation: expired lexical handles use fresh logical
lookup; expired receiver/offset handles are rejected. A live SourceManager never
recycles its old IDs.

The retirement threshold bounds retained source generations, not total process
RSS. Syntax, semantic models, allocators and independent diagnostic workers also
consume memory. Retirement itself can add a latency spike, and short sessions
can retain more memory than native parsing.

With this setting enabled, isolated diagnostics may keep one worker alive for the
current parent generation. The worker captures its own immutable UVM prefix and
resumes compatible snapshots; syntax pointers and source IDs never cross process
boundaries. Reuse requires unchanged ordered options, root identity, retained
source bytes, generated command-line definitions, include lookup topology and
fixed macro state. Every job still performs semantic analysis and produces the
complete diagnostics and inactive regions. Incompatible snapshots reset the
worker context; a failed resume restarts the entire job with fresh native parsing.
One-shot diagnostic parses and permanently declined foreground parses do not
capture unused checkpoints.

Worker contexts use the same source-storage/table retirement budget. Cancel,
parent generation changes, timeout, failure and shutdown discard the worker;
five seconds without another job also releases it. A warm worker retains memory
between edits, so compare both the parent and the whole process family. The
experiment preserves complete diagnostic entries, versions and related
information; diagnostics that share an expanded location and code can appear in
a different array order because physical source IDs differ. Related-information
ordering and completion/definition/resolve ordering are unchanged.

See the [worker and retirement follow-up report](../../validation/macos-uvm-followup-2026-10-19.md)
and the [initial Mac acceptance and performance report](../../validation/macos-uvm-prefix-2026-10-19.md)
for measured first-query, continuous-edit, full-resolve, fallback and memory
tradeoffs, including negative results. The setting remains opt-in.
