# Full-design memory pilot on Mac

The executable SHA-256 is
`0cf6e751d4c41151d5751bbc1599db60bf96f087e099d8c2b8cffb1ca32d11df`.
Four sequential processes cover PSS and UBUS, native/off versus exact instance
reuse/on, with 200 alternating unsaved task edits and one pilot repeat. The old
UVM-prefix experiment is off; enabled instances reserve 48 MiB for syntax and
16 MiB for hierarchy values/proofs. These are separate memory runs, not latency
acceptance. No builds, editors or primary timing run alongside them.

Each edit checks unchanged complete hierarchy values and matching-version user
diagnostics. After 200 edits, each run checks a missing user-task error and repair,
a task-local declaration that requires full proof rebuilding, repair and manual
refresh. All four runs pass; enabled runs have at least 200 cross-edit hits and
at least two full certifications after declaration fallback.

RSS is sampled every 20 ms for the exact server and descendants through libproc.
Sampling begins after initialization: peaks below are **post-initialization**,
not startup/whole-process peaks. Full-design diagnostics currently run in the
parent (`captureDiagnosticSnapshot` declines a full compilation), and raw samples
observe no diagnostic child. Family RSS therefore equals parent RSS in this
pilot. Exploration worker-family lifetime needs separate coverage.

| Project | Arm | Initial query RSS | Edit 50 | Edit 100 | Edit 200 | After refresh | Sampled post-init peak |
|---|---|---|---|---|---|---|---|
| spi-pss | Native off | 185.7 MiB | 346.7 MiB | 487.8 MiB | 420.5 MiB | 425.8 MiB | 740.3 MiB |
| spi-pss | Instances on | 169.7 MiB | 182.5 MiB | 192.4 MiB | 203.8 MiB | 228.8 MiB | 228.8 MiB |
| official-ubus | Instances on | 153.5 MiB | 327.1 MiB | 480.4 MiB | 451.5 MiB | 461.8 MiB | 742.5 MiB |
| official-ubus | Native off | 149.7 MiB | 323.1 MiB | 476.4 MiB | 332.5 MiB | 339.0 MiB | 726.0 MiB |

PSS benefits substantially from syntax retention in this workload; the experiment
still grows across edits. UBUS has similar growth in both arms and receives no
stable whole-session memory advantage here. A later RSS decline does not prove
bounded memory: resident pages can change under memory pressure/compression.
The retained-byte cache counters do not include all SourceManager/context state.

The existing retirement path explicitly skips full compilations and active
build/top-file selections. Extending context lifetime management to preserve a
full design, unsaved inputs, active instances, source epochs and diagnostics is
the next implementation task. UBUS also needs investigation of actual syntax
reuse boundaries rather than assuming a selected module retains an earlier UVM
prefix. These observations are not a universal memory-gain claim.

Final acceptance still requires three repeated 1,000-edit runs across SPI/PSS/UBUS,
startup capture, relevant exploration worker families, fallback/refresh and the
accepted older baseline. The final harness must also drain obsolete non-target
diagnostic notices while preserving latest-per-URI/current-version evidence;
otherwise its Python queue can increase memory pressure and search costs. Those
Python allocations are excluded from the server RSS measurements above, but
can still affect the machine. No latency inference is drawn from this pilot.
