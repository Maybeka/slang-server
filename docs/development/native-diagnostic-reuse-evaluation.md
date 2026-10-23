# Native diagnostic route reuse and exact file proofs: Mac CVA6

Qualification accepted. This is the first implementation stage of the active five-category
goal, covering initial input witnesses, first diagnostic certification and complete input proof
cost. The remaining local-update, scheduling and memory work stays open. The implementation
uses the existing `experimentalMethodBodyCertification`, which remains default off.
See the [feature contract](../features/method-body-certification.md) and
[five-category status](performance-roadmap-status.md).

Baseline main: `a8d2da918b3b8547182854b33d405f1c80e5d02e`; binary SHA256
`8b867738794e5cf7484a1332d37ee9122aa42c822e9c68f476aea2ea5150d77f`.
Candidate SHA256: `262099aa0c9949f8ceb7016143faa19d397395f61931739d9bdd219899475ac6`.
Both arms enable the same existing experiments, including body certification, certificate
renewal, cooperative analysis, UVM semantic reuse and whole-DUT reuse. Native disables body
queries/diagnostics. All arms use the same physical UVM sources from the qualified responsiveness
installation. The original CVA6 plus complete core-v-verif project retains 258 roots, 404 DUT
instances and 612 diagnostic URIs. Cached CMake version `49569ff` is not the artifact identity;
source/commit/binary receipts bind the actual candidate.

## Implementation

The real diagnostic client records physical routes only for diagnostics that reach an LSP
bucket. The native publication records the exact raw syntax/semantic/dataflow batches while
issuing them; it does not reconstruct identity using temporary addresses or Diagnostic's partial
logical equality. A cancelled publication discards its draft and rolls back its rendered map;
scoped observers are restored. The one committed snapshot has weak compilation ownership.
A consumer requires the same owner, context, generation and configuration before using it.
Current editor aliases are applied to physical routes on consumption. Capturing the pending
baseline transfers the driver's raw snapshot, so successful certification or decline releases
the extra whole-project raw copy. Original detached-versus-native diagnostic equivalence and
final input/version validation remain mandatory.

Selecting the active DUT refreshes shallow views without dropping a still-current complete
native publication. This was a real-project gap in the first implementation: unit tests that
did not select a DUT passed while CVA6 silently used the old formatting path. The selection
sequence now has a dedicated generated test. Native reanalysis/source invalidation still clears
or fails the snapshot proof. Retained diagnostic source classification caches physical-path
membership per BufferID within one constructor, including newly encountered synthetic buffers.
Driver diagnostics are coalesced once per complete publication rather than once for logging
and again for issue.

Initial witnesses read each path once under the experiment. macOS full proofs use descriptor
path resolution, read every byte and check EOF, regular-file type, current descriptor/path
identity, size and observed modification during the read. This replaces repeated canonical
ancestor traversal; it does not cache a metadata-only success proof. Other platforms and
unsupported descriptor path queries use the original portable reader. Directory entries,
missing include lookups, configuration and open-document witnesses are preserved. Body install,
query return and diagnostic commit each keep their full publication proof. Filesystem checks
remain observations at checkpoints, rather than an atomic snapshot of all project files.

## Ordinary latency

| Scenario | Final hover, baseline → candidate | Final diagnostics, baseline → candidate | Successful/cancelled queries |
| --- | ---: | ---: | ---: |
| Historical cold, 12 inputs | 37.87 → 19.86 ms | 889.26 → 844.76 ms | 35/1 → 36/0 |
| Eligible first single input | 93.72 → 42.62 ms | 232.12 → 67.70 ms | 3/0 → 3/0 |
| Eligible first burst, 12 inputs | 33.21 → 24.82 ms | 172.41 → 36.54 ms | 36/0 → 36/0 |
| Warm, two priming edits then 12 inputs | 37.06 → 25.94 ms | 56.54 → 38.07 ms | 36/0 → 36/0 |

Historical multiline edits with new external references and later macro line shifts remain in
the matrix and retain native fallback. The eligible first variants preserve line count and
external-reference usage; they do not replace that original scenario. No phase trace or memory
sampler is enabled in ordinary latency runs; diagnostic delta is off. Short scenarios use three
paired sessions, reversing order in the second. Independent input arrives every 100 ms.
Successful responses are compared with Native; obsolete cancellations are reported separately.

## Long session and memory

One 1,008-input ordinary session per arm uses reverse order. Successful hover p50/p95 are
32.82/59.39 → 24.74/32.36 ms. Both succeed all 1,008 times.
The final hover is 30.26 → 21.21 ms and final diagnostics
45.93 → 31.65 ms. Both accept 1,010 edits including
two primes and cross 15 renewals with zero retained legacy handles. Local diagnostic updates
are 999 → 1009. All successful hovers and complete terminal maps match Native.

The separate global-macro fallback session has final hover
949.33 → 959.99 ms and diagnostics
1268.89 → 1278.20 ms.
Cancellations are 7 → 6; final requests succeed. This one-session fallback safety observation
is not evidence that full semantic compilation has been accelerated. Long-session observations
are finite samples, not an indefinite tail distribution.

Memory probes are separate, with delta publishing, phase tracing and 20 ms server-family RSS
and physical-footprint sampling. Python is excluded. The 3 GiB/180-second per-arm guard is an
evaluation limit, not a configurable process-RSS guarantee or proof of indefinite memory bounds.

| Probe | Peak family RSS, baseline → candidate | Peak footprint, baseline → candidate |
| --- | ---: | ---: |
| 1,008 inputs; baseline first | 1402.48 → 1412.42 MiB | 1340.00 → 1516.61 MiB |
| 1,008 inputs; candidate first | 1386.12 → 1434.34 MiB | 1512.78 → 1464.80 MiB |
| Forced retirement every 3 edits; 12 inputs | 1490.53 → 1528.34 MiB | 1364.74 → 1183.16 MiB |

The first footprint probe regresses by about 177 MiB; reversing order changes the sign
(candidate lower by about 48 MiB). Both observations remain in the results. They do not
establish a memory win. Natural successful retirements are one per arm per order; cancelled
replacements increase 284→365 and 274→356. These differ from query cancellation: 3→0
and 3→1. All final queries/maps and guards pass, with zero delta-mode post-input idle
publications. Repeated candidate preparation and old/candidate overlap remain category 4 work.

In the forward sampled probe, completed file checks have medians
15.50 → 7.23 ms (2957 → 3028 stages). Stage counts can repeat
within a request and are not disjoint end-to-end samples. The eligible first mechanism probe
records matched native publication, zero retained-route formatting, and first certification
of 14.54 ms; the earlier attribution probe needed about 121 ms.
Initial witness construction is 44.28 → 22.86 ms in that one paired traced probe.

The source charge remains 32,850,221 bytes across repeated cancellations despite rising
entry count, because table capacity contributes to this metric. Final successful replacement
reduces it to 20,499,561 bytes. SourceManager accounting also needs scrutiny for query data
not in its lookup-cache metric. This evidence motivates growth/work-aware retry admission and
separate source-storage accounting; neither is implemented in this stage.


## Correctness and delivery

Five new cases cover native route reuse, DUT selection, initial witness read reduction,
path/content mutation after capture, cancelled native publication and observer restoration,
editor aliases and macro diagnostics, and file/parent symlinks, nonregular or missing inputs.
Existing cancellation, error/repair, same-size/same-mtime mutation and hard-retirement coverage
also runs. Related tests pass 1,218 assertions in 23 cases. The early-abort mutation case checks
correct rejection; read counts are compared only for two successfully completed constructors.

The final binary passes 797 CTest entries, 17 Python tests and 43 transport cases. SDK clean
archive plus restoration patch reproduces 709 files byte for byte without changing gitlink
`151a801ceaf6faf23d79a42d210ff67e50eb4660`. No SDK modification is introduced in this stage.

The public oracle passes 42 states across both query orders: hover, completion/resolve,
definition, source/expanded/both macro previews, repeat queries, errors/repair, global macro
fallback and complete diagnostic maps. Installed default UVM uses shipped preparse with zero
syntax parse time; the external profile hits the project cache with zero syntax parse time.
Both installation checks also require a hit on the retained entire-DUT root, no body-cache
declines and successful UVM/scope preparation.

Instrumented public-oracle setup through first diagnostics is 2949.72 →
2913.25 ms (two sessions per arm). This includes setup, source loading and analysis;
it is a safety observation under memory sampling, not an ordinary startup benchmark.

An intermittent parallel-build race was also reproduced: test and production executables copied
UVM resources into the same runtime directory concurrently. Staging targets now depend on the
preceding staging writer; compilation remains parallel. Subsequent parallel builds pass. The
package continues to contain one default preparse; other configurations retain project caches.

Raw scripts, final results and receipts are in `untracked/native-diagnostic-reuse-2026-10-23/`.
Earlier non-hitting and attribution probes are separated under `pre-attribution/` and are not
pooled with the final-binary latency results. The active total plan and follow-up closure,
scheduling and memory audits are in `untracked/five-category-completion-2026-10-23/`.
The total five-category goal remains active after this stage.
