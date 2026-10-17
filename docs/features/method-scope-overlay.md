# Experimental method scope overlay

`experimentalMethodScopeOverlay` defaults to `false`. It is an experiment for
package exploration using a literal complete `compilationFilelist`,
`--single-unit --threads 1`, and one included class header. It preserves the
native full analysis path for unsupported edits and queries.

The first implementation accepts one change per notification, at most eight
consecutive accepted edits, equal-length ASCII text, unchanged CR/LF positions,
and a fixed number of initialized scalar `int` locals. The selected ordinary
instance function must have explicit scalar `input int` arguments, a scalar
`int` result, flat initialized locals, and one final return. Its expressions may
use those locals and arguments, integer literals, parentheses, unary `+`/`-`,
and binary `+`/`-`/`*`. A mini compilation verifies the new bindings and types.

Only the target header may be open. Macro environments, repeated includes,
imports in the containing package, inheritance, local names shadowing enclosing-scope
members, nonlocal expressions, interface changes, variable-length edits,
line-layout changes, and local additions/removals are outside this first slice.
Configuration, command files, source bytes, successful and failed lookups,
source generations, and index revisions must still match the preparation
certificate. Unknown cases take the native path.

Completion and definition responses cover current argument/local reference
sites. Completion preserves the full native item order and field values, using
new method-owned items and value snapshots for the unchanged environment.
Definition links come from current mini-compilation bindings. Old automatic
local/formal completion handles cannot resolve to a new declaration merely
because its name or offset matches. Queries requiring changed enclosing-method
documentation take the native path. Diagnostics still run over a complete
current source snapshot in the isolated worker.

The source manager and AST for a mini compilation have a separate lifetime.
Preparation retains completion values and source witnesses, rather than base
AST or symbol pointers. Cancellation discards an unpublished candidate; editor
text synchronization still occurs exactly once. Once synchronization starts,
an exception cannot replay the same incremental range.

## Validation tools

`tests/method_scope/check.py` compares full ordered JSON against native queries,
including binding changes and source-generation checks. `tests/cpp/MethodScopeOverlayTests.cpp`
checks cancellation, publication failure, fallback, and lifecycle invalidation.
On Linux, `slang_server_counted` wraps the parser entry point for count evidence;
it must never supply performance samples. The ordinary `slang-server` executable
has no parser wrapper.

`tests/method_scope/benchmark.py` records a plan before measurement and applies
one framing/timestamp protocol to both arms of the same ordinary binary. It
reports completion and definition separately, keeps cold preparation, whole
sequence and diagnostic waits separate, and reports the ninth edit separately
from the eight accepted edits. Measurements from another build are not current
validation. This experiment does not establish general responsiveness gains or
UVM support.

The 2026-10-17 Linux rebuild passed its recorded correctness checks, but its
synthetic timing medians were slower at every measured step for both query
kinds. See [the validation record](https://github.com/Maybeka/slang-server/blob/main/validation/linux-2026-10-17.json). The
experiment remains default off.
