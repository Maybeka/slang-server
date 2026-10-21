# CVA6 RTL and UVM namespace follow-up

## Workload and controls

Mac evaluation on 2026-10-22 uses CVA6
`81245a47fad8fe1a5d562d953ef2662e099def76` and its pinned core-v-verif
`b3149ab0da3ac8bf2179e24595a4b7a860b668ff` dependency. The official core,
core testbench and UVM filelists expand to 258 roots and 612 diagnostic URIs.
The configuration is `cv64a6_imafdc_sv39`, with no `VERILATOR` define, verification
source repairs or diagnostic suppression. All dependencies were reused locally.

The selected RTL instance is
`uvmt_cva6_tb.cva6_dut_wrap.cva6_tb_wrapper_i.i_cva6`; the other selection is
`uvm_pkg`. Consumer edits change a class in `uvme_cva6_env.sv`. Native here means
the **same server binary with both semantic experiments disabled**, not an
upstream-server comparison. Every arm uses the shipped default UVM CST artifact.
Experiments remain off by default; the fixed-owner budget remains 256 MiB.

## Corrections covered

Fixed compilation-unit classes, helpers, types, values, DPI declarations and
package imports now remain available in new consumer units. Imported declarations
retain their lexical owner; import lookup records and their indices belong to the
current unit. Package projection cannot discard that fixed namespace. Explicit
`$unit::` lookups bypass package-local shadows during dependency checks.

Configured package inclusion also takes precedence over cached nearest-file
discovery. This fixes an actual pair of same-named core-control packages: Native
previously returned 171 receiver members initially and only 47 after editing.
All six current states return the correct 171 members.

Independent RTL contexts preserve inherited bind-target flags. An unrelated
consumer error does not reject every body, while local errors, unscoped diagnostics,
actual bound dependencies and unproven effects remain protected. Immutable UVM
parse-option views can be rebound after source-context retirement. UVM status
reports the shared scope owner when that owner supplies UVM.

## Measured response times

Final binary SHA256:
`a7139b85af92a989dc9c7d65c9f18d24f27bef26bee8e1379cf428a5a5b73c1d`.
Five randomized paired blocks use 15 fresh sequential processes and 90 edit
states. Filesystem caches are warm. No build, other test workload or memory
sampler runs alongside these measurements. Values below are medians in ms.
Continuous values first take a per-session median over ordinary edit/repair
states. Full-graph timing waits for every expected diagnostic publication after
an edit and immediate hover, without a timed `didSave`.

| Measurement | Semantic caches off | UVM only | UVM + fixed RTL namespace |
| --- | ---: | ---: | ---: |
| Project setup | 2331.3 | 2277.5 | 2954.3 |
| First open query after setup | 1547.0 | 1824.3 | 1152.5 |
| Setup + first open query, per-session sum | 3856.9 | 4058.9 | 4118.4 |
| First subsequent edit query | 1050.8 | 893.8 | 677.3 |
| Continuous edit query | 1004.1 | 887.8 | 685.8 |
| Continuous full-graph diagnostics | 2098.9 | 1945.7 | 1682.1 |
| Injected-error full-graph diagnostics | 1947.5 | 1890.2 | 1554.7 |

The combined arm improves continuous query latency by about 32% against caches
off and 23% against UVM alone. Full-graph diagnostics improve by about 20% and
14%, respectively. Initial setup costs more; the combined first-use measurement
does not establish a startup win. First-open scheduling varies across processes.
These results do not establish cold-disk performance or long-lifetime percentiles.

Every combined process records 54 real RTL body hits over six edits and zero
namespace declines in the timed edit sequence. The gain combines fixed package
and definition sharing with eligible body reuse; it cannot be attributed solely
to those body hits.

## Memory and correctness

Memory is sampled separately at 20 ms over three paired blocks, nine fresh
processes and 108 states (12 edits each). All servers exit normally and all
observed owned PIDs disappear. Values are medians of sampled process-family peaks
or final samples, in MiB; sampled peaks are not hard memory bounds.

| Measurement | Semantic caches off | UVM only | UVM + fixed RTL namespace |
| --- | ---: | ---: | ---: |
| Peak RSS | 1353.1 | 1380.7 | 1408.3 |
| Peak macOS footprint | 1280.8 | 944.2 | 1029.9 |
| Final macOS footprint | 1280.8 | 673.5 | 708.4 |

RSS increases with retained semantics. The combined footprint is lower than
caches off and higher than UVM alone; this is a memory tradeoff, not a universal
memory reduction.

All six functional states compare complete completion lists and every resolution,
hover, definitions, references, highlights, symbols and the full diagnostic graph
against Native. The project's original 164 strict-language errors are retained;
the injected user error raises this to 165 and repair returns it to 164. This is
an LSP evaluation, not a clean simulation-build claim. Independent completion
details that load a default 32-bit configuration correctly fall back instead of
mixing that package with the project's fixed 64-bit configuration.

Acceptance also includes 737 CTest entries, 17 Python tests, 43 transport tests,
exact reconstruction of 709 SDK files, installed explicit/bundled UVM with default,
`UVM_NO_DPI` and converter configurations, and the earlier Verilator RTL profile's
six-state public/read/write/error-repair comparison.

## Remaining reuse boundary

The full-UVM DUT root is **not entirely body-reused**. External testbench type
arguments, beginning with `rvfi_probes_instr_t`, originate outside its fixed
owner. Unsupported calls and object effects also retain Native analysis. Fixed
declarations and eligible subtrees do share; the earlier Verilator profile still
admits an independent owner. Library preparation, declaration sharing and body
hits must therefore be reported separately. The next broader body-reuse work
must address stable nominal type providers and remaining certified effects.
