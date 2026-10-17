# Linux validation records

`linux-2026-10-17.json` records tests and measurements from this rebuild. The
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
