# Experimental UVM prefix reuse

Set `experimentalUvmPrefixCache` to `true` to evaluate reuse of an unchanged UVM
library in exploration mode. It defaults to `false`. Each process owns its
parsed-library cache in memory; this does not create a portable or persistent UVM
database.

This dedicated UVM route remains independent of generic package/module freezing.
Using a common implementation is optional; UVM is not required to pay the cost
of a general frozen-input group. For a UVM-only evaluation, enable this setting
and keep `experimentalFrozenSources` disabled. General scope and RTL experiments
can be evaluated separately. Turning on both settings currently allows a generic
group to take precedence, so it must not be assumed to preserve the dedicated
route's performance.

## Select a UVM library

Configure sources independently of enabling the cache:

```json
{
  "uvm": {"sourceDirectory": "third_party/uvm/src", "entryFile": "uvm_pkg.sv"},
  "experimentalUvmPrefixCache": false
}
```

`sourceDirectory` resolves from the workspace root, or accepts an absolute path.
`entryFile` resolves from that directory, or accepts an absolute path. Without an
entry, the server selects `uvm_pkg.sv` directly in the directory, then
`src/uvm_pkg.sv`, then `uvm.sv`. If both package locations exist, select an entry
explicitly. The source directory, entry directory and existing `src` directory
are added after configured include paths as literal directories, including names
containing spaces or wildcard characters.

When a project already supplies the selected library, the server retains the
original root order and include wrappers. Otherwise it prepends the entry to
the source inputs. It requires exactly one `uvm_pkg` reached through the selected
entry; a different library, missing entry or ambiguous choice reports a
configuration error. Set `entryFile` to the entry used by the project, such as
`uvm.sv` when the filelist uses that wrapper. No configuration means no automatic
UVM injection. Use the separate `useBundled` choice below to select the shipped kit.

A library configured without a complete compilation filelist also supplies the
package to independently opened user sources. Filelist projects retain their
explicit source boundaries. Watched library inputs and configuration changes
rebuild the context while preserving unsaved editor text. Creation of a missing
entry can repair the configuration. Open editor text takes precedence over a
disk event for the same library file.

Setting `experimentalUvmPrefixCache` to `true` permits compatible in-process
prefix reuse, including explicitly named entries. Source configuration alone
does not enable caching. The cache restrictions below still apply: a library
included through a different root, or positioned after another source, keeps
native parsing. The configuration does not promise semantic-analysis reuse or
a persistent precompiled database.

For explicitly immutable libraries, the separate opt-in
[trusted semantic cache](uvm-semantic-cache.md) retains elaborated UVM semantics
and uses manual reconstruction instead of these automatic freshness rules.

## Reuse and invalidation

The ordered compilation must start with `uvm_pkg.sv` or `uvm.sv`, containing only
the `uvm_pkg` package, followed by the design roots in the same compilation unit.
An explicitly configured entry can use another filename. A standalone library
can retain its native immutable tree without parsing it again for preparation.
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

The [current Mac class-session comparison](../../validation/macos-native-idle-class-sessions-2026-10-20.md)
also compares the dedicated route with generic package freezing on real
SPI/PSS/UBUS. Generic freezing remains slower than dedicated UVM in ordinary
class editing and macro-fallback sessions. Retain the dedicated route for those
workloads rather than assuming that the more general implementation is faster.
The current [RTL comparison](../../validation/macos-native-idle-rtl-2026-10-20.md)
describes the separate hierarchy workload where generic instance-value reuse
helps. These are workload-specific measurements, not an automatic route selector
or evidence that enabling both switches combines their benefits.

## Optional shipped UVM

The server distribution includes an unchanged UVM 1.2 source kit. Activate it
explicitly when the project does not supply UVM:

```json
{
  "uvm": {"useBundled": true},
  "experimentalUvmPrefixCache": false
}
```

`useBundled` defaults to `false` and cannot be combined with `sourceDirectory` or
`entryFile`. Existing projects retain their source choices by default. A project
that supplies a different UVM while requesting the shipped kit receives a
configuration error rather than mixing or replacing libraries. Selecting the kit
does not enable either experimental cache. It loads the shipped preparsed syntax
data for the default configuration, including on the first startup. Only that
configuration is shipped. Other parse configurations generate a project-local
one-time syntax cache. See
[the persistent UVM contract](uvm-semantic-cache.md#persistent-uvm-syntax-data).

The fixed upstream revision is
`71f30a7473118b3cea582d7673c3af8e161fce71` from Accellera's UVM repository.
`resources/uvm/1.2/MANIFEST.json` records the exact source/version/SHA-256 inventory;
configuration checks every source file against it before staging the build.
The original `LICENSE.txt`, `NOTICE.txt` and source headers accompany the kit.
The server does not download a library while opening a project.

CMake installation places the binary in `bin` and the kit under
`share/slang-server/uvm/1.2`. Keep those directories together when moving an
installation. A copied executable without its resources remains usable with
normal project inputs or `sourceDirectory`; selecting a missing shipped kit
reports how to repair the installation. The shipped sources and `preparsed/` data
form one immutable distribution. Rebuild the kit when changing that distribution;
use `sourceDirectory` for a custom source-based library. Ordinary user source edits
remain live.

Release archives keep the executable at their root and carry
`resources/uvm/1.2` beside it for existing editor installers. Keep that folder
when moving a release. See the [Mac packaging validation](../../validation/macos-bundled-uvm-2026-10-20.md)
for relocation, missing-resource and compatibility checks.
