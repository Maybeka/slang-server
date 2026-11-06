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

For an explicitly unsupported profile, `--allow-native-fallback` checks the same
exact public-result oracle while permitting native analysis. The report labels
those states `native-fallback:<category>` and keeps the import/coverage status.
It does not turn a prepared cache or a requested freeze into evidence of reuse.
USB device exercises real retained RTL roots; SPI device exercises a global-name
dependency refusal, correct fallback, and subsequent local method-body updates.
The evaluated SPI device analysis profile has 14 initial Native errors; the
fallback check preserves those diagnostics and separately verifies the inserted
error and repair. It does not certify an error-free simulator configuration.

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

The added projects are configurations of one OpenTitan codebase, not six
independent repositories. Not every configuration is a successful freeze case.
The versioned quality evaluation records passing cases and these limitations:

- Chip DV exposed generated command-line macro buffers in diagnostic links. A
  small `[generated-macro-note]` C++ test now checks both retained/replayed
  generated locations and real macro-definition locations.
- DMA exposes a preexisting warning representative difference between 32-bit and
  64-bit generic class specializations. The exact public-payload oracle rejects
  this result; it is not counted as diagnostic equivalence.
- OTBN exposes a preexisting generic type-alias spelling difference in a
  `dynamic-cast-const` warning. It is recorded as a mismatch, without normalizing
  or suppressing the warning to obtain a pass.
- The complete chip profile can decline fixed package preparation when constant
  evaluation enters `lc_state_to_bin` with an invalid state and executes `$fatal`.
  It continues on the native path; preparation and actual reuse must not be
  inferred from a requested freeze configuration.
- SPI host/device can prepare a fixed closure but decline importing it because
  of `unclosed-global-name` dependencies. Their fallback coverage and any local
  query/diagnostic reuse are reported independently of frozen-root reuse.

These additional corpora remain outside the routine test suite. Reduce concrete
new lifecycle or language failures to independent regression tests as they are
addressed; do not add whole external projects to ordinary CTest discovery.
