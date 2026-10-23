# Local method certificate renewal

`experimentalMethodBodyCertificateRenewal` is **false by default** and requires
`experimentalMethodBodyQueries`. Enable `experimentalMethodBodyDiagnostics` as well to
retain a completed full-design diagnostic baseline for eligible class methods.

Without renewal, a body-query certificate expires after 64 accepted edit notifications.
With renewal, the next accepted edit starts another 64-edit accounting window. Each edit
continues to validate the original lexical owner, completion epoch, configuration, dependency
index, open-document witnesses, disk contents and directory listings. Method exterior text,
signatures, preprocessing state and external constant dependencies retain their existing checks.
Renewal neither reparses the whole design nor certifies edited syntax as a new outer context.
Cancelled input can synchronize text without binding; the next query binds only the latest body.

The original lexical owner and diagnostic baseline remain fixed. Each bound body replaces the
previous private body rather than retaining an edited-owner chain. General body queries fence
completion tickets by context epoch, certificate index and exact editor version; they no longer
accumulate an unused list of editor buffer IDs. The legacy overlay retains its independent bound.
`slang.methodScopeStatus` exposes cumulative `accepted`, `certificateRenewals`,
`retainedHandles` and `localDiagnosticUpdates` for the current owner.

Standalone renewal also activates context retirement when no stable-library experiment is enabled.
This does not remove source-manager storage growth or context retirement. Existing storage
thresholds, cancelled-replacement checks and the synchronous hard high-water fallback still apply.
A context swap or unsupported input invalidates the certificate and may require native analysis.
There is no additional fixed debounce. Complete semantic elaboration remains a potentially
blocking fallback; first certification and full dependency reads remain separate optimization work.

For an existing body-query test configuration, add:

```json
{
  "experimentalMethodBodyQueries": true,
  "experimentalMethodBodyDiagnostics": true,
  "experimentalMethodBodyCertificateRenewal": true,
  "experimentalCooperativeAnalysis": true
}
```

Stable UVM/DUT cache settings remain project-specific and independent of these options.
