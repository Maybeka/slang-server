# Optional real DV workloads

These are explicitly invoked language-server integration workloads. They are not
CTest dependencies, do not require VCS to run, and do not simulate the designs.
They use the HDL order and include inputs emitted by official FuseSoC simulation
targets, generic primitives, the project's real verification classes, and the
server's UVM sources. No package/class stubs or warning suppression are added.

The current runner targets macOS because its memory sampler uses the Mac SDK's
`rusage_info_v4`. Measurements distinguish RSS, physical footprint and lifetime
peak physical footprint. Python preparation additionally requires PyYAML, normally
available in the environment used for FuseSoC setup. The runner itself uses the
standard library and the repository's framed LSP client.

## Prepare once, replay offline

Use a pinned OpenTitan checkout. The evaluated revision is
`8e7f977585b8e8304feaf688d7e8429059bfd097`. Generate setup-only EDAM directories
named `<profile>-setup` under a work directory, using this pattern:

```sh
fusesoc --cores-root /path/to/opentitan run --target=sim --tool=vcs \
  --mapping=lowrisc:prim_generic:all:0.1 --setup \
  --build-root=/path/to/setups/opentitan-usbdev-setup lowrisc:dv:usbdev_sim
```

| Profile | Official target |
| --- | --- |
| `opentitan-usbdev` | `lowrisc:dv:usbdev_sim` |
| `opentitan-spi-host` | `lowrisc:dv:spi_host_sim` |
| `opentitan-spi-device` | `lowrisc:dv:spi_device_sim` |
| `opentitan-otbn` | `lowrisc:dv:otbn_sim` |
| `opentitan-dma` | `lowrisc:dv:dma_sim` |
| `opentitan-chip` | `lowrisc:dv:top_earlgrey_chip_sim:0.1` |

Prepare all six generated setups, then invoke from the repository root
(`--profiles` can restrict preparation to named profiles):

```sh
python tests/large_dv/prepare.py --setup-root /path/to/setups \
  --output-root /path/to/recipes --uvm "$PWD/resources/uvm/1.2/src" \
  --repository-root /path/to/opentitan \
  --repository-commit 8e7f977585b8e8304feaf688d7e8429059bfd097
python tests/large_dv/run.py --manifest /path/to/recipes/large-matrix.json \
  --output-root /path/to/results --case opentitan-usbdev --scenario real-edit \
  --label comparison-01 --binary /path/to/slang-server --repeats 3
```

The SPI device profile selects the official `SramType2p` build define; chip DV
includes the official `TOP_LEVEL_DV` build option. Both originate in DVSIM HJSON
configuration outside the FuseSoC core file. The input receipt identifies those
configurations. EDAM alone is not the entire simulator command line; the explicit
analysis flags and macros are recorded in the generated filelist and manifest.

The manifest records absolute paths to the local inputs. Regenerate it after
moving a checkout or setup. Preparation fails if a listed HDL input is absent.
`large-inputs.json` records EDAM and source hashes, loaded-filelist scale and
lexical class declarations. Those counts describe physical input files, not the
number of elaborated class specializations or instances. The UVM library is
reported separately by the server's library status.

Native, freeze and combined arms use the same source index, flags, thread count
and cooperative scheduling. The latter two explicitly enable experiments with
unlimited semantic test caches. The source files are never written: edits exist
only in LSP documents. The sequence covers six successive method-body edits,
line insertion, an intentional error, repair, a structural edit, close/reopen,
manual rebuild and an edit after rebuilding. Public hover and the complete
observed diagnostic map must match Native at each state. Errors and repair must
be observed, and noninitial frozen states must demonstrate actual semantic reuse.

Use `--scenario background` to edit a separate consumer with the complete DV
design loaded. Use `--scenario real-edit` to edit the configured DV source in
the editor. The two scenarios exercise different source-ownership and
preprocessing boundaries. Verify reuse while editing the DV source through
the real-edit scenario, especially when its file is inside the fixed closure.

For an explicitly unsupported profile, `--allow-native-fallback` checks the same
exact public-result oracle while permitting native analysis. The report labels
those states `native-fallback:<category>` and keeps the import/coverage status.
Preparation and actual reuse are reported separately. The evaluated SPI device
analysis profile has 14 initial Native errors. The oracle preserves existing
design diagnostics and separately verifies the inserted error and repair.

`queryMs` ends at receipt of the complete hover response for a batched change and
query. `diagnosticMs` starts at the same dispatch and ends at the matching
edited-document version publication; it is not global diagnostic completion.
These intervals overlap. Process startup runs use a new process but do not flush
OS caches. Serial awaited edits do not establish typing-burst or p99 latency.

Use `--readiness` while other tests/builds are active; such measurements are
explicitly unqualified. Qualified timing must run serially after other work ends.
Labels cannot overwrite a previous result. Preserve failed results and diagnose
them before selecting any follow-up experiment. Remove ended stderr/build logs
after recording the conclusion, input/code identities and useful measurements.

## Newly exposed concerns

The six profiles exercise configurations of the same pinned OpenTitan codebase.
The original quality evaluation recorded these cases:

- Chip DV exposed generated command-line macro buffers in diagnostic links. A
  small `[generated-macro-note]` C++ test now checks both retained/replayed
  generated locations and real macro-definition locations.
- DMA exposed a warning representative difference between 32-bit and 64-bit
  generic class specializations. OTBN exposed a forwarding type-alias spelling
  difference in a `dynamic-cast-const` warning. The semantic reuse work fixes
  representative selection and diagnostic type rendering, with exact payload
  comparisons for both profiles.
- The chip profile can report a Native `$fatal` during constant evaluation.
  Declaration projection and closed-child admission can preserve reuse beside
  that diagnostic. The status separates reused descendants from the enclosing
  design and from structural edits that need native analysis.
- SPI host/device exposed `unclosed-global-name` dependencies. The fixed
  namespace witness now admits independently proven closures and reports
  remaining refusals. Both SPI profiles, DMA, OTBN and USB device pass the
  strict background-edit lifecycle with actual RTL imports and local updates.

See the [semantic reuse plan](../../docs/development/semantic-reuse-completion-plan.md)
for the ownership contract and release qualification.

These additional corpora remain outside the routine test suite. Reduce concrete
new lifecycle or language failures to independent regression tests as they are
addressed; do not add whole external projects to ordinary CTest discovery.
