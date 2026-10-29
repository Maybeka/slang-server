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

`experimentalMethodBodyMacroProof` separately defaults to `false`.
`methodBodyMacroProofMaxMiB` defaults to 16; zero disables the additional proof.
With complete original full-unit preprocessing access facts, a method can add, edit or remove
macro names absent from the original positive and negative reads, writes and exit environment.
Existing observed `define` / `undef` effects must retain their exact contents and byte positions.
The original method entry/exit journal authenticates the terminal macro environment. Dormant
raw directives use the same absence rule on both sides. Private query fragments cannot supply
a whole-unit EOF proof, and incomplete, missing or over-budget facts retain native analysis.

Line shifts also verify that later macro lookups keep their identity. This check may memoize
only the immutable retained syntax graph; it never memoizes successful input authentication.
Pasted or dynamically constructed lookup names remain unsupported. Input/version checks and
cancellation checkpoints remain mandatory. Public macro hover, original/expanded preview,
definition, completion/resolve and expansion continue to use current macro data. After retiring
a generation, local reuse requires a current query context in the replacement generation.

The option separates cheap context/document checks from complete input certification. Editing
checks the context before binding and full input contents before installing its bound body.
A deferred superseded edit may synchronize text without binding. Hover, completion, definition
and resolve construct their private result, then verify all directory entries and disk bytes
before returning it. Lazy binding inside that query does not repeat the same disk walk. Local
diagnostics similarly retain their final complete validation before committing any publication.
No mtime-only proof, persistent success memo, asynchronous access to shared semantics or fixed
debounce is introduced. The constructor still establishes its original source witnesses.

Repeated validation of unchanged later macro methods can memoize its pure syntax evidence.
`methodBodyLineSyntaxCacheKiB` limits the retained suffix to 1024 KiB by default; zero disables
this memo and values are capped at 65536 KiB. It requires method-body queries, diagnostics and
certification. One exact suffix and one raw end-line coordinate are retained, so typing does
not accumulate cache entries. A changed line coordinate replays the original proof and replaces
that one coordinate. Changed suffixes, insufficient budget and unsupported evidence retain
normal replay. Neighbour semantic dependencies and the complete physical publication proof are
still checked each time. This memo never certifies filesystem contents for another request.

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
General unsupported external reference usage, unsupported specialization dependencies, unsupported local declarations,
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

### Native request cancellation and completed-caller evidence

Rename, document highlight, inlay hints, document symbols, document links and code
actions carry their registered request context through analysis and collection.
Cancellation checks run between shared lazy-binding operations. Symbols are built
in a private visitor and enter the outline cache only after a complete traversal;
a cancelled nested traversal cannot leave a partial cache or a dangling child pointer.
With `experimentalMethodBodyQueries`, a subsequent edit of the request's primary
document can also supersede these requests. Explicit LSP cancellation is supported
independently. These boundaries do not make every shared binder operation interruptible.

Completed, unrelated ordinary Native class methods can also prove that an ordinary
nonstatic class-function callee is already read. This can admit a newly added call
without changing the aggregate read bit, preserving both implementation and extern
prototype identities. The peer must be independent of every edited physical include
occurrence. Static, task, generic, constructor and foreign-owner calls retain their
separate guards. This is positive reference evidence, not general effect closure.

### Automatic package and module diagnostics

With the existing method-body query and diagnostic experiments enabled, an
input-only automatic package or module function/task can use local diagnostics
when one completed full-design context and the detached-baseline equivalence proof
are available. Declaration-level unused-subroutine diagnostics remain outside the
replaced body. Static lifetime, ref/output/inout formals, interface/program contexts,
multiple full-design contexts and external elaboration dependencies use Native
analysis. The existing reference, specialization, nominal-declaration and final
input-proof checks still apply.

Native propagates package/module function drivers into callers, unlike nonstatic
class methods. A persistent driver, including a write to a static local, therefore
requires full Native caller analysis. The local path must not suppress a caller's
`multiple-always-assigns` diagnostic. Automatic local-variable edits and the
supported task-timing caller update can remain local. Caller driver and initializer changes use private full-design effect replay on the completed Native AST, borrowing unaffected procedure results. General local global-driver updates, multiple contexts and virtual-interface finalization remain unfinished; taking the correct Native path is not
proof that those updates have been implemented.

### Reducing proof construction work

Raw preprocessing-state and conditional checks use one scan in place of repeated
regular-expression scans. They deliberately retain the prior conservative treatment
of strings/comments, word boundaries, nested conditionals and rejection priority.
The legacy regular expressions remain an independent test oracle.

On Mac, byte-identical normalized absolute POSIX paths can avoid repeated
`filesystem::path` construction. Other spellings retain the original comparison.
Both descriptor-path checks, complete content/EOF and final file metadata checks
remain; no successful proof is reused across requests. These implementation changes
require their own large-project performance acceptance before a speedup is claimed.

### 后续声明宏与跨类行号证明（实验）

行数变化不再一律因后续声明宏而放弃局部更新。服务器根据完整的原生宏变更日志检查所有可能的嵌套定义（含默认参数）：只有闭合的宏依赖图不包含行号内建量、拼接或预处理状态修改时，才证明该声明展开与行数无关；缺失历史、动态宏名、递归及不能证明的情形仍转Native。后续方法的运行时行号参数证明也扩展到同文件的其他词法类，用于package函数位于验证类之前的场景。依赖行号的常量、类型或其他语义声明仍需完整分析；这不等于一般调用方/类型影响闭包已经完成。

### 分段宏方法体及泛型实例（实验）

完整宏入口日志也可用于回放跨多次宏调用生成的方法体，先核对原生展开token，再验证编辑后的展开。只允许已完成原生分析证明为普通非静态实例void函数的signed int输入实参的运行时行号变化；类型、常量、预处理状态和语法恢复等未证明情形保留完整分析。

泛型方法不必已存在于查询索引：在证明原生声明就是查询树中的同一节点后，按声明保护已有和后续实例，并按声明登记外部求值或体作用域依赖，供后续请求失效。嵌套保护不能覆盖外层保护。原生行号证据对所有已分析实例取交集；int/byte等不一致实参类型不会因其中一个实例合格而获准复用。该扩展不等于一般多上下文、虚拟接口或调用/初始化影响闭包完成。首次语法身份查找及宏证明成本仍需优化；详见[泛型宏方法实测](../development/generic-macro-context-evaluation.md)。

宏证明的遍历范围可按物理源端点及真实include祖先缩小。声明端点未知、跨文件等不能排除时保留完整遍历；原生token比对仍必须通过。多个缺失查询上下文的声明合并一次核对，其静态身份只在保留的原始CST生命周期内复用；依赖文件的输入认证不复用成功结果。首次增行实测已改善，但外围排队和诊断延迟门限尚未通过，详见[范围限定实测](../development/scoped-macro-evidence-evaluation.md)。

Private diagnostic dataflow now accepts an optional cancellation callback. Experimental body-analysis consumers check it at procedure entry/exit and every256 concrete flow visits or loop-discovery steps, including the loop lint prepass. Default analysis has no callback. A cancelled private pass is discarded; completed public analysis and published diagnostics remain available, and a fresh request can retry. This bounds traversal steps between checks, not elapsed time: individual constant evaluation, decision-DAG operations and shared binding still include atomic work. It does not reduce the current full caller-effect replay to an affected procedure cone.

Experimental private caller-effect replay now retains native intrinsic procedure diagnostics separately from global driver-conflict diagnostics. A cancellable, immutable dependency index on the completed Native baseline identifies changed bodies and transitive callers; unaffected completed procedure summaries and intrinsic warnings can be reused. The replay pins exactly that baseline owner and rejects chaining private generations, incomplete baselines, other compilations and option mismatches. Assertion/checker publication side effects are rebuilt. Scope/unused checks, initializer propagation and global driver tracking still run, so this is procedure-result reuse rather than a fully local global-analysis update. Diagnostic status exposes the reused and recomputed procedure counts from the last successful publication.


### 稳定类函数的外部效果证明（实验）

非静态、仅输入参数的类函数不向Native调用方传播函数体。每次编辑仍重新绑定和分析当前函数；当外部直接驱动的目标符号、位范围、标志、种类、来源和数量与已完成的原生函数一致，包/静态调用集合未变化，且外部引用的聚合状态未变化时，可以省去全设计数据流重算。存在引用该函数体位置的全局诊断、间接端口、ref连接、虚拟接口路径或无法比较的效果时，保留原有重算/重建路径；新增调用和改变驱动范围也保留必要重算。输入与版本证明仍逐次执行。

发布诊断的编辑器URI映射只在本次串行发布中预先收集打开文件；精确打开的URI优先，同一物理路径的别名随当前请求重新映射。格式化缓存和只读视图不能保留旧编辑器别名，也不能跨请求复用成功的输入认证。

全量格式化复用索引也按需建立：本次仅有目标文件局部诊断时不建立全设计比较索引；之后需要全局效果重算或外部发布桶时再构建。索引构建使用私有草稿，最终完整提交，取消可重试；原始目标源Buffer身份随固定Native基线保留，不能因编辑后的投影而误用旧位置。


### 方法体内局部类型声明（实验）

局部typedef、enum、packed/unpacked struct和union由一次性私有函数/task体持有，普通绑定也会触及未使用别名及枚举值，保留它们的错误。支持新增、已有类型修改、错误修复、多行变化、删除和取消重试；悬停仍需与当前原生视图一致，且不能使后续编辑丢失局部状态。局部类型若被函数体外部求值依赖，原有外部依赖保护仍有效；局部类型与常量作为泛型实参也在私有上下文中绑定，其特化语义与引用闭包须一起完成，见下节。局部import与新包/类型引用见下节；泛型特化依赖支持见下节；checker/断言等影响仍须分别落实，当前不能宣称所有声明均可局部更新。配置继续默认关闭。

泛型类的实例句柄可全部位于其他文件。package独立查询视图没有实例时，服务器可借用已完成且覆盖当前输入的Native泛型上下文，并保留同一原生分析owner；不会凭默认参数制造实例，也不会为此遍历并重新绑定全部消费者。取消保持公开诊断不变，下一次有效输入可重新认证。没有已完成兼容基线、存在外部求值依赖或上下文身份不能证明时仍需要原生分析。


### 局部导入及新包/类型引用（实验）

显式和通配import、直接包限定类型/函数引用可以在方法体中新增、切换、删除和修复。已有完整、当前且兼容的Native诊断基线时，查询借用它的完整命名空间，保留同一个分析owner；不为了开启此路径同步构建旧版完整设计。module仍映射到用户选择的实际实例参数。不同CST对象须逐一核对物理端点及展开结构/token；重复include的不同原生声明不能合并。悬停的作用域显示通过当前请求内的投影保持原生格式，不改动共享语义或返回旧源码。

提前访问方法声明也会触发导入类型查找，这些引用现在归属于方法体，删除或切换导入时可恢复旧包的unused警告；签名和默认参数的引用保持固定。调用和使用状态改变仍进入私有调用影响/引用重算。宏声明可直接核对已完成Native方法的原始节点，也保留视图不可用时的纯CST遍历；两条路径均支持取消并保留最新输入。静态CST成员证据不替代逐请求文件字节、EOF、路径和最终元数据证明。

局部class/checker/property/sequence声明不是函数/task的合法嵌套语法；过程checker和普通并发assert需要initial/always上下文，不属于此函数/task局部路径。task中的显式时钟expect property已有局部诊断回归；function不允许其传时间语义。真正影响外部展开的函数依赖仍需要完整模型分析，不能把这类回退当作局部优化收益。所有选项继续默认关闭。

### 泛型特化变化的局部诊断（实验，阶段 A 验证中）

私有函数/task 体新增、切换、删除或修复泛型类实例时，实际查找根与 Native 已记录的来源图共同计算仍然存活的类。诊断按旧函数/失效类作用域过滤原始记录后统一合并，引用使用采用同一排除规则；数据流视图排除失效类并纳入新类。有独立使用者的特化继续保留；没有外部根的循环不会互相保活。首次/最后实例的占位语义检查同样参与视图。

编辑的方法属于该泛型类自身时，新建的私有特化会在构造参数、局部变量之前使用当前方法语法，覆盖类内和 extern 的 function/task。Native owner 和共享泛型定义不被修改，私有上下文保留当前语法树；旧方法体误用保护仍有效。没有依赖来源证明、存在外部展开求值依赖、致命错误或有界错误流时仍可回退。

局部 typedef/struct/enum/localparam 参与泛型实参已解除名字保护，按实际类型和值绑定私有类特化并完成语义及引用闭包。值参数的推断类型也参与特化缓存键，不能将数值相等但位宽/符号性不同的参数混为一类；推断类型相同仍保持复用。新类型与特化的所有权、先查询后诊断、连续修改/无效/恢复和取消重试由独立Native对照检查。合法局部let的端口及默认模板（包括未使用模板）现在完整绑定，引用归属于词法函数体，后续替换可撤销旧使用；只触及私有方法，不启用借用设计实例的展开。过程checker/普通并发assert的函数/task嵌套形式由Native拒绝，task的显式时钟expect property及函数内立即assert已有合法性/局部路径检查。整体性能验收尚未完成。新增同类泛型单测精确比较 Native 诊断；该位置的非实验 Native 悬停返回空值，因此单独检查实验悬停的当前变量与类型，不能宣称这组悬停获得了 Native 等值证明。真实 CVA6 悬停和完整诊断仍使用逐版本 Native 对照。所有实验选项继续默认关闭。

原生编译自身已完成的占位实例检查可能与真实实例同时存在；局部视图保留这些原生检查，不能套用导入库占位检查的全局撤销规则。类型查询在语义分析完成后才创建的缓存项，也不等于已完成语义使用。引用查找穿过固定 import 作用域时，临时来源记录归查询调用方持有；显式诊断事务的路由优先。完整回归和CVA6验证包含先悬停再诊断、下一次输入释放旧查询的顺序。深二元表达式与参数初始化检查用显式操作数队列，仍检查所有子表达式和不安全调用。相关正确性证据见[泛型闭包评估](../development/generic-specialization-closure-evaluation.md)，不代表剩余A类或性能验收完成。

### 完整模型回退的明确边界

若函数被函数体外的常量求值使用（例如决定外部向量宽度），修改其返回值会改变声明类型，当前局部路径不能只替换函数体。完整设计有致命层次错误时，也不以不完整的owner认证局部诊断。新增回归以8/未知名字/4连续编辑，对照独立Native的悬停、全部历史URI载荷及当前版本；边界回退不计作局部加速成果。

Server当前将命令行error-limit重置为0，使用完整诊断流；它与用户可设置的缓存内存预算无关。局部缓存另有防御检查，直接注入受限DiagnosticEngine设置会拒绝捕获。在受支持的无限制设置下，CLI传入的error-limit不会阻止正常局部更新。测试不会把不受支持的受限发布模拟当成产品配置。回归143断言、完整925CTest及服务器ASan910组169680断言通过，生产二进制与f4f42668保持一致。


## First witness source storage

With the existing default-off method-body certification experiment enabled, the
initial source capture uses a non-replayable view of immutable file storage.
The SDK applies the same graph, stale-buffer, library-context and capacity checks
as an owned snapshot. The view keeps file bytes alive without keeping their
SourceManager alive, and the server releases it after building the physical
witnesses. This avoids copying frozen UVM/RTL and other parsed source bytes merely
to compare them and discard the capture. Owned diagnostic replay snapshots retain
their existing independent byte copies and schema.

This view is not a filesystem proof. Non-frozen sources still undergo complete
byte, EOF, version and path checks before publication. Fixed library identities
keep their existing manual-rebuild contract. No successful proof survives between
requests. Native and ASan correctness and original performance acceptance remain
required before this candidate is accepted.


### Withdrawn same-request query and diagnostic experiment

The local `experimentalQueryDiagnosticTransactions` experiment was rejected after
its first controlled performance runs did not meet the declared diagnostic and
query-tail gates consistently. Those failures remain archived; they were not
accepted or replaced by repeated samples. The experiment is withdrawn from the
candidate implementation. Its legacy configuration key is readable and ignored.

Hover, completion, definition and resolve do not prepare or publish diagnostics
as part of query certification. Each query retains its complete fresh physical
input proof. Pending background diagnostics prepare their own result and prove
their inputs independently before publishing. Cancellation and changed inputs
continue to reject stale work. There is no optional diagnostic preparation
budget or new debounce in the query path.

Stable source-prefix reuse, local background diagnostics, retained diagnostic
renderings, delta publishing and deferred retirement models remain separate
optimizations. Their acceptance still requires the original workload and
correctness gates. Withdrawal closes this attempted mechanism as rejected; it
is not a passing performance result and does not waive the original fixed14 or
direct6 qualification.


### Retained target display memo

`methodBodyRenderingCacheKiB` defaults to 256, zero disables this memo, and values
are capped at 16384 KiB. It is active only with the existing query, diagnostic
and certification experiments. It charges copied fixed outside text, metadata
and pure LSP payloads; this is not a process RSS cap or a cap on read-only
indexes borrowing the already retained Native publication.

The cache holds one end coordinate used by the Native renderer, plus exact
prefix/suffix byte witnesses including EOF. Retained diagnostics, raw ranges and
recursive notes must exclude the edited function; physical target and related
ranges must also stay outside it. Unknown custom arguments and ConstantValue
arguments use normal rendering. Fixed Native scalar/integer/packed-bit types
under the validated builtin formatter may reuse their completed Native display;
this never applies to fresh body diagnostics or types in the replaced function.

A new coordinate builds a private payload in background. Cancellation, oversized
data or failed input proofs cannot install half a result. A successfully
published oversized slot retains only a bounded negative marker, avoiding repeat
allocations for that coordinate. Same-coordinate hits reapply current URI aliases
and source ordering; semantic/reference checks and the final full input/version
proof still run. No new semantic or SourceManager owner is retained.


### Invocation-local directory proofs (experimental candidate)

The optional Mac method-body certification path can retain a bounded name plan.
`methodBodyDirectoryProofCacheKiB` defaults to 512, accepts zero to disable, and is
capped at 16384. The budget includes the conservatively charged immutable input
pointer/group table. Exceeding it uses the existing complete reader. It does not
enable any experiment or cap process RSS.

Each proof freshly opens and scans eligible local directories, conservatively
falls back for case/Unicode ambiguity and matching entries including dangling
links, and rechecks absolute namespace identity and directory versions at its end.
At most 64 directory leases exist inside that invocation. Positive files retain
their complete byte, EOF, descriptor-version, absolute leaf and resolved-path
checks. A positive reader may borrow a validated parent descriptor. Cancellation
unwinds every partial lease; descriptor exhaustion releases the optional session
before retrying the entire ordinary proof. Neither descriptors nor successful
physical proofs survive the invocation. The retained table points only into the
overlay's immutable expected-input map, without adding a semantic owner.

This candidate is not qualified: native certification regressions pass, but the
first CVA6 two-order preliminary evaluation failed its forward RSS and reverse
maximum-query gates despite improving query medians. Preserve those samples;
full ASan, correctness, pressure and original performance qualification remain.


Unchanged prototype hover also uses the certified current method parse. Return
types, argument names/directions/types and default values are compared before
answering, including non-ANSI argument declarations in the body. Function names
and formal arguments can therefore be hovered during body edits without forcing
a document-wide analysis. Native leading trivia is retained for documentation
without executing its directives again; comment formats and source/expanded/both
macro views retain native output. Queries requiring current enclosing or later
code keep the existing native-rendering route. This does not remove first-baseline
construction costs or admit edits across the method interface boundary.

When baseline capture declines before an edit, diagnostic preparation preserves
that specific reason if a later retry only observes the invalidated full model.
Successful capture or certification clears the old reason, so recovery does not
retain a stale refusal. This affects status attribution only; full diagnostic
fallback and its current-version publication remain unchanged.

### Deferred shared-baseline preparation

Under the existing certification and local-diagnostic experiments, an edit may
retain an exact current pre-edit owner before its semantic/dataflow diagnostics
are ready. Source text, physical path, configuration and publication witnesses
are captured before synchronization. Foreground work does not complete that
analysis. Cancellable background work prepares the retained owner and uses the
local diagnostic path, avoiding a second full model. Cancellation preserves the
pending baseline for a newer body edit; publication still validates current
inputs and document version.

Shared preparation checks cancellation between completed lazy binding operations,
not inside a binder. Native instance keys avoid warming redundant RTL instances;
they do not replace Native context, side-effect or canonicalization checks. The
normal complete semantic pass remains required, and fatal baselines fall back.
A single binding can still take time. This introduces no new default-on option.
See the [new-project qualification](../../validation/new-project-query-view-2026-10-29.md)
for measured responsiveness, diagnostic and memory tradeoffs and coverage limits.
