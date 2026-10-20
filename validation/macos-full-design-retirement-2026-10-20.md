# Full-design context retirement on Mac

The tested executable SHA-256 is
`fd97e0ff81a97ab9c2cbea0eb3c780042e0e23048b32bb6117b20d9a42999815`.
The candidate manifest/archive pins implementation sources; only repository
Markdown changed after freezing. This milestone extends lifetime management;
it does not complete integrated performance or publication acceptance.

## Implementation and correctness

Either experimental cache now permits growing parser contexts to retire in full
builds and selected-top mode. Both cache switches still default off. The existing
SourceManager source/location-table growth trigger is retained: at least 16 MiB
or twice the fresh context's reported size. A trigger can be exceeded by one
allocation/query before the next edit boundary; it is not an RSS cap.

The replacement retains the current build/top choice, all unsaved documents and
versions, and valid active instance/generate paths. Paths are resolved against the
new compiler; no AST pointers or physical source IDs cross owners. Completion
epochs fence bare physical handles and permit supported current logical lookup.
A failed filelist or top replacement retains the already edited owner and can
retry after repair. A package-only syntax tree cannot masquerade as a top module.

Extending this path exposed two omissions: legacy-UVM-only replacements did not
restore active selections, and top-tree wrappers discarded open-document versions.
Both are fixed. `sources.sourceContext` reports source bytes, source-location-table
bytes and entry count independently of retained cache bytes or process RSS.

Two focused cases pass 534 assertions. The matrix covers configured builds,
interactive filelists and selected tops, with experiments off, legacy UVM cache
on and generic freezing on. It checks two replacements, multiple unsaved files,
updated RTL widths, preserved active selection, old completion resolution/fencing,
matching-version user errors and repair. Failure tests cover unavailable filelist
inputs and temporarily missing top modules, old-owner retention and successful
retry. The initial overly strict empty-diagnostic assertion was corrected to allow
existing native warnings; current native/enabled public diagnostics are compared
without suppression in the real replays below.

Full CTest passes 684/684; Python passes 16/16; the focused group passes 7,180
assertions in 96 cases; standalone transport passes 43/43. Six real SPI/PSS/UBUS
native/enabled processes compare all five unsaved states, including hierarchy,
completion/every resolve, definitions, hover, references, highlights, symbols and
complete current-version diagnostics. Each enabled process is forced through two
actual replacements; native off has none. Public responses match after removing
only opaque completion data and normalizing outer diagnostic order.

Six isolated native/enabled LazyVim sessions also pass on this executable, with
actual Mac file monitoring and two forced replacements per enabled session. Five
unsaved states check hierarchy/steps, matching diagnostic versions, user errors
and repair, and continued cross-edit reuse after replacement. The first editor
fixture had only one compatible edit interval between two forced replacements,
so its unchanged three-hit requirement could not be met; a fifth compatible edit
was added instead of lowering that requirement. Initial failure evidence and
helper versions are retained. No user editor configuration or state is changed.

## Natural lifetime: first 1,000-edit runs

Three separate enabled processes run 1,000 alternating unsaved task edits, followed
by user-error insertion/repair, declaration fallback/repair and manual refresh.
No forced-edit threshold is supplied. All pass. Hierarchy values are checked at
every state against the initial complete result, and current-version diagnostic
errors are checked at every edit. This is one repeat per project, not the required
three-repeat final comparison.

The improved sampler begins before process spawn, samples every 20 ms and excludes
the Python harness. It reads RSS and physical footprint through the local Mac SDK's
libproc interfaces. The harness drains obsolete diagnostic notices while retaining
latest-per-URI evidence. Family sums can count shared pages, and sampling can miss
peaks. Full-design diagnostics use the parent, so no diagnostic child is observed;
family peaks equal parent peaks here. Exploration worker-family coverage remains
separate.

| Project | Successful natural retirements | Peak RSS incl. startup | Peak physical footprint incl. startup |
|---|---:|---:|---:|
| SPI | 8 | 230.0 MiB | 162.8 MiB |
| PSS | 8 | 249.5 MiB | 143.2 MiB |
| UBUS | 111 | 257.9 MiB | 161.0 MiB |

Source/location counters repeatedly return to fresh-context sizes. This directly
confirms replacement rather than inferring it from a later RSS decline. The older
200-edit pilot had different sampling/notification handling and captured peaks
only after initialization, so it is not a paired numerical comparison to this run.
The first short forced sampler smoke also passes four actual replacements.

## Next work and remaining acceptance

UBUS's 111 retirements are a remaining cost. Its selected `dut_dummy` declaration
is in the edited final root. The plan currently selects both ordered roots:
`uvm/uvm.sv` and `examples/ubus_tb_top.sv`, then declines the whole group because
there is no editable suffix. It therefore has zero syntax preparations/resumes
and repeatedly parses the unchanged earlier UVM root. A stable configured-UVM
dependency prefix should be reused while the edited selected-root suffix is
parsed natively, with exact witnesses, macro-override decline and truthful status.
Reducing this work is preferable to claiming that frequent replacement alone
solves overall performance.

The goal still requires three-repeat memory comparison, the accepted older
baseline, 20 paired first/continuous/fallback timings and the older PSS/UBUS class
bulk-resolve regression. No new latency claim follows from these sampled memory
runs. All experiments remain off and no main push occurred for this milestone.
