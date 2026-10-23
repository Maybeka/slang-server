# Cooperative analysis and context retirement (experimental)

`experimentalCooperativeAnalysis` defaults to `false`. It enables cancellation of
disposable syntax parses, including native configured-unit background reanalysis, and
cooperative background context retirement. Retirement still requires an existing UVM
prefix, frozen-source, UVM semantic, or scope semantic cache; it does not enable them.
A cancelled syntax parse keeps the newest editor text and pending diagnostics for retry.
It does not install a partial configured-unit list or disable an otherwise valid prefix.

With the experiment enabled, reaching the ordinary source-storage generation trigger
queues retirement after current diagnostics. The input notification keeps its current
query owner. Retirement builds one private replacement on the state worker. A newly
queued message cancels parsing at token checkpoints; all parallel source-loader workers
join before the candidate is discarded. The old owner, its query caches, current unsaved
texts, and published diagnostics remain available. There is no additional typing debounce.

A successful candidate checks cancellation before switching drivers. Diagnostic publication
ownership transfers only at that boundary. Cancelled or invalid candidates cannot clear
editor diagnostics. Invalid configuration waits for another edit before retrying, avoiding
an idle retry loop. Copied parser options release their request state after construction.

The ordinary trigger and its accounting are unchanged. If source storage grows beyond
twice that trigger while candidates keep being cancelled, retirement uses the existing
synchronous path. This is a source-storage bound, not a process RSS limit. At most one
candidate is built at a time; the server does not retain a chain of candidate generations.

Cancellation is cooperative. Syntax parsing checks every 1,024 returned tokens. Individual
lexer / macro operations and model construction still run to their next safe boundary.
Full-design semantic diagnostics retain their existing phase checks, and dataflow checks
between procedures, scopes, and assertions. This option does not make the complete compiler
preemptible or move shared semantic state onto a concurrent worker.

Enable it explicitly in `server.json`:

```json
{
  "experimentalCooperativeAnalysis": true
}
```

Existing body-query, body-diagnostic and stable-library options remain independent.
To suppress unchanged diagnostic publications across retirement, also enable
`experimentalDiagnosticDeltaPublishing`. The cooperative experiment preserves publication
ownership with that option both on and off, and does not implicitly enable it.
