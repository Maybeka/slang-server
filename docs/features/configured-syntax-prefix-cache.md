# Configured syntax prefix cache

This experiment is off by default. It reuses authenticated syntax from preceding
roots in a configured single compilation unit, reducing repeated parsing after a
structural edit. It does not freeze those mutable roots or reuse their semantics.

Enable `experimentalSyntaxPrefixCache` together with
`experimentalCooperativeAnalysis` and `experimentalScopeSemanticCache`. A prepared
fixed scope prefix is required. Keep the existing `frozenSources` selectors and
fixed dependency / manual rebuild contract for that fixed portion.

`syntaxPrefixCacheMaxMiB` defaults to 256; zero disables retention. It limits the
estimated retained CST owner beyond the already charged fixed prefix. One pending
or reusable owner is retained. Oversized or unsupported entries are abandoned.
This is a separate budget from `frozenSources.maxMiB` (fixed semantic and body
owners) and `sourceHistoryMaxMiB` (source storage). These are cache budgets, not a
process RSS limit.

The normal parse captures a root boundary. The first structural query does not
prepare a separate prefix view. A later replay authenticates the recorded input
bytes, aliases, failed include lookups and editor overlays before preparing or
using the view. Earlier edits invalidate it. Configuration reload or driver
retirement releases this driver-local state.

Callbacks, unsupported preprocessing state, and declarations spanning the
boundary use the ordinary parse. Complete balanced plain `pragma protect begin`
and `end` regions have no terminal protection state; encoded, nested, unbalanced,
reset and diagnostic pragma state are not admitted by this implementation.

`slang.scopeLibraryStatus` reports a separate `syntaxReplay` object with the
budget, retained charge, pending/cached root counts, captures, hits, refusals and
failed input witnesses. This change adds no fixed debounce.

Status: implementation under validation. SDK Native/ASan tests cover checkpoint
ownership and source boundaries. Full controller, CVA6 performance, diagnostics,
lifecycle and release qualification are still required before delivery.

Controller regression tests cover both included files and directly opened root
documents, cancellation before delayed extraction, earlier prefix edits, disabled
mode, zero budget and oversized-entry refusal. Final qualification is pending.

If the mutable input witness fails, this driver refuses further results through
its old file/lookup cache. The server recovers with a fresh input context before
a subsequent request or background analysis, preserving unsaved documents.
Cancelling recovery before commit retains the previous driver as invalid rather
than publishing a result through it; a later request retries recovery.
