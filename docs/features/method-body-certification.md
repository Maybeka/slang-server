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

This retains diagnostic coverage limits. Later macro expansions can qualify through the
restricted runtime-line proof below; other line-dependent structure still requires native analysis.
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
provide a general reference dependency ledger. See the
[fixed-provider evaluation](../development/fixed-provider-reference-evaluation.md).


Line-count changes can reuse retained diagnostics for later methods in the same class when a
fresh parse proves that their expanded CST tokens are identical except for builtin `__LINE__`
integers used as entire runtime input arguments of direct, ordinary nonstatic void instance
functions. Both the original literal and formal must be signed 32-bit `int`; the current value
must fit its nonnegative range. This admits common UVM log line arguments without rebinding
unmodified neighboring methods. It does not replace their semantic graphs or driver effects.

Each affected method replays its own entry macro environment, keyword version, logical line
and warning directives. Direct intrinsic uses are checked even though they are absent from the
preprocessor reference list. Nested definitions are followed; preprocessing state changes,
conditional definitions, token concatenation, ambiguous contexts, syntax errors and external
elaboration dependencies require native analysis. Every publication reparses the current
expansions and rechecks token shape, provenance and the original runtime-argument proof.
Line numbers used in returns, branches, dimensions, strings, system calls, static/task calls,
other types or conversions remain outside this proof. Existing body driver, timing, reference,
specialization, error-limit, complete input and final version checks continue to apply.

Hover and completion documentation for enclosing or later retained declarations use the current
native view, so they cannot print old body text, expanded line values or source links. Ordinary
edited-body hover continues to use its current local compilation. Native documentation is rendered
inside the value certificate and then fully revalidated, preserving earlier completion tickets
from the same document version. Cancellation during the proof
leaves the previous complete diagnostic publication intact and permits retry. The proof is
lazy and belongs to one original diagnostic owner, without a chain of edited semantic owners.

See the [later-macro evaluation](../development/later-macro-line-evaluation.md) for the
original multiline CVA6 measurements, current Native documentation checks and remaining limits.


Private query parse text now follows its syntax/compilation consumers and explicit
buffer pins. Disposable neighbor proofs release their bytes on success, cancellation
or error; retained baselines and current mini ASTs keep their leases until unused.
`slang.methodScopeStatus` includes capacity-based `sourceMemory` with live query
bytes/buffer counts and still-pinned replaced bytes; source-location table storage
is separate. Names and BufferIDs remain valid within the source manager, and ordinary
files / immutable prefixes are never released by this private-query path. Source-location
history and repeated canceled retirement still require further work. See the
[source storage evaluation](../development/source-storage-evaluation.md).


Cooperative retirement retries use the actual cost of a canceled candidate with an
exponential wait capped at one second per retry. After cancellation, foreground
activity moves that maintenance deadline; normal input, queries, current diagnostics,
input/configuration recovery and asynchronous result wakes bypass it. The existing
hard source highwater still forces retirement, and idle retry runs without another
user message. Already-started full model/semantic work can still contain atomic
segments. This adds no fixed input or diagnostic debounce. See the
[maintenance retry evaluation](../development/maintenance-retry-evaluation.md).


Current requests also reach disposable owner, configured/hinted package and dependency
syntax parsing, inferred include reparsing, snapshot discovery, references and CodeLens.
Shallow option refresh preserves the parser checkpoint; hinted-package cancellation
propagates instead of becoming a persistent context error. Hint/interface metadata
caches publish only complete collections and weak hint ownership does not pin old text.
Completed syntax can remain reusable when its caller is canceled. Full model refresh,
semantic binding and already-committed selection updates still have separate cancellation
boundaries. See the [owner cancellation evaluation](../development/owner-cancellation-evaluation.md).

## Source history allowance and accounting

`sourceHistoryMaxMiB` defaults to 64 and accepts 1 through 4096. It controls parsed
source text, retained source data and location-table growth beyond a generation's
initial working set. It does not enable any experiment. With cooperative analysis
or another context-retirement experiment enabled, half the allowance requests
private background replacement; the full allowance requires synchronous retirement
at an observed safe boundary. This can add a Native rebuild tail. No fixed input
or diagnostic settling delay is added.

```json
{
  "experimentalCooperativeAnalysis": true,
  "sourceHistoryMaxMiB": 64
}
```

The baseline is established after configuration/build/top/exploration changes,
not by the first later edit. Notifications and completed foreground requests
observe growth, including a query that finishes a deferred Native parse. Queries
are not counted as edits by the test-only generation edit limit. Every successful
replacement establishes a new initial working set. Lazily allocated metadata also
counts when materialized; unusually small allowances can cause frequent rebuilds. Current editor text and diagnostic versions
survive replacement, cancellation and configuration errors.

This allowance is separate from `uvm.semanticMaxMiB`, `frozenSources.maxMiB` and
process RSS / physical footprint. Fixed providers or live old views can retain
older source namespaces outside the current generation. A retained Native design
view now owns its source namespace and library identities independently of the
driver; releasing the last view releases that lease without retaining a chain of
old drivers.

The read-only `slang.sourceStorageStatus` command reports retirement activation,
configured allowance, baseline, soft/hard thresholds and pending maintenance. Its
`storage.current`, `storage.retained` and `storage.total` give capacity-based source
allocation charges, including per-manager location tables and immutable-prefix
tables. Shared FileData and immutable-prefix allocations are deduplicated in the
total, so current and retained subtotals are not simply additive. These charges do
not include all compiler AST allocations or the system allocator's retained pages
and are not a process-memory cap. The registry uses weak observers; a status read
does not keep unused namespaces alive after returning.

Native 历史回收从第一个补全响应开始携带代际标记，保证第一次回收前返回的项目也能安全解析详情。普通符号按当前名字重新查找；参数化类和虚拟接口使用当前接收对象，保留实际类型参数和 modport。后者只在同一 URI、打开版本和完整文本均相同时跨回收使用原始文档偏移；include 的不同物理编号还须核对文件与完整字节。编辑或重新打开不同文本会使旧位置失效，宏展开及私有方法 slice 不按原始文档偏移复用，方法证书与版本检查仍保留。

跨代际重绑定保留补全项原先选择的符号种类：原先指向 `extern` 方法声明时继续展示该声明和注释，原先指向实现时继续展示实现。不能因名字查找会解引用 prototype 而改变详情内容。


### Provisional synchronization and native input admission

With experimental method-body certification enabled, applying an edit checks the
current context and commits editor text plus a private provisional body slice. It
no longer repeats the complete file proof during synchronization. Every successful
local completion, definition, hover, resolve and diagnostic publication retains its
final complete proof. No successful proof is cached across requests.

Native-only protocol requests, semantic commands and local requests that fall back
to native analysis admit the certificate inputs before using a filesystem-derived
view. A declined local diagnostic slice has the same obligation without requiring
a foreground query. Failed admission reloads privately, preserves current unsaved
text and fails rather than returning an old cached result when configuration cannot
be recovered. Status and maintenance commands remain available; hover preview uses
its own final query proof.

### Independent native receiver reads

A completed native method can prove that an ordinary, nonstatic, nongeneric class
handle property is independently read as another method call's receiver. The local
diagnostic cache can then add or remove this property's read without changing its
aggregate read status. The evidence comes from completed native call records,
not mutable reference flags that navigation might affect. Methods sharing the
edited physical syntax, including repeated include contexts, cannot supply that
evidence. Static, generic, array and virtual-interface receivers do not use this
proof. This proves only the property's read bit; writes, newly referenced callees,
specializations, driver/timing effects and all final input checks retain their
existing guards.
