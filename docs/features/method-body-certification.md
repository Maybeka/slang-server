# Method-body input certification (experimental)

`experimentalMethodBodyCertification` defaults to `false` and requires
`experimentalMethodBodyQueries`. Add `experimentalMethodBodyDiagnostics` to use the retained
diagnostic baseline. Existing certificate renewal and cooperative retirement remain independent.

```json
{
  "experimentalMethodBodyQueries": true,
  "experimentalMethodBodyDiagnostics": true,
  "experimentalMethodBodyCertificateRenewal": true,
  "experimentalMethodBodyCertification": true,
  "experimentalCooperativeAnalysis": true
}
```

The option separates cheap context/document checks from complete input certification. Editing
checks the context before binding and full input contents before installing its bound body.
A deferred superseded edit may synchronize text without binding. Hover, completion, definition
and resolve construct their private result, then verify all directory entries and disk bytes
before returning it. Lazy binding inside that query does not repeat the same disk walk. Local
diagnostics similarly retain their final complete validation before committing any publication.
No mtime-only proof, persistent success memo, asynchronous access to shared semantics or fixed
debounce is introduced. The constructor still establishes its original source witnesses.

A completed current native diagnostic pass can be captured before the first edit clears its
completion marker. This capture owns one original compilation, its raw diagnostics and one
target-file text value. It requires current coverage of open documents, an unlimited diagnostic
stream, a nonfatal baseline and one eligible nonstatic class-method context. It does not create
a new semantic owner for each edit. Capturing the text value is necessary because editor buffer
replacement removes its old SourceManager lookup even when its storage is otherwise pinned.

The expensive detached-original-versus-native comparison and retained diagnostic formatting run
only when current diagnostics need them. Cancellation preserves the capture for retry; the old
published diagnostic map remains untouched. Current reference/dependency eligibility is checked
before formatting all retained diagnostics. Unsupported baseline preprocessing is rejected, and
failed preprocessing binding releases the capture before native analysis. After successful
certification the temporary capture is released; the ordinary body diagnostic cache retains its
single original owner. Context retirement discards the entire local state before replacing the
driver. A newly completed current native pass can seed a later attempt after fallback.

This retains existing diagnostic coverage limits. Line-count changes in a file with later macro
expansions conservatively require native diagnostics, including common UVM `uvm_info` cases.
General changed external reference usage, specialization dependencies, unsupported local declarations,
driver/timing effects and bounded/fatal streams also retain native fallback. Moving certification
into background does not eliminate its first-use cost. Publication checks are observations of
filesystem contents at their checkpoints, not atomic filesystem snapshots.

When a complete native publication runs with this option and local diagnostics enabled, the
real diagnostic client records raw diagnostic values and their physical publication routes.
The draft commits only after successful full publication; scoped observers are restored on
cancellation. First local certification can use this exact batch after checking its generation,
configuration, context and compilation owner. Editor URI aliases are resolved when consuming
physical routes. Selecting a DUT instance only refreshes shallow views and preserves this
full-design publication; source/configuration invalidation still rejects it. The snapshot has
weak semantic ownership and transfers its raw data into one pending baseline. After certification
or decline, no second driver-owned raw copy remains. Paths without a matching publication use
the existing detached comparison and formatting path.

Initial source witnesses read each distinct path once under the experiment. On macOS, complete
content proofs use an opened descriptor's resolved path, read all bytes, and recheck EOF,
regular-file identity and descriptor/path consistency. The implementation rejects symlink paths and nonregular files, and checks for replacement,
growth/shrink and observed changes during the read. Metadata alone
never admits reuse. Unsupported descriptor path queries and other platforms keep the portable
strict path/content reader. Directory, missing-lookup, configuration and final version checks
remain in place. These are observed checkpoints, with the same filesystem snapshot limitation
as the original reader.


If complete input validation observes a changed dependency, directory or configuration, the
server records a pending reload before releasing the local certificate. Native fallback builds
a private current driver from disk configuration and all latest unsaved editor buffers; it
cannot reuse the previous driver's cached source just because a watcher notification has not
arrived. Cancellation before replacement commit preserves the old published diagnostic map and
the reload obligation. Later incremental notifications still synchronize their text once before
retrying. Invalid configuration fails semantic requests until a valid replacement can be built;
status and explicit maintenance commands remain available. This recovery uses full native
analysis and can take seconds in a large design. It adds no debounce.

The first certificate also compares source and command-file bytes with the inputs actually read
by the retained native context. Configuration loading keeps the exact workspace/user/local layer
bytes, including absent layers, in runtime-only witnesses; these are excluded from configuration
serialization and the public schema. This closes the window where an options file changes
before the first local certificate. Explicitly trusted frozen libraries keep their existing
fixed-dependency / manual-rebuild contract.


Local diagnostics can also prove a changed read/write reference bit when that bit was already
set in an explicitly trusted provider's original exported usage profile. The proof excludes later
navigation and consumer overlays. Normal instance-function references such as UVM `get_name()`
can qualify. Changed package/static/task/constructor call references still require native effect
analysis; all other driver, timing, initializer, specialization and fatal-stream checks remain.
Without a qualifying fixed provider, external usage changes retain native fallback. This does not
provide a general reference dependency ledger or remove later-macro line restrictions. See the
[fixed-provider evaluation](../development/fixed-provider-reference-evaluation.md).
