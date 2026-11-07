# Configured syntax prefix cache

This experiment is off by default. It reuses authenticated syntax from preceding
roots in a configured single compilation unit, reducing repeated parsing after a
structural edit. It does not freeze those mutable roots or reuse their semantics.

Enable `experimentalSyntaxPrefixCache` together with
`experimentalCooperativeAnalysis` and `experimentalScopeSemanticCache`. The
server needs prepared fixed scope semantics in a configured single compilation
unit. It can extend an ordered fixed scope prefix or capture a syntax prefix
during the required parse for an isolated fixed declaration view. The
`frozenSources` selectors still control semantic reuse and its manual rebuild
contract.

`syntaxPrefixCacheMaxMiB` defaults to 256; zero disables retention. It limits the
estimated retained CST owner beyond the already charged fixed prefix. One pending
or reusable owner is retained. Oversized or unsupported entries are abandoned.
This is a separate budget from `frozenSources.maxMiB` (fixed semantic and body
owners) and `sourceHistoryMaxMiB` (source storage). These are cache budgets, not a
process RSS limit.

The required parse can capture a root boundary on the first structural edit.
A later replay authenticates the recorded input bytes, aliases, failed include
lookups and editor overlays before preparing or using the view. A prefix
captured independently of an ordered fixed scope prefix also checks the
source-loader selection and configuration inputs. Earlier edits invalidate the
view. Configuration reload or driver retirement releases this driver-local
state.

Callbacks, unsupported preprocessing state, and declarations spanning the
boundary use the ordinary parse. Complete balanced plain `pragma protect begin`
and `end` regions have no terminal protection state; encoded, nested, unbalanced,
reset and diagnostic pragma state are not admitted by this implementation.

`slang.scopeLibraryStatus` reports a separate `syntaxReplay` object with the
budget, retained charge, pending/cached root counts, captures, hits, refusals and
failed input witnesses. This change adds no fixed debounce.

Controller regression tests cover both included files and directly opened root
documents, cancellation before delayed extraction, earlier prefix edits, disabled
mode, zero budget and oversized-entry refusal. SDK tests cover checkpoint
ownership and source boundaries. The
[qualification record](../development/semantic-reuse-completion-plan.md) gives
the real-project latency, memory and reuse measurements.

If the mutable input witness fails, this driver refuses further results through
its old file/lookup cache. The server recovers with a fresh input context before
a subsequent request or background analysis, preserving unsaved documents.
Cancelling recovery before commit retains the previous driver as invalid rather
than publishing a result through it; a later request retries recovery.
