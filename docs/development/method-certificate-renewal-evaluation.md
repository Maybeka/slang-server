# Method certificate renewal: Mac CVA6 evaluation

Status: accepted. `experimentalMethodBodyCertificateRenewal` defaults to false.
See the [feature contract](../features/method-certificate-renewal.md) and
[five-category status](performance-roadmap-status.md).

Baseline main: `2db54246c5bc1d6fa0c4d3fb795dcbca21ec7814`; installed binary SHA256
`5b74a4ed82499a2bf01c53e0ef95efb41b94319e1b71c2e0505b30c95678f2cf`. Candidate SHA256:
`07dfe95cf883358c2d6b0655d565daa09a88d8ca2249d2f9b6cafb3966b4d228`. Its embedded version remains the cached CMake string `49569ff`;
artifact identity is bound by SHA256. Both arms enable cooperative analysis, method-body
queries/diagnostics, UVM semantic reuse and whole-DUT semantic reuse. The original CVA6 +
full core-v-verif workload has 404 DUT instances and 612 diagnostic URIs; UVM source paths
are identical across arms. Native uses the candidate with body queries/diagnostics disabled.

## Implementation and safety

Each accepted edit still validates the original lexical owner, configuration, context/index,
open-document witnesses, directory contents and disk bytes. Method exterior, signature,
preprocessor-state and external constant-dependency checks remain. A still-valid certificate
starts another 64-edit accounting window without dropping its original lexical owner or
completed diagnostic baseline. Each current body replaces its predecessor; there is no chain
of edited semantic owners. Exact-version completion tickets replace the unused general-body
buffer-ID history. The legacy overlay retains its independent bound.

Standalone renewal also activates source-storage retirement without requiring a UVM/RTL cache.
The ordinary trigger, cooperative replacement and synchronous hard fallback remain; no fixed
debounce is added. Renewal does not preserve certificates across driver swaps or remove
native fallback and complete semantic elaboration costs.

## Ordinary latency runs

Edits arrive independently every 100 ms. No phase trace or memory sampler is enabled;
diagnostic delta publishing stays off. Each long length has one session per arm, so these
are finite observations, not an estimate of an indefinite tail distribution. Native supplies
all 12 repeated expression results and the complete final diagnostic map.

| Inputs | Last hover, baseline → renewal | Last diagnostics, baseline → renewal | Successful / cancelled queries, baseline → renewal |
| --- | ---: | ---: | ---: |
| 96 | 960.98 → 67.34 ms | 1288.42 → 111.94 ms | 63 / 33 → 96 / 0 |
| 1008 | 1082.60 → 87.15 ms | 1419.41 → 121.15 ms | 63 / 945 → 987 / 21 |

Every final request succeeds. Every successful hover and complete 612-URI terminal diagnostic
map matches Native. The long candidate accepts 1010 edits including two priming edits,
completes 15 renewals, retains zero legacy buffer handles, and publishes
800 local diagnostic updates. Its final idle check preserves the diagnostic map.
Cancellations above are obsolete-content responses, not successful low-latency responses.

Four short warm sessions use two forward and two reverse arm orders. All 48
queries per arm succeed; pooled medians are 51.925 → 47.811 ms.
This is not a claim that renewal accelerates edits below the old ceiling. Primary benefit is
retaining local analysis beyond that ceiling. Individual paired warm medians are retained
in the evaluation data. None of the four paired medians regresses by 10%; additional
balanced 48-edit sessions were therefore not required for the final binary.


## Separate memory and mechanism probes

These enable delta publishing, phase traces and 20 ms family RSS/physical-footprint sampling.
The family includes server/diagnostic children and excludes Python. Their timings are not
pooled into ordinary latency results. The 3 GiB/180-second per-session guard is a test guard,
not a configurable process RSS promise.

| Probe | Peak family RSS, baseline → renewal | Peak footprint, baseline → renewal |
| --- | ---: | ---: |
| 1,008 continuous edits | 1542.11 → 1529.84 MiB | 1134.00 → 1140.70 MiB |
| Forced retirement every 3 edits; 12 inputs | 1521.67 → 1521.44 MiB | 1177.53 → 1177.56 MiB |

In the long probe, baseline records 23 successful retirements;
candidate records 148 cancelled replacements and
1 successful retirement events. Query cancellation is separate:
945 baseline versus 52 candidate queries.
Final requests and all complete diagnostic maps pass. Delta publishing yields zero additional
publications in all post-input idle checks. Forced retirement remains operational. All probes
remain within the guard. Source storage still grows and private replacements overlap the old
owner; these observations do not establish an indefinite process-memory bound or a memory win.

The candidate records 4585 completed file-validation stages with
15.406 ms median. Local parsing/binding medians are
0.277/0.129 ms; apply revalidation remains
15.738 ms. Stages can repeat within a request; they are not
disjoint end-to-end samples. Repeated input certification remains the next priority.

## Correctness and delivery

Three new cases pass 2,498 assertions: disabled bound, 193 edits over three renewal boundaries,
user errors/repair and line shifts, driver/epoch continuity, stale completion tickets,
cancellation during renewal, signature/exterior/dependency/macro invalidation, and standalone
hard retirement without any library experiment. The dependency witness rejects reuse before
a watcher event; native caches then receive the editor's file event before full comparison.

The original-project public oracle passes 42 states across both query orders: hover,
completion/resolve, definition, all three macro previews, repeat queries, user error/repair,
native macro fallback and complete diagnostics. Full validation passes 785 CTest entries,
17 Python tests and 43 transport cases. Clean SDK archive + patch replay verifies all 709
inventory files byte for byte without changing gitlink `151a801ceaf6faf23d79a42d210ff67e50eb4660`.

The installed default UVM reports shipped preparse and zero syntax parse time; the external
profile hits its project cache with zero syntax parse time. Both UVM and whole-DUT semantic
reuse pass. One default preparse still ships; other profiles remain project-local caches.

Raw evidence/scripts are in `untracked/certificate-renewal-2026-10-23/`; the tracked
[evaluation data](../../validation/data/method-certificate-renewal-2026-10-23.json) binds final
binary identities, results and raw artifact hashes. Initial pre-guard measurements are retained
separately in `pre-guard/`; they are not pooled into final-binary results. Temporary files are
cleaned and active sources, reusable caches, qualified installations and raw evidence retained.
Experiments remain default off. First certification and native fallback remain open work.
