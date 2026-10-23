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
Changed external reference usage, specialization dependencies, unsupported local declarations,
driver/timing effects and bounded/fatal streams also retain native fallback. Moving certification
into background does not eliminate its first-use cost. Publication checks are observations of
filesystem contents at their checkpoints, not atomic filesystem snapshots.
