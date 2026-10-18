# Linux validation records

The current capability acceptance is in [capability-acceptance.md](capability-acceptance.md),
with executed results in [linux-2026-10-18.json](linux-2026-10-18.json).
The follow-up [independent clean-checkout report](clean-checkout-2026-10-18.json)
closes the earlier configuration-only limitation: commit `9dfd335` was actually
compiled from zero product object/library files in a separate checkout and build
directory (887 build steps, 576 seconds). It passed CTest 607/607, Python 11/11,
transport 32/32, all oracle/count/held-diagnostic checks and static checks, plus a
fresh execution of the documented 216-request benchmark recipe. SDK/dependency
integrity and unchanged source state were checked again afterward.

That build reused the installed GCC/CMake/Ninja/Python tools, existing frozen-version
Python test packages and four hash-verified dependency source directories. It
reused no prior product objects, libraries or binaries. It was not a fresh OS/tool
installation or a new `uv sync`. No existing Neovim executable was available, so
real headless LSP attach/initialize/query smoke was not run; the Lua configuration
contract passed and interactive LazyVim remains unverified. See the report's exact
paths, versions, commands and limitations.

The [independent raw evidence archive](artifacts/clean-checkout-2026-10-18-9dfd335.tar.gz)
is retained in Git (400,026 bytes, SHA256
`b0ad3cf7e47d2d3a0094738e6729e69bee21c3cbe575a8b0add44a120a2dfd7b`).

The compact [raw evidence archive](artifacts/linux-2026-10-18-b194c8a.tar.gz)
is retained in Git (480,843 bytes, SHA256
`53633240ed8561e4ffaa1042e9f1d7b62830b05b9dba7574dfc9381f064fa502`).
[delivery-2026-10-18.json](delivery-2026-10-18.json) records source/bundle hashes
and the Library upload's network blocker. No Library success was returned.
The `b194c8a` binary passed CTest 607/607, Python 11/11 (including the shipped
Lua configuration contract), transport 32/32, the native oracle, parser counts,
and held diagnostics on the multi-header fixture. At the time of that earlier
report, the independent checkout had only configured successfully. The separate
follow-up report above records its actual full build and acceptance.

The 500-class experiment uses five class headers, a nested include, two packages
and three ordered roots. Each cell below is a median in milliseconds, n=6 per
arm per step; the middle column is the range of the seven per-step medians.

| Query / arm | First edit | Edits 2–8 | Ninth edit (native fallback) |
|---|---:|---:|---:|
| Completion / native | 1.842 | 1.522–1.690 | 1.594 |
| Completion / overlay | 6.269 | 0.712–0.842 | 1.943 |
| Definition / native | 1.745 | 1.384–1.513 | 1.469 |
| Definition / overlay | 6.281 | 0.657–0.787 | 1.778 |

All 216 requests succeeded in 24 fresh processes. Accepted repeated edits have
lower medians in this fixture; certification and fallback cost more. Complete
sequence times include diagnostic waits and do not consistently improve. This
small synthetic experiment does not establish general or UVM responsiveness.
Both experimental flags remain default off. Raw outcomes and preregistered
binary/driver/fixture hashes accompany the report.

Use [the complete fork checkout/build/acceptance recipe](../docs/start/building.md),
including `SLANG_SERVER_INCLUDE_SDK_TESTS=ON`, for new environments.

`linux-2026-10-17.json` records the previous rebuild. The
`testedProductCommit` identifies the C++ product tree; a later evidence-only
commit may contain the report and additional executed test drivers. No historical
pass or performance sample is included as a current result.

The method experiment remains default off. Its synthetic Linux measurement had slower medians
at every recorded step for completion and definition. The first edit and
the ninth-edit native fallback are recorded separately from edits 2–8. Each arm
has six samples per step; this is not production or UVM performance evidence.

Canonical commands (after SDK/dependency preparation and CMake configuration):

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure --no-tests=error -j4
python3 -m pytest tests/system tests/method_scope --binary "$PWD/build/bin/slang-server" -q
CXX=g++-14 bash tests/transport/run-diagnostic-transport-tests.sh
python3 tests/method_scope/check.py --binary build/bin/slang-server --output oracle.json
python3 tests/method_scope/count.py --binary build/bin/slang_server_counted --output counts.json
python3 tests/method_scope/held_diagnostics.py --binary build/bin/diagnostic_test_server --output held.json
python3 tests/method_scope/benchmark.py --binary build/bin/slang-server --plan plan.json --output timing.json --seed 20261017 --blocks 3 --plan-only
python3 tests/method_scope/benchmark.py --binary build/bin/slang-server --plan plan.json --output timing.json
```

The benchmark plan binds the executable, driver, fixture, and arm configuration
hashes before measuring. The counted executable is exclusively for scope
observations; timings use the ordinary executable. Logs and raw measurements
are retained outside Git, with their hashes in the report.

GCC 13 and Linux arm64 are unverified here. CMake used the existing Python 3.12.14
for source generation; the final Python/LSP suite and benchmark used a scoped
Python 3.12.6 matching the project's Python pin. Tool installation did not change
user settings or the system Python installation.
