# Standalone diagnostic transport regression tests

This test directory is independent of the server build. It compiles the real
`src/diagnostics/DiagnosticProcess.cpp` directly with its public header and uses
a dedicated test executable as the fixed-argv `--diagnostic-worker`. It does not
build or link slang, modify server source, publish anything, or require `/proc`.

## Run

From the repository root:

```sh
tests/transport/run-diagnostic-transport-tests.sh
```

Or pass another checkout explicitly:

```sh
tests/transport/run-diagnostic-transport-tests.sh /absolute/path/to/server
```

The default source is this repository root. `CXX` can
select the compiler. The runner creates all binaries and ephemeral test files
under this directory's `build/`. It uses C++17, pthreads, and warnings-as-errors.

The harness calls `initializeDiagnosticProcessPlatform()` before any worker,
thread, or fork entry point.

On native macOS it also links Security/CoreFoundation with `-Wl,-adhoc_codesign`,
ad-hoc signs the harness
and a deliberately different replacement worker, and prepares an unsigned
copy. Ad-hoc signing changes only these newly built test fixtures, not system
trust settings. Run on both Intel and Apple Silicon for architecture coverage;
Linux execution cannot validate the Darwin branches or code-signing behavior.

## Coverage

The portable suite has 32 cases:

- A binary 1 MiB frame, generation/job identity, single-consumption results,
  empty output, fragmented header/body, and copied identical executable
- Oversized response header, truncated header/body, extra bytes, second response
  frame, worker crash, input limit, encoder exceptions, and post-error reuse
- A held worker's timeout, cancel, newest-job replacement, stop/join, destructor
  cleanup, and submit-after-stop rejection
- A gated encoder makes pending-job replacement deterministic; 20,000
  notifications exercise nonblocking/coalesced wake behavior
- A deliberately non-CLOEXEC unrelated fd must not reach the child, while the
  parent's copy remains open
- Independent child runs with stdin closed, stdout closed, both closed, and all
  three standard descriptors closed
- Worker read/write helper size and truncation guards, plus broken-peer output
  without process-wide SIGPIPE suppression. The Darwin helper fixture explicitly
  establishes SO_NOSIGPIPE, matching the real transport's socket contract
- Invalid/wrong parent PID rejection; owner death before request framing,
  before guard registration, and during a busy semantic loop after registration

Normal transport cleanup is checked using `kill(pid, 0)` and `waitpid`, including
absence of a live child or zombie. Parent-death scenarios use a separate owner
process and a lifetime socket to observe worker exit. Every helper, owner, and
worker starts through `posix_spawn`/exec; no raw fork child invokes Security,
CoreFoundation, or C++ thread construction. They do not claim the
orphan is reaped by the test process. A busy loop exercises an independently
scheduled watchdog; a wholly SIGSTOPped worker is intentionally not tested as
if macOS supplied Linux's kernel parent-death-signal guarantee.

Three additional macOS-only cases are explicitly skipped on Linux:

1. A different signed executable is rejected before receiving any request byte.
2. A configured path first succeeds, then is atomically replaced by a different
   signed executable, which must be rejected before receiving request bytes.
3. An unsigned worker fails closed, either through launch/signature rejection.
   This does not assert that an unsigned parent can start on all architectures.

The replacement executable records `BOOT` and `INPUT` markers if scheduled. A
missing BOOT marker is allowed because rejection may precede its startup; an
INPUT marker always fails the test. These tests establish withholding of job
bytes, not prevention of all replacement-executable bootstrap instructions.

The full harness has a 90-second watchdog; helper processes use bounded
watchdogs too, so broken notification or cleanup code cannot hang CI forever.

## Recorded Linux result

`linux-port.log` records the latest complete run: **32 passed, 0 failed**, with
native-only signature tests explicitly skipped. `linux-verification.txt` records
the compiler, platform, exact command, and verified source hashes.
`linux-baseline.log` is the
initial 27-case run before parent-guard/identity coverage was added.

The exact command was:

```sh
tests/transport/run-diagnostic-transport-tests.sh > recovery/macos-port-tests/linux-port.log 2>&1
```

The script prints the full compile command and test executable invocation into
that log. This is focused transport validation, not a full server or macOS pass.
