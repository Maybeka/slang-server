# New-project optimization and Mac qualification

Experiments remain disabled by default. The qualified implementation is recorded
by binary and source hashes in
[data/new-project-shared-semantic-warmup-2026-10-30.json](data/new-project-shared-semantic-warmup-2026-10-30.json).
Installation and cache lifecycle checks are recorded in
[data/new-project-final-delivery-2026-10-30.json](data/new-project-final-delivery-2026-10-30.json).
Older data receipts retain the intermediate findings and rejected prototypes.

## Result and coverage

The corpus includes OpenTitan SoC, official OpenTitan AES/UART UVM verification
filelists with generic primitives and bind tops, small AES, Ethernet MAC/PTP/UDP,
OpenOFDM receiver/demodulator, SerDes PCS/8b10b, and CVA6 with UVM. Inputs and
pinned project revisions are retained locally; repeated measurements require no
new source or dependency downloads.

The final Mac candidate passed 1,020 CTest checks, 31 targeted ASan cases with
3,726 assertions, 1,710 release protocol states, and 180 ASan protocol states
with 12 normal exits. Public hover results and complete diagnostic maps matched
the experiments-off controls. ASan leak detection was disabled. The unchanged
SDK patch was verified against all 709 source hashes and reconstructed from local
Git objects without network access.

The implementation addresses four connected costs:

1. Full-model work: release unused cached query views and obsolete sole-owner
   models before allocating replacements; retain genuinely borrowed contexts.
2. Query certification: use the current local parse for unchanged method
   prototypes, retain documentation and macro-view behavior, and capture an
   unprepared pre-edit diagnostic owner without analyzing it on the foreground.
3. Compatibility: replay Native SourceLoader contexts and persistent preprocessing
   state; close array/type/system-function, bind, class, interface, and separately
   rooted RTL dependencies before semantic reuse is admitted.
4. Lifetime and interruption: retain CST source/macro storage with the tree,
   prepare the same retained diagnostic owner in background, and check for newer
   input between completed lazy binding operations. Native semantic checks and
   instance canonicalization remain authoritative.

No new experiment switch is enabled by these changes.

## Whole DUT and UVM freezing

AES and UART retain and actually reuse the complete `tb.dut` root together with
UVM. The structural-edit case exits local method updating while preserving those
fixed providers. Sequential single-repeat measurements on the final binary:

| Workload | Experiments off: query / diagnostics | Frozen providers plus local-update fallback | Stable local edits |
| --- | ---: | ---: | ---: |
| OpenTitan AES + UVM | 2469.77 / 2643.10 ms | 80.42 / 254.49 ms | 2.20 / 3.47 ms |
| OpenTitan UART + UVM | 2119.27 / 2265.01 ms | 109.75 / 254.21 ms | 2.28 / 3.61 ms |

Diagnostic time starts at the same edit as query time; do not add the two.
These are initialized-session measurements, not cold-disk or simulator execution
results. Diagnostic parity includes the Native workload's existing diagnostics;
it is not a claim that every external workload compiles without errors.

The large OpenTitan SoC freezes only `top_pkg` and `prim_util_pkg`; it does not
prove whole-SoC freezing. Ethernet and SerDes measurements cover selected RTL
subtrees and contain no UVM. The full OpenOFDM receiver retains its documented
semantic-freeze boundary and uses correct Native source replay. Editing the UDP
fixture's fixed `lfsr.v` dependency remains an expected manual-rebuild refusal,
not a successful latency sample; a separate mutable-consumer fixture is measured.

## Navigation followed by editing

A Native query outside the edited method can replace a full model without
preparing its diagnostics. A matching source version and publication generation
alone must not make the next edit perform full semantic/dataflow analysis on the
foreground. The certificate now retains that exact pre-edit owner and witnesses;
background work completes it and applies the local diagnostic update.

In the earlier three-repeat exact-control study, large-SoC next-edit hover fell
from 642.60 to 35.50 ms, diagnostics stayed about 704–708 ms, and boundary process
footprint stayed about 587 MiB. A guard-only prototype was rejected because it
retained two full models and raised diagnostics and memory substantially.

The final preparation stage additionally checks for newer input between complete
lazy bindings. It never throws a cancellation exception from inside a shared
binding operation. Native parameter/interface keys select representative warmup
work only; the normal complete semantic pass still decides instance reuse and
all diagnostics. Generic specialization pointers are copied before bindings can
extend their map. Imported owners remain immutable.

Three alternating runs with pre-encoded newer input gave these large-SoC medians:

| Metric | Previous qualified control | Final candidate |
| --- | ---: | ---: |
| Latest hover after newer input | 361.08 ms | 17.60 ms |
| Latest complete diagnostics | 636.18 ms | 689.86 ms |
| Process footprint at that boundary | 585.74 MiB | 600.85 MiB |

Actual delivery was approximately 83–90 ms after the first input, with host
scheduling variation recorded per sample; this is not an exact 80 ms deadline
claim. The earlier sender performed diff/encoding work after its timer; those
measurements are retained separately and not relabeled as identical schedules.
A shorter-arrival control measured approximately 27–30 ms delivery and 16 ms latest
hover. Three-repeat stable editing remained about 16.5 / 31.7 ms for the large SoC
and 6.5 / 11.6 ms for CVA6. With no intervening new input, the SoC's first background
diagnostic preparation increased about 44 ms; stable local diagnostics did not.

The tradeoff is a modest one-time diagnostic/footprint increase for a much shorter
foreground wait. One complete binding operation remains indivisible; there is no
universal latency bound for arbitrary source. Process boundary footprint, model
allocation estimates and OS-reported peaks are distinct. These runs do not claim
independent high-frequency memory sampling or broad statistical confidence.

## Installation and reconstruction

A fresh install contains one bundled UVM 1.2 `preparsed/default.cst`. Installed
CVA6 checks used it with zero UVM syntax parsing and no project artifact. A custom
configuration in a fresh project created one project-local artifact; restarting
hit that artifact without changing its bytes. All three installed cases proved
actual CVA6 DUT semantic reuse and left original source/configuration untouched.

Changed-provider checks use isolated copies. After adding a member to UVM or a
stable package, the explicit rebuild command changes the semantic generation and
the new member resolves without errors. UVM's project artifact also changes.
The installed default configuration keeps method overlays, semantic freezing and
context retirement disabled. Current build/input caches remain reusable; raw logs
are replaced by conclusions, metrics and code-version receipts.
