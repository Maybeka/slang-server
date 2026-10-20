# Mac frozen package native-seed candidate

This phase-2 candidate is under validation and has not been published. Package
reuse remains experimental and off by default. Module definitions, instance
semantic reuse and the remaining macro-hover/bundled-library milestones are
separate pending work.

## Change

The parent registry can retain the initial native UVM root's immutable syntax
view and extend it through other selected package roots. It certifies all current
inputs before use and charges the full retained native owner. Unavailable or
oversized views use ordinary preparation. Diagnostic workers receive a negative
hint when the current parent analysis already declined reuse, avoiding repeated
failed preparations; a repaired group can prepare again. Independent completion
parses retain their CLI context and separate cache identity.

## Timing

Frozen candidate SHA256:
`7bf941d5fa7807e625fbab0512f08df085887e46a923dbf492c3bf887ce892a8`.
The private reproduction area is `untracked/frozen-registry-2026-10-20/` at the
workspace root. Inputs use the immutable 590-file SPI/PSS/UBUS corpus.

The confirmation contains 180 successful fresh processes: 20 randomized paired
observations for each of three cases and three arms. Each process performs ten
alternating unsaved edits, primary/repeated queries, selected completion resolve
and matching-version diagnostics. Primary time spans process start through final
complete diagnostics and excludes shutdown. No build, test, tracing or RSS sampler
runs concurrently. Filesystem caches are not flushed. All arms disable the older
UVM cache. The ablation keeps parent/worker generic reuse but disables initial
native seed capture. This is not a comparison against the older UVM-enabled binary.

| Scenario | Native whole session ms | Enabled ms | Seed disabled ms | Median saving | Paired mean saving 95% interval |
|---|---:|---:|---:|---:|---:|
| SPI definition | 2047.8 | 1228.1 | 1270.7 | 40.0% | 37.7–40.0% |
| PSS completion | 2517.0 | 1774.2 | 1795.9 | 29.5% | 28.3–30.0% |
| UBUS completion | 2112.2 | 1241.4 | 1294.5 | 41.2% | 40.4–41.6% |

| Scenario | Native first edit ms | Enabled ms | Seed disabled ms |
|---|---:|---:|---:|
| SPI | 51.0 | 41.4 | 78.1 |
| PSS | 52.5 | 42.3 | 77.8 |
| UBUS | 52.6 | 31.3 | 73.1 |

The native-seed refinement removes the previous first-edit preparation regression
in these cells. Initial process-start-through-first-response time still increases:
101.1→113.3, 104.3→118.4 and 96.0→108.8 ms. Cached repeats still incur exact-input
certification: 0.34→6.96, 1.69→7.83 and 1.86→6.07 ms. Thus the aggregate improvement
does not mean every request becomes faster. The paired bootstrap intervals use
10,000 resamples and do not establish tail guarantees.

Raw evidence: `native-seed-worker-confirm.json`,
`native-seed-timing-summary.json`, `analyze_native_seed_timing.py` and the frozen
candidate/source hashes. A separate trace-only mechanism check proves witnessed
native seed consumption plus one worker capture and three resumes in all projects.

## Validation state

- CTest 662/662, Python 16/16; broad focused group 1470 assertions/40 cases.
- Fresh SDK patch reconstruction agrees across 708 files.
- Isolated LazyVim checks pass completion/resolve/definition/hover through two
  unsaved edits on all three projects. The live user editor is untouched.
- Full current-candidate real replay passes all 60 processes, including every
  completion resolve, stale handles and extra public protocol comparisons. The
  16-process forced-retirement oracle passes with actual retirement observed.
  The known native UTF-16 W12 failure remains excluded and is not counted as a
  successful case; negotiated UTF-8 and CRLF cases pass.
- Separate three-repeat, 20-edit RSS runs pass all 18 sessions. Median parent
  peaks off/on are 161.9/68.1, 162.3/68.3 and 176.1/93.5 MiB; simultaneous family
  peaks are 247.4/173.4, 260.9/174.2 and 272.1/214.2 MiB. Sampling every 20 ms can
  miss peaks and family sums count shared pages. Raw compressed samples and
  retirement/status observations are retained in `native-seed-memory-short*`.
- All-item resolve timing completes 80 successful fresh processes, 20 paired
  observations per case/arm, with two unsaved edits. All three states resolve
  every completion: 171 items per PSS process and 393 per UBUS process. Complete
  process-start-through-final-diagnostic medians increase from 789.0 to 1834.8 ms
  and from 763.8 to 2554.2 ms. Paired mean increases are 131.7% (95% interval
  126.9–137.0%) and 231.2% (224.3–238.6%). Resolve-only medians increase from
  64.3 to 1267.3 ms and from 152.2 to 2132.2 ms. Per-request exact certification
  remains expensive; the selected-resolve timing benefit cannot be generalized
  to bulk resolve. Raw evidence is `native-seed-all-resolve-confirm.json`, its
  summary and the separate `evaluate_all_resolve.py` driver. No tracing, RSS,
  build or tests overlap these timings. This gate exposes a regression, not an
  accepted performance result.
- Older accepted baseline comparisons and the final 1000-edit integrated gate
  remain pending. The short RSS experiment does not prove the long-run bound.

The full goal remains active. Bulk-resolve certification still requires an
optimization or an explicitly documented measured usage tradeoff before final
acceptance. These results do not justify enabling the experiment by default.
