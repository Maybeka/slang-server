> B3范围增量新证据：原97715 exit0（stage-b3-ranged-arrival），forced96次20ms与hard384次20ms均按前一版本发送minimal range；最终hover和全612URI诊断/空闲后图与Native一致，目标版本发布单调、重复同版同内容0，owner配对完整。forced正常预算p50约6.4ms/p95约7.5ms；hard1MiB仍205–206取消、最大query549–617ms/末次diag1316–1384ms，明确B3未完成。已有trace表明foreground-source-accounting含420–421ms同步段、retirement-load-publish最高424–434ms；其它phase最大值含启动，不可全算回收。当前无活跃构建/CVA修改者；下一步定位hard回收的输入接纳与原子段，在不放宽硬预算/版本证明下优化。实验默认OFF、无新增防抖、无网络、未推main，B2已验收，B3/C/D待做。

> B2正式关闭（6532880f）：原fixed14 session47964 exit0、direct6 session8361 exit0，额外既定B2门槛审计b2-completion-audit.json通过；120源码身份在全部20组结束时一致。双向相对main9b查询p50约-19%、CPU约-8%、RSS约-22%、foot约-25%、末次诊断30.27/26.14ms（基线33.12/33.74）。更正观察日志时的读数误会：print第三列为maximum单次查询，39.731833并非lastDiagnosticMs；不可据此宣称诊断失败。所有正确性448+38/112图及四压力完整通过。总目标未完成，现在按路线进入B3输入接纳/取消/原子段，不跳到C；实验默认OFF、无新增固定防抖、无网络、未推main。

> 6532880f压力验收更新：原8495已exit0，四组natural/forced正反负载及owner配对审计全部通过；forced双方各有真实retired，细节见stage-b2-local-ready-pressure/pressure-summary.json。自然1MiB峰值RSS约-22%至-23%、foot约-14%、CPU略降，双方204取消仍属B3待解决；不是默认配置延迟结论。完整正确性原2153及448+38/112图已通过。下一步密封原14+6及既定B2额外诊断尾门槛；采样期间不改源码/文档/快照或构建，不并发同CVA。B2性能未验收，总目标active，B3/C/D保留，实验默认关闭、无新增防抖、无网络、未推main。

> 最新核实：6532880f完整正确性原2153已exit0；938CTest、Python17、transport43、709 SDK文件、SDK Native/ASan2632/35510、server ASan923/171555全部通过。CVA448 Native状态、38 ASan状态及112份完整诊断归档审计通过（stage-b2-local-ready/evidence-audit.json）。当前唯一CVA修改者为四组生命周期压力原session8495（stage-b2-local-ready-pressure/run.py），等待四组与owner审计；通过后再密封原fixed14+direct6和既定B2长测诊断门槛。当前无活跃构建，未开始性能资格；B2及总目标均未完成，后续B3/C/D保留。实验默认关闭、无新增固定防抖、无网络、未推main。

> 最新候选6532880f（stage-b2-local-ready），SDK仍1c8ec487。split两路长测原54924 exit0机制结果：query/p95约-10%，但CPU+12.699%、末次diag+4.94%，已拒绝并移除全部并行生产代码；RequestContext无hasObserver新增API/没有额外proof线程。目录批量证明也已移除，仅保留原证据。
> 当前仅两项：1）watch原始路径每次完整收集，值完全相同才复用纯规范化/排序不可变DTO；不保留模型、AST或成功物理证明，server按自己最后消费DTO判断，新driver仍重建anchor，key/value完整构造后提交。2）当前driver的已准备局部诊断，在cooperative+bodyDiagnostics开启且无input/config reload时不再等旧2ms grace；未准备/非协作保持原路，retirement retry期限不变。所有物理逐文件proof回到原d31串行、EOF/fstat/lstat/F_GETPATH和取消检查均保留。实验仍默认OFF。
> 定向26case12888assert PASS（含不等待/非协作仍2ms及Native诊断对照，watch独立reader/重建anchor及大批文件proof）。CVA短48 edits机制612URI/hover一致，query20.09→18.87ms/p9521.36→20.99、末次diag31.02→27.98、CPU-1.14%、foot-0.11%，非资格不可替代原20组失败。
> 当前唯一完整pipeline原session2153已启动且工具确认live：先NativeSDK，再Native全目标/CTest/Python17/transport43/709完整性、ASan全目标/SDK/server，再CVA448Native+38ASan/112图audit。stage-b2-local-ready/pipeline.log与correctness-runtime.json。构建活跃禁止改C++，不能并发完整单元/ASan或同CVA配置修改；等待原2153不要重启。当前候选尚无完整正确性或性能资格，不能继承旧server结果。后续四压力与原14+6/更广B2计划diag限仍需验收；B2未完成、B3/C/D待做，总目标active，无网络、无新增固定防抖、未推main。

> 最新当前源候选01607a62（stage-b2-split-proof）：目录缺失批量方案已撤回生产，helper真实代表输入1960候选capture+recheck约2047us，旧1993失败open约1483us；真实短测无收益，源码/19组12636断言/安装/原报告均留在directory-absence(-alias)证据目录，工具文件已从source删除、CMake去引用、SourcePathProofTests还原。两路全时读取CPU+12–13%也已拒绝。
> 当前实现：每次仍完整收集watch原始路径，只对全值相同的路径向量复用纯规范化/排序DTO（两实验标志开启时才启用），不保留AST/模型或成功物理证明；server保存自己最后消费的DTO身份，独立reader提前更新缓存不漏注册，换driver仍重建anchor，key/value完整构造后才提交。新增watch回归及现有稳定/重建测试同时覆盖开关，watch+batched共25case12857assert PASS。
> 另保留foreground-only物理两路：RPC且无测试observer且>=64输入时，辅助线程读existing/unknown、caller查expected-missing；无任一路则串行，后台/通知均串行。一个helper归局部上下文所有，任务引用在complete前清空、析构先停join再释放语义owner；ctx测试observer始终caller-only。所有文件完整字节/EOF/fstat/lstat/F_GETPATH和取消谓词未改，无物理成功证明跨请求缓存。短测相对d31 query20.06→17.97ms/p9521.33→19.01ms、CPU+7.09%、foot-0.57%，但最后diag31.21 vs26.01ms更慢，不验收。之前foreground按下标分半3656的CPU+11.29%已留证未过。
> 唯一活跃CVA修改者：原session54924，stage-b2-split-proof/run-long.py，forward1008机制对照d31（非资格），先摊薄启动成本确认持续CPU代价再决定保留并行或仅保留watch。没有当前候选的完整ASan/448+38/112，不能继承旧server结果。无活跃构建，不并发同CVA/额外测试/构建；继续等原54924不要重启。当前状态source/validation/data/stage-b2-split-proof-2026-10-26.json，9源快照/plan与short-analysis在stage。B2未验收，B3/C/D和总目标仍未完成；实验默认OFF、无固定防抖、无网络、未推main。

> 最新：请求归因40040已exit0/612URI与hover一致，analysis.json证实hover查找/渲染仅几十us、排队median5.43ms来自前序编辑+watch工作；主要执行在物理证明。纠正前述“未知调度间隙”：应取每对body-validate-files前条为hover（median11.206ms、last10.069ms），后条才是pub（6.249/6.272ms），不能用末条pub解释hover。文件stage探针及同binary lstat探针均终态且源码已还原，lstat无稳定整体收益，不采用。
> 新生产WIP为两路物理证明：>=64独立输入由caller+一个helper处理、每路径原完整证明不变，ctx测试observer保持caller-only；每次创建helper的88c2513短测query-14%/diag-10%但CPU+13.3%，故未完整资格。现改为上下文持有单个空闲helper，任务引用在completed前清空，析构先停/join helper再释放语义owner；新并发取消/重试回归及batched组13case12483assert通过。reuse短测query19.89→17.09ms、p9521.41→17.90、diag29.26→26.57，但CPU仍+12.15%、foot+0.72%，不能验收，也不为此立即再跑20组。新状态source/validation/data/stage-b2-reused-proof-reader-2026-10-26.json及stage对应source-snapshot；完整ASan/CVA尚未跑，SDK未改。
> 当前无活跃构建/测试/CVA修改者。下一步研究每请求的目录缺失候选证明，减少1993次独立缺失打开，而非仅并行叠加CPU；须先论证目录版本/命名空间替换/缺失候选新增可检测、不跨请求缓存成功证明、保留已有文件完整读取与路径身份检查，再实现回归与短验证。d31原20组诊断失败仍保留，B2未验收，B3/C/D待做，总目标active。默认OFF、无固定防抖、无网络、未推main。

> 当前d31ae631资格全部终态：fixed14原64454 exit0通过，direct6原49859 exit1；119源文件在两组结束时身份一致，无pending。direct双向长测末次诊断各+13.26%/+43.09%未过原10%；fixed反向31.875 vs26.704375ms亦与B2计划长测诊断不退化10%约定冲突，虽原fixed14 evaluator未覆盖，仍保留为未解决要求。B2不能验收，所有失败/源快照/收据保留，不重采未改代码求过。原阶段trace显示proof最后6.1–6.5ms，与基线6.0–6.2ms差较小，但末次query candidate18.18 vsbase9.33ms；现有phase不覆盖所有请求前后/hover/排队时间，不能继续把全部差异归于文件读取。下一步以完整请求与hover阶段归因定位，再选实质改动。当前无活跃构建/测试/CVA修改者，实验默认OFF、无防抖、无网络、未推main；总目标active、B3/C/D待完成。

> d31ae631进入资格采样前状态：完整正确性935CTest、Python17、transport43、SDK Native/ASan2632/35510、serverASan920/171068、CVA448Native+38ASan及112完整图均PASS。压力原26168 exit0，4组及owner寿命平衡PASS；相对B1d88自然1MiB RSS约-22%、foot约-14%、CPU约-1%，但query p50约+7–8%，双方204取消，不能说取消/所有延迟已改善。forced64MiB双方各1真实回收，foot约-22%、CPU约-6%，query约+1–5%，最后诊断candidate25.7/29.9ms。原始Bbe诊断门槛失败保留，不重采未改代码求通过。
> 当前无活跃构建/测试/CVA修改者，下一步absence-proof-qualification/seal.py捕获新源快照并同时密封原fixed14/main9b及direct6/A7b，所有负载/基线/性能门槛/harness不变，只更新新测试数量与候选身份。整个20组期间禁止改源码/文档/快照、构建、额外测试或并发CVA配置修改；进度查外部runtime，等待原进程不要重启。B2尚未验收、B3/C/D待完成，总目标active，默认实验OFF、无固定防抖、无网络、未推main。

> 最新：d31ae631完整正确性原pipeline14487 exit0，935CTest/Python17/transport43/SDK Native+ASan2632/35510/serverASan920/171068及CVA448Native+38ASan/112完整图audit全部PASS。现唯一CVA修改者为stage-b2-absence-proof-pressure/run.py原session26168，4组计划在启动前预声明，当前自然回收正向；不重启/并发配置修改。等压力四组及owner审计通过，更新源状态后密封已准备的absence-proof-qualification原fixed14/direct6（门槛/基线/harness不变）。B2性能未验收，总目标active，B3/C/D仍待完成，实验默认OFF、无固定防抖、无网络、未推main。

> 当前候选d31ae631（SDK仍1c8ec487）：原失败样本定位到2237个物理证明路径中1993个预期缺失路径，目录openat失败后还会绝对open。新代码仅对预期缺失路径直接执行原绝对open，不缓存成功证明、不减少已有文件的完整读取/EOF/前后fstat/绝对lstat/F_GETPATH。新增提前include在目录检查后出现file/symlink/directory三分支仍拒绝，连目录移动/取消/压力合计3case574assert PASS。临时FILE_PROOF_TRACE已撤回，probe源/安装留证。短对照证明阶段median约8.4→7.1ms、发布证明8.3→6.9ms，但端到端query/diag未改善，非资格结论。935CTest、Python17、transport43、SDK Native/ASan2632/35510、serverASan920/171068已PASS。唯一活跃pipeline为14487（stage-b2-absence-proof/run-full-correctness），正在448Native CVA，后续38ASan/112图audit；不可并发CVA修改。随后四组压力及原14+6draft已准备但未启动/密封。B2未验收、B3/C/D未完成，总目标active，默认OFF、无固定防抖、无网络、未推main。

> 当前状态：bbe08be2 的20组资格采样已全部终态（fixed14原28589 exit0、direct6原52934 exit1），源身份118文件在两组结束时一致。fixed14通过；direct6仅 direct-memory-forward-lastDiagnosticMs 未过（32.383541 vs29.091333ms，1.113168>1.10）。其余直接对照门槛通过，包括双向真实回收；全部原样本/失败/快照保留，不重采相同代码求通过。B2未验收，B3/C/D待做，总目标active。下一步仅归因诊断尾部；默认OFF、无固定防抖、无网络、未推main。

> bbe08be2/SDK1c8ec487准备资格采样：原压力56265 exit0、4组PASS、owner lifetimes平衡。相对B1d88，自然1MiB正反RSS约-22%、foot约-14%、CPU约-1%；双方仍204取消，末次diag candidate约1.73/1.68s vsbase1.82/1.81s，不能声称取消/尾延迟已解决。forced64MiB双方各1真实回收，RSS约-20%、foot约-22%、CPU约-4%；query p50正向+5.09%、逆向-1.36%，并非所有时延改善。最终612URI/hover与硬源/query界限均过。完整正确性934CTest、Python17、transport43、SDK Native/ASan2632/35510、serverASan919/170782、CVA448Native+38ASan/112解压图也全部通过。
> 当前无活跃构建/测试/CVA修改者。接下来stage-b2-owned-suffix-qualification/seal.py捕获新源码快照，密封原fixed14/main9b和direct6/A7b，两计划在首样本前密封，负载/基线/门槛/harness SHA均保持。整个20组采样期间禁改源码/文档/快照，禁额外测试/构建，只看小日志与runtime，观察超时不重启。进度以资格目录runtime为准，本段为采样前记录。旧0cd两个原门槛失败原样保存，不重复相同代码采样求通过。B2未验收、B3/C/D待完成、总目标active，无网络、实验默认OFF、无固定防抖、未推main。

> bbe08be2/SDK1c8ec487完整正确性已完成：原32537 exit0终态；SDK Native+ASan各2632/35510、934CTest、Python17、transport43、709离线恢复/完整性、serverASan919/170782均PASS。完整CVA448Native+38ASan及112解压诊断图audit PASS，每状态612URI/hover一致，原256MiB整个DUT/UVM共享保持。没有继承旧SDK测试冒充新结果。当前无活跃构建。
> 新压力pipeline56265已启动（stage-b2-owned-suffix-pressure，四组计划在样本前预声明、工具返回live session）：natural-forward/reverse各384编辑/history1MiB；forced-forward/reverse各48/history64MiB/force8，基线仍原B1合格d88，当前natural-forward。仅此进程修改同CVA配置。等待原56265，不重启/并发，不把压力时间当默认64MiB正常时延。后续stage-b2-owned-suffix-qualification原fixed14/main9b与direct6/A7b draft未密封/未采样，需压力passed+owner审计后捕获新源码收据并同时密封。资格采样期间禁改源码/文档/快照、禁额外测试/构建。B2尚无性能资格，0cd失败仍保留，B3/C/D待完成，总目标active、实验默认OFF、无网络、未推main。

> bbe08be2/SDK1c8ec487完整ASan结果已确认：原32537活跃pipeline的ASan构建、SDK2632/35510、server919/170782全部PASS；Native934CTest/Python17/transport43/709和NativeSDK2632/35510仍已PASS。现已进入同一pipeline的CVA Native448（当前local-let），之后38ASan及112完整图audit；当前无活跃构建，不改候选，不能并发同CVA配置修改。原32537经工具确认live，继续等待它不要重启。压力四组与原14+6draft保持未启动，须完整CVA正确性后依序进行。已补充开发文档中的独占后缀接管/共享视图回退/父图与源生命周期约定；还没有大场景或性能资格，不能把731MB短探针当作最终通过。B2与总目标active、B3/C/D待完成，无网络、实验默认OFF、未推main。

> bbe08be2/SDK1c8ec487后续状态：原完整pipeline32537已完成Native全目标、934CTest、Python17、transport43、SDK709完整性，均PASS；SDK Native2632/35510与新1/51也已PASS。当前32537仍live、正在ASan全目标构建（之后SDK/server完整套与448Native+38ASan/112图CVA）；不要重启原进程，构建活跃时不得改C++，当前无第二个CVA修改者。四组相同自然/强制压力已准备stage-b2-owned-suffix-pressure且started=false，资格stage-b2-owned-suffix-qualification两draft也未密封/未采样，沿用原fixed14/main9b和direct6/A7b的负载/门槛/基线/harness，SDK证据要求改为新2632/35510且不能继承旧SDK。等完整正确性通过再顺序压力→密封117+新增源文件→20组一次资格；采样时禁改源码/文档/快照和禁额外测试/构建。旧0cd两项失败保留。B2及总目标未完成，B3/C/D待做，无网络、默认OFF、未推main。

> 2026-10-26 B2新实质优化候选bbe08be2，SDK1c8ec487（709离线恢复PASS）：sharedPrefixView元数据调查9次累计799300字节、298us，判定不足1MB不值得共享，探针源码已还原。外部malloc_history不能读取自己创建的测试子进程任务端口（非auto-review拒绝），未请求额外权限；已改用进程内BumpAllocator分配/释放/移动与调用PC探针，72092及分析均终态，暖态存活arena507MiB，定位withCachedPrefix深复制suffix同时保留原suffix导致两处约66.8+25.2MiB重复arena。Bump探针源码也已还原，原始记录/符号/安装保留。
> 新APIwithCachedPrefixOwnedSuffix仅对use_count==1且raw parser root、无额外arena owner的suffix接管；复用suffix根、转移Bump arena，避免重写父/preview/trivia图；shared/view/composed保持旧深复制。现有withCachedPrefix不变。SDK内新建suffix组合、prefix扩展及server resumeScopeLibrary使用消费路径；首次promotion仍保持之前回收限定guard，未加预算、未去物理证明、无防抖。新增1组51断言（独占/shared/declaration-view、枚举、元数据、宏引用、释放生命周期）Native PASS，完整SDK2632/35510 PASS。初版测试把timescale放prefix违反既有resumeSafe边界，已移到suffix，没有扩边界。
> 真实固定初始化CVA短机制session68471 exit0，最终612URI诊断/hover与Native一致、原256MiB全DUT和UVM共享；warm malloc live731108272字节，前次同初始化约830MB，与消除92MiB重复arena相符；仅机制非资格，不用短组query中位数证明性能。当前完整正确性pipeline32537活跃：Native全目标/CTest/Python17/transport43/709、ASan全目标→SDK/server完整套，再CVA448Native/38ASan/112图audit。仅此pipeline将修改CVA配置，构建活跃时禁止改C++，等待原32537不要重启。日志correctness-runtime.json。此前所有调查与0cd的20资格均终态；0cd仍两个原门槛失败，原117源快照/失败日志保留不重采。bbe尚无完整正确性/ASan/性能资格，B2未验收，B3/C/D待做，总目标active。实验默认OFF、无网络、未推main。

> 2026-10-26 B2当前0cd56617性能未通过：原fixed14 session29123与direct6 session7850均终态exit1，20组全部采样完、无pending。唯一fixed14失败memory-final-reverse-peakFootprint=1.0650356（限1.05），direct6唯一失败direct-memory-forward-startupMs=1.1951909（限1.10）；其余原门槛通过。117源快照/两份密封计划/原raw/eval全部保留，不改门槛、不重采求通过。当前无活跃构建/测试/CVA修改者。
> 峰值调查：失败逆向pid19275无上下文retired、无新完整模型phase，只有启动构建；701-703编辑3次取消，对应约72.8s RSS由1191559168降至276185088，而foot升至1177601320。原sampler没记录压缩/allocator，不能据此断言系统原因或豁免失败。额外独立counter census session34153已exit0（stage-b2-heap-census），沿用同一失败fixed初始化+reverse1008文本/调度，新增每32处理响应/取消后的现有sourceStorageStatus进程counter与UTC owner/source traces；额外RPC扰动调度，仅归因不参与资格。新实验无压缩，malloc活跃828.8–834.1MB、分配容量1.10–1.13GB、reusable144–309MB，最终612图/hover一致；不替换原失败。下一步量化不可变语法metadata/arena保留和重复占用，减少保留堆；不要盲清BodyDiagnosticCache.completeAnalysis引用，不重复已返回0的malloc压力释放探针。详见heap-analysis.json。
> B1原d88资格仍保留，B2未验收；B3/C/D待做，总目标active，无网络、实验默认OFF、无防抖、未推main。全部资格已结束后才更新本源状态，旧快照仍准确保存采样前版本。

> 2026-10-26 B2采样前状态：0cd56617/SDK54412c3a完整正确性已通过（934CTest、Python17、serverASan919/170782、CVA448Native+38ASan/112解压图audit、同SDK2631/35459两套继承），原256MiB整个DUT与UVM共享保持。压力原45052 exit0终态，4组全部PASS、owner-analysis寿命平衡；自然1MiB两顺序双方各204取消，candidate35 vs base31次回收，RSS约-10%、foot约-2%、CPU约-2%，末次diag约1.3s vs1.8s，不能当正常64MiB延迟或取消已解决。forced64MiB两顺序双方均1回收，RSS约-7%、foot约-8%，完整根最多2且估算193246488字节不变，重叠candidate约179/181ms vsbase187/188ms。保留不利观察forced-forward query p50相对B1约+14.35%（未设门槛机制组），不宣称所有延迟改善；正式20组原门槛待验收。
> 当前无构建/测试/同CVA配置修改者。下一步用stage-b2-prefix-qualification/seal.py在第一资格样本前捕获源快照、密封原fixed14/main9b及direct6/A7b（负载/门槛/基线/harness SHA不变）。后续整个20组资格期间不改源码/文档/快照，不构建/额外测试，不因观察超时重启或重复相同候选采样。该段是密封前记录；进度查stage外runtime，不在采样中更新本文。B2未验收，B3/C/D待做，总目标active，默认OFF，无固定防抖，无网络，未推main。

> 2026-10-26 B2完整正确性完成：0cd56617，SDK54412c3a；原pipeline29316 exit0终态。934CTest、Python17、server ASan919/170782通过；全部CVA448Native+38ASan及112解压完整诊断图audit通过，每状态612URI/hover及实际UVM/全DUT共享均核验，原256MiB预算不变。SDK全部709字节与cc66完整Native/ASan2631/35459相同，继承有明确SHA证据；当前无活跃构建。
> 现唯一CVA修改者是新压力pipeline45052（pid18754/birth51897152043733，runtime.json记录子pid/birth），已确认live，stage-b2-prefix-pressure的4组计划在首样本前预声明。natural-forward/reverse各384编辑、history1MiB；forced-forward/reverse各48、history64MiB/force8，baseline为B1合格d88，当前natural-forward。仅机制归因，不是默认延迟声明或资格重采。等原45052终态，别重启或并发同CVA。
> 后续正式资格已准备独立stage-b2-prefix-qualification（尚未密封/采样）：原fixed14/main9b和direct6/A7b的负载、基线、门槛完全保留，harness SHA不变；coordinator只更新新测试数量，evaluator仅更新B2标签。seal.py要求完整正确性、4组压力passed及owner-analysis、709SDK当前字节、全部测试日志、112 Native archive hashes后捕获源快照并同时密封两计划。源码/交接/状态须在密封前更新；任何资格样本期间不改源码/文档/快照、不构建、不跑额外测试。已保留cc66首次promotion预算失败和两次pytest参数/cwd错误原始记录，不借此重采性能。B2性能未验收、B3/C/D待完成，总目标active；无网络、实验默认OFF、无固定防抖、未推main。

> 2026-10-26 B2当前候选0cd56617（SDK仍54412c3a）：cc66在local-let完整CVA的native frozen初始状态未保留整个DUT，原256MiB预算只保留8子树，失败日志/部分图/全部旧安装保留、不提高预算。原因是loader hook也进入borrowInitialLibraries首次promotion，增加拼接CST保留计费；新guard要求libraryInputs->m_preserveTrustedUvm，仅真实回收可用。增加首次promotion syntaxResumes==0回归，连同宏/未保存/重排/固定根-header编辑-手动重建共3组83断言Native PASS，934CTest PASS。固定编辑边界之前30断言已证实当前原逻辑拒绝，不额外加推测性guard。SDK字节未变，继承cc66阶段Native/ASan2631/35459完整SDK证据；旧server ASan919/170779属于cc66，不能冒充新guard证据。
> 当前唯一CVA pipeline session29316活跃：17Python已PASS（在source cwd明确system+method_scope收集；两次参数/cwd错误原始日志和attempt1/2均保留），完整Native448正在local-let，已越过之前初始DUT预算断言并到native version5，随后其余10场景、ASan构建/完整server、38ASan/audit112图。原13848构建、42673/42348脚本参数失败、67796旧CVA失败都终态；勿重启29316或并发同CVA修改。任何构建活跃时不得改C++。四组自然1MiB/强制64MiB压力新计划stage-b2-prefix-pressure已预声明但未启动，须新correctness-runtime.passed后。未有新候选性能资格；cc66单样本8.4%峰值改善不是新候选或全场景验收。B2未完成，B3/C/D待做，默认实验OFF、无防抖、无网络、未推main。

> 2026-10-26 B2安全边界已实测：新增scope-retirement-fixed-edit回归1组30断言Native PASS；分别显式编辑固定根/包含header，createFromFileLists退休候选均抛RequestFailed，原编辑文本和library generation保持，manual rebuild新generation可查询。未复现漏检，不新增推测性的生产guard。原27448全部exit0，933CTest、SDK Native+ASan2631/35459、server ASan918/170749全部PASS。只追加测试，slang-server仍cc66f0d7/SDK54412c3a，旧source-snapshot保留，新测试另存boundary-source-snapshot。当前扩展pipeline67796活跃：再跑含新case的NativeCTest，增量构建ASan server_unittests并完整运行，随后串行448Native/38ASan/112图audit（脚本已准备，receipt在ASan稳定后创建）。NativeCVA配置修改仅此pipeline未来一步，无其他修改者；构建活跃时不改C++。B2未验收；后续自然1MiB压力和原双向资格门槛尚未开始。无网络、默认OFF、无新增防抖、未推main。

> B2 cc66f0d7验证进度：Native完整build、933CTest、SDK2631/35459已PASS。原完整pipeline27448仍活跃（ASan build，随后SDK/服务器ASan）；不得只凭日志重启，不在构建中改C++。另准备了原448Native+38ASan/112完整图CVA脚本及计划但尚未启动，须等待完整pipeline终态并密封ASan receipt。下一项安全复核：loader hook在新driver登记m_openDocs之前执行，而trustedLibrarySourceContext仅检查已登记打开文档；需验证显式编辑固定根/头文件时仍要求手动重建，必要时在loader恢复前检查旧driver的已知覆盖。此处只是待验证边界，尚未复现运行时错误，不宣称已解决或已确诊。B2未验收、总目标active，无网络、未推main。

> 2026-10-26 B2新候选cc66f0d7（SDK54412c3a）：SourceLoader主single-unit新增瞬态parse override；ServerDriver只在已保留scope prefix、实验cooperative开启且有效配置比对后挂接已有resumeScopeLibrary，使用当前加载buffers顺序与未保存覆盖，顺序不匹配/命名库/独立unit保持原生。hook在异常/正常出口恢复，未缓存物理证明，未新增防抖。新回归1组33断言PASS（宏状态/文本覆盖/重排回退/旧driver销毁/清hook），709SDK离线恢复PASS。89718 forced48机制组终态PASS，最终612URI/hover一致；baseline d88 peakFoot1175995808→candidate1077708120字节，lastDiag31.939→30.687ms。仅一个机制样本，不是性能资格。原harness的scopeStatus在编辑前抓取，故新增97399独立12编辑正确性检查，finalSyntaxResumes Native1/base0/candidate1，确认真实回收命中；不使用其时间重新挑选样本。当前完整Native build→CTest→SDK，再ASan build→SDK→server串行pipeline session27448活跃，状态full-tests.json；任何构建活跃时禁止改C++，同一CVA无活跃修改者。原B1 d88资格保留，B2未验收，B3/C/D待做，无网络、实验默认OFF、未推main。

> 2026-10-26 B2采样更新：原24712、59015及新增57829均exit0终态，当前无活跃构建/测试/CVA配置修改。4组自然/强制owner分析与2组UTC对齐归因均完成；不是资格重采。自然1MiB压力未观察两个完整根重叠，强制回收约183ms重叠。最新candidate99697的最近20ms样本：输入解析前810.41MiB、解析后1054.33MiB、model-ready1119.61MiB；峰值foot1173996960字节，登记源存储约19.4MB。树估算含共享owner，不能作物理内存相加；release是析构入口。最终612URI诊断/hover一致。下一步检查retirement的Driver.parseAllSources重复解析分配：已有固定prefix恢复在SlangDoc路径，不能直接假定适用于SourceLoader原生重放，须保留宏/源顺序/未保存覆盖。B2无生产优化新增、未验收；B1 d88原资格保留，B3/C/D待做。当前可执行文件8b5611a3仅新增可选分阶段跟踪，已保存install与两源码快照；归因见stage-b2-build-attribution-2026-10-26.json。实验默认OFF、无新增防抖、无网络、未推main。

> B2已开始：untracked/stage-b2-natural-overlap-2026-10-26/plan.json在首个新机制样本前预声明4组，run.py执行session24712（pid/出生时间在runtime.json）。自然预算1MiB、384编辑正反，forced8/64MiB/48编辑正反；已有同一CVA负载builder新增用户已有history预算参数和现有MODEL_OWNER_TRACE，保持完整612URI/hover oracle与硬源界限。仅归因模型重叠/自然回收/重复取消，不是重采d88性能资格，不运行资格eval。单一CVA配置修改者，等待原24712不要另起同工程脚本。B1全部原14+6已过且completion审计保存；额外观察固定14/main逆序长测末次diag35.270875 vs31.381792ms（+12.39%，不在原固定14长测predicate内）也保留至B2/B3，不声称每个延迟样本都改善。源码仍d88，默认实验OFF、无网络、未推main；B2/B3/C/D仍待完成。

> 2026-10-26 B1验收完成，当前进入B2，整体B/五类目标未完成。d88b21b2固定14和direct6均qualified=true、无pending/失败，cell826全部终态；b1-completion-audit.json映射所有权/64取消/源隔离/保留consumer/共享prefix及完整测试证据。固定首次38.103→28.561ms/诊断65.897→59.507ms；连续p50约18.3ms；双向1008对main RSS约-5.2%、foot约-6.0%。直接A基线两顺序首次34.376/33.188→29.099/28.778ms，连续p50约18.3ms，末次diag25.37/30.20ms；direct内存近持平（长测foot+0.77%/+0.03%），不要声称全部场景内存下降。跨函数影响p95约470→20ms、diag约969→55ms，foot+2.3–2.5%仍在5%门槛内。强制回收两顺序双方均实际1次。所有原门槛不变、旧失败保留、无相同候选资格重采。下一步B2预声明自然预算压力/重复取消的model-overlap机制工作负载，保持已借用Native分析有效；不能直接释放BodyDiagnosticCache.completeAnalysis。当前无活跃构建/测试/CVA配置修改，无网络，默认实验OFF，未推main。

> 2026-10-26 d88b21b2完整正确性齐备：测试修正后SDK Native/ASan各2631组35459断言全过，服务器932CTest/ASan917组170716断言、Python17、transport43、709SDK验证通过。SDK最终d1bd8f1a（仅测试修正区别于e3），Native/ASan生产代码未因测试修正变化，Native仍d88、ASan2bb32001。Native448已由cell815完成（该cell原ASan分支因无效测试断言失败，未接ASanCVA）；修复流程cell820成功，ASan38+112图audit由cell824成功终态，结果passed。当前无活跃构建/测试/CVA配置修改。准备捕获114文件及源码快照，原14/main9b和direct6/A7b负载、全部门槛不变，双计划首样本前密封，无新性能样本。真实检查点位置表/immutable20.118→8.664MB只是机制证据，不替代进程footprint门槛。采样期间禁止改源码/文档/快照、构建或额外测试，B1未完成、总体目标active，无网络、默认实验OFF、未推main。

> 2026-10-26 新共享前缀候选d88b21b2，SDK e3d6c617/709离线恢复通过：位置表捕获转移原vector到immutablePrefix（元素/元数据地址不变），多SourceManager共享只读前缀+独立尾；旧位置元数据写入才COW，历史预算仍逻辑计费、存储汇总物理去重。新增Native共享/隔离/weak释放/失败事务21断言通过，完整SDK Native2631组35458断言通过。namespace.log仅其它选中5组46断言，wildcard标签没匹配，不能称全namespace套；完整SDK已覆盖所有namespace用例。真实CVA1008检查点物理位置+immutable20.118→8.664MB，存活分配853105760→834931488，配置/文件恢复；这是机制验证，不是性能资格。Native96574/cell810/检查点16628均终态。当前Native全目标68289、ASan74394构建中，禁止C++编辑；接下来完整服务器/SDK ASan及448+38/112后再密封原14+6。ea3逆序长测两项失败保留，不重采求过；B1及总目标未完成，无网络，默认实验OFF，未推main。

> 后续所有权归因已终态：untracked/stage-b-model-memory-profile-2026-10-26/profile.py（64627成功结束）用现有SLANG_MODEL_OWNER_TRACE各运行一次first-edit，不是资格重采。candidate与A基线均只有同等138801200字节AST/36148456估算分析的Native root，baseline-pin均4refs；候选首编辑存活分配815845056、基线804452768（差11392288），都无压缩。BodyDiagnosticCache.completeAnalysis仍被过程/宏行号证明借用，不得为了过门槛强行清除。结论与raw trace/完整612图hover一致性保留在conclusion.json/attribution.json；原ea3反向长测foot+9.92%/末次diag+11.38%仍失败。下一步集中分解源namespace额外分配、allocator空闲页和压力下生命周期重叠，不能直接归因OS或换样本求过。当前所有进程已终态、无构建或CVA配置修改，源码仍ea3，默认实验OFF、无网络，未推main。

> 2026-10-26 ea3a693c的20组已全部终态，cell768结束：固定14全部通过；首次查询37.323→30.494ms，直接加载两顺序34.196/33.711→29.603/28.895ms，修复fa14首次query门槛。direct6仍未通过：仅direct-memory-reverse末次diag31.835167 vs28.582625ms（+11.38%）、peakFootprint910361800 vs828179608（+9.92%）；该candidate有3次取消，CPU+0.30%、RSS+1.27%、p50改善6.95%、p95持平。其结束时compressed400146432、reusable77627392、allocatorInUse848680848；baseline compressed0、reusable194363392、inUse837566864。不能据此就归因OS或忽略失败；全部source/correctness门槛过，raw/install/receipt/114文件snapshot保留，不重采同candidate求过。后续集中生命周期内存/延迟压力归因，不追加无关查询优化。当前无活跃构建/测试/CVA配置修改，实验默认OFF，无网络、未推main，B1及总目标未完成。

> 2026-10-26 ea3a693c完整正确性已通过，cell755成功终态：932CTest、服务器ASan917组170716断言、新回归1/22、package6/312、Python17/transport43/709完整性；SDKddfa95ad未改，明确继承fa14的Native/ASan2630/35437及709恢复证据。CVA全部448 Native+38 ASan、112图audit通过，逐状态612URI/hover及实际UVM/全DUT共享核验。当前没有活跃构建/单元套/CVA配置修改；将捕获114源文件（加入本次新改SlangDoc.cpp与ExplicitContextTests.cpp），密封原14+6，无新性能样本。B1仍未完成；不要把profiling的6ms准备阶段收益视为性能资格。采样中不改源码/文档/快照、不构建/额外测试，B2/B3/C/D排队，默认实验OFF，无网络，未推main。

> 2026-10-26 最新协调：cell755正在运行，独占ASan40269及后续服务器ASan，并在Native6973成功终态/install后完成Python17、transport43、709完整性（均已终态0）。Native CVA448现在由该cell顺序执行；等ASan构建/服务器全套和Native CVA都成功后，自动绑定ASan receipt、执行38状态ASan CVA及112图audit。只poll functions.wait(755)，不要并发poll内部session/再起CVA配置脚本。尚无ea3性能样本，SDKddfa2630/35437明确继承fa14相同SDK证据，932CTest/新case22/package6/312已过，B1未完成。

> 2026-10-26 新候选ea3a693c：fa14固定14过/direct6仅反向首次query+11.8%未过，全部旧样本保留、不重采。分阶段证据定位package上下文语法种类判定，新增仅一条弱CST+路径+完整设计模式缓存，随实验开关启用，完全不缓存物理输入认证；无收益的include搜索map已撤回。真实CVA profiling依赖图7108→1252us、准备上下文13074→7155us，612URI/hover一致；932CTest、新回归1/22、package6/312通过。这不是性能资格。SDK仍ddfa95ad，明确复用fa14 SDK Native/ASan2630/35437和709恢复证据。当前Native全目标6973、ASan40269构建中，任何构建活跃时不要改C++；下一步完整服务器ASan、Python/transport/709、448+38/112再密封原14+6。B1及总目标未完成、B2/B3/C/D排队，未推main，无网络。

> 2026-10-26 fa14efd7 的20组性能已全部终态，cell681结束：固定14全部通过（暖态p50 18.779→17.311ms；首次36.973→35.342ms；双向1008 RSS约-4.9/-5.4%、footprint约-5.7/-11.6%）。direct6内存、连续编辑、强制回收及正确性门槛均通过，但反向首次查询35.642 vs31.880292ms（+11.8%）未过10%门槛，failedChecks仅direct-first-reverse-p50/p95。因此B1仍未验收，不重采fa14求过。全部correctness448+38/112已过；保存原raw/install/receipt/snapshot。接下来用现有阶段跟踪做首次认证归因，profiling不算资格样本。当前无活跃构建、测试或CVA配置修改；无网络，实验默认OFF，未推main。

> 2026-10-26 fa14efd7 正确性齐备：931CTest、SDK Native/ASan各2630组35437断言、服务器ASan916组170694断言、Python17、transport43、709SDK恢复/完整性，CVA6 448 Native+38 ASan状态及112完整诊断图审计均通过。cell669已成功终态，当前无活跃构建/测试/CVA配置修改。将封存112文件收据及快照，保持原固定14/main9b和direct6/A7b全部负载/门槛，双计划在首样本前密封；无性能资格，B1及总目标未完成，B2/B3/C/D排队。采样中不改源码/文档/快照、不构建或额外测试。无网络，实验默认关闭，未推main。

> 2026-10-26 fa14efd7/ASan efb96789：全部单元与集成检查完成，931CTest、SDK Native/ASan各2630组35437断言、服务器ASan916组170694断言、Python17、transport43、SDK709均通过。cell652已成功终态，无活跃构建。cell669现独占等待Native CVA92546，结束后顺序执行38状态ASan及112图审计；不要并发修改同一CVA配置。性能仍未开始，B1尚未验收。见source/validation/data/stage-b-source-table-growth-2026-10-26.json。

> 2026-10-26 新候选fa14efd7：SDK Native完整2630组35437断言、新增扩容回归6151断言通过；同一真实CVA6检查点负载1008编辑后的源位置表总记账24708539→16611275字节（约-33%），这不是进程内存或性能资格。配置/原文件已恢复。cell652独占协调Native39943后续全目标构建/CTest/install与ASan33500后续SDK/server全套；无CVA配置脚本正在执行。新stage untracked/stage-b-source-table-growth-2026-10-26已有正确性脚本及Native preflight receipt，ASan receipt待实际构建完成绑定。后续Python/transport/完整CVA及原14+6仍待验收；B1未完成，默认实验OFF、无网络，未推main。

> 2026-10-26 后续候选正在构建：控制检查点确认真实CVA6在672→840次编辑间当前源位置表由7012878增至14025534字节，固定provider保留表始终10683005字节；存活分配同步增加约6MiB。新增冻结前缀namespace较小几何扩容，保持位置编号与条目append-only；补大前缀追加及原始宏位置回归。SDK709离线恢复通过，patch ddfa95ad。构建/SDK定向、完整SDK与真实源表检查点由cell645顺序执行，尚无新正确性或性能资格；B1未完成，不得沿用60f结果宣称新候选通过。

> 2026-10-26 更新：60f72765 的固定14及direct6已全部结束，正确性通过，性能资格未通过。失败仅为两次正向1008编辑的peakFootprint门槛：固定组+7.67%、直接build组+9.24%；其余既定查询/诊断/CPU/RSS及正确性门槛通过。原始样本保留，不降低门槛、不重复采样同一候选。控制探针在另一次真实CVA6连续1008编辑后尝试malloc_zone_pressure_relief，96MiB及两次全量请求均返回0，内存未变，悬停结果一致；该次空闲页已可复用，不能推断失败样本的压缩页可被回收。临时RPC已移除，探针不算性能资格。继续B1，定位额外存活分配与源位置表增长；B2/B3/C/D仍排队，实验默认关闭，无网络，未推main。

# 阶段 B：连续编辑的模型生命周期

2026-10-26开始。阶段A候选7b205824已通过448+38个CVA6状态、112份完整图及固定14性能验收；阶段B集中解决原第1/4类，首次认证与格式化留到C，最终38组/4032编辑与交付留到D。实验默认关闭，无新增固定防抖，完整输入/版本证明保留。

## 实施顺序与关闭条件

1. **B1：provider历史、失败及取消候选释放。** 先画清ServerDriver、完整/局部Compilation、诊断快照、冻结库和SourceManager的持有关系；覆盖重复取消、构造失败、成功替换及查询释放。用weak_ptr/析构计数和存储统计证明无使用者的候选/旧owner可回收，有使用者的库仍有效；不得为减少峰值破坏诊断事务性。已于d88b21b2完成，见共享前缀阶段的b1-completion-audit.json；下一步B2。
2. **B2：旧/新模型重叠峰值与源存储。** 在B1明确的所有权边界上减少可提前释放的分析缓存及重复候选资源，明确软/硬源预算和冻结库预算的区别。自然回收、强制回收及连续取消必须守住源上限；含UVM/整个DUT冻结、当前未保存文本的诊断和hover与Native一致。正反长测预先声明CPU/查询/诊断不超过10%回退，RSS/footprint不超过5%回退。历史峰值失败仍保留，不用重复相同代码采样求通过。已于6532880f完成，见stage-b2-local-ready-qualification的b2-completion-audit.json；下一步B3。
3. **B3：回收过程的输入接纳、取消传播与原子段。** 新输入应在已支持的安全点取消旧工作，完整保留每次文本变更；明确共享绑定和析构不可中断段，测量输入到查询/诊断的排队延迟。不得只测服务时间，不引入固定等待100ms。需要保证最终版本可查询、诊断最新且没有取消后的重复/过期发布。

每个子项先重现并记录，再实现和定向回归，最后做适当完整Native/ASan与真实CVA6验证。完成B1→B2→B3及阶段门限才进入C；不把源存储受限等价为整个语义模型峰值已解决。

## 初步所有权核查

`Compilation::retainImportedCompilation`会按owner指针去重并强持有导入者；sourceOwnerLease保活SourceManager。`loadConfig`在完整候选构造及取消检查之后才交换m_driver，因此后台取消需要保留旧查询/诊断owner；候选销毁时是否间接留存在共享缓存中仍须验证。同步硬预算回收会先清局部/诊断缓存并尝试releaseUnsharedModel；保留使用者时不能提前释放。尚不能仅凭代码结构断言没有历史链或峰值问题。

## 阶段A仍保留的完整模型边界

A验收覆盖已实施的class普通方法、input-only automatic package/module函数/task、泛型/虚接口/局部类型/import/let、后续宏行号、外部引用及效果闭包。静态/构造方法、一般output/ref或static-lifetime package/module过程、层次值引用、不完整依赖来源、外部常量求值影响设计展开、致命完整设计和受限诊断流等仍受现有保护。外部展开/fatal/受限流已有独立边界回归；不把这些完整模型路径称作局部优化成功，也不声称任意修改均可局部化。后续发现真实合法覆盖缺口按问题记录，不能靠放松保护隐去。

## B1 首轮证据（2026-10-26）

新增跨driver回归连续64个不同RTL参数候选：数据流阶段取消，独立冻结体可入缓存，但整个候选Compilation必须释放；预算淘汰后候选SourceLifetime必须释放，缓存存在时被查询使用的对象仍有效。Native和ASan定向各2组892断言通过（ASan leak关闭），生产字节仍是已验收7b候选。原UVM已有手动重建后SourceLifetime释放测试，不能重复声称没有这一覆盖。

发现仍需解决的首代保活：固定库的sourceOwner与最初driver共用SourceLifetime。旧模型销毁后，provider仍保留最初SourceManager；synthetic probe追加4MiB无关文本，源存储记账从2271增到12585479字节，body cache预算1048576字节。这不是RSS样本，body cache预算也不等同所有源存储预算；它证明了固定provider可以间接保留后续无关编辑文本，不能仅凭Compilation过期判定整代资源回收。缓存/所有消费者销毁后这些资源都释放，因此当前证据不是全局永久泄漏。

下一步使固定provider持有独立固定源命名空间，避免绑定初始编辑代。必须保留准确BufferID、宏展开/文件名、SourceLibrary字符串、延迟泛型绑定、库手动重建和仍存活消费者；不能简单清空旧SourceManager或改变仍被使用的Compilation。实现时优先复用已解析树和语义，不以再次解析UVM换取释放；若需要调整已完成provider的源绑定，须核对SyntaxTree namespace视图的原始owner生命周期并补ASan。B1尚未关闭，B2/B3/C/D排队。

探针勘误：上段初版assignText(path,text)参数反置，4MiB放在路径而非正文；12585479字节是长路径相关记账，不可作为4MiB正文保留量。原始日志与ownership-audit-long-path-original.json保留，现有记录加勘误。正在用正确正文参数，分别复测直接bootstrap的原始保活机制和编辑factory隔离效果。该参数错误不改变已验证的64次候选模型/缓存源lease释放关系，但正文内存数值必须以新结果为准。

## B1 隔离实现与当前验收（3022f87e）

编辑factory在固定库准备完毕后创建独立consumer driver，复用原语义库与cached prefix；固定provider的SourceManager从此不再接收用户编辑。初始化真实old driver的未保存overlays独立传递，bootstrap析构不发布诊断，首次cold/repair缓存状态保持原值。没有调整已绑定Compilation或SyntaxTree内部SourceManager，也没有改变SDK。

完整回归发现5个真实边界失败后，修复继承prefix中已有owned overlay的登记（相同文本复用，不同文本replaceBuffer）及固定模块私有fallback的namespace视图；保留原始5失败日志且未改旧测试要求。当前927CTest、服务器ASan912组170307断言、Python17、transport43及SDK709完整性通过；真实CVA6当前448+38状态与112图尚未完成，不计性能资格。

正确正文探针：直接bootstrap复现原始共用SM机制，追加4194304字节正文后记账1848→8390733；经过编辑factory隔离后provider保持1848→1848。64候选取消/淘汰和旧查询/保留consumer验证通过。这是合成可达性/记账证据，不能当RSS或真实项目内存收益。

性能计划保留原固定14/main基线。另预声明6组直接build配置初始化作业，与阶段A已验收7b比较：首次启动正反、1008编辑正反、每8次编辑触发回收的48编辑正反。启动到选定完整DUT、查询p50/p95、诊断及CPU最多10%回退，RSS/footprint最多5%回退；完整612URI图、取消、源hard/query上限必须通过，强制回收两臂必须实际发生。唯一源存储占用单独报告，不把源记账比例当RSS门槛；仍使用用户可配置的既定源预算。脚本及门槛必须在任一性能作业前密封；计划尚未开始。新增评估器11个合成正/负门槛检查通过，不属于真实负载结果。

## B1 两种独立RTL体源命名空间（87b7f886）

稳定DUT参数回归发现并复现：旧Compilation销毁并不代表旧SourceManager能释放，BodyCache独立owner原先仍保活编辑driver。最初紧凑路径与完整回退分别有失败记录，完整路径至少2项生命周期/源占用检查失败。现在紧凑上下文在独立SM中解析一次生成wrapper；完整回退调用SDK forkRetainedNamespace复制原生命名空间，保持BufferID/base-prefix和原CST节点，不重复解析。每批缓存共享自己的namespace，未来编辑不再追加其中，所有独立SM都进入SourceStorageRegistry弱观察统计。

完整快照同时处理FileData目录自持、展开名称及诊断directive名称自持、私有query缓冲排除、源库身份小型lease及PrecompiledUvmSyntax命名空间视图。TrustedUvmImport的选项bag改为弱源lease，语义绑定时才获取强lease，防止CST options/namespaceOwner间接保活编辑SM；存活Compilation和ShallowAnalysis仍拥有强源lease。快照是固定依赖的数据表示，不是跨请求成功输入认证缓存。

紧凑/完整 × 初始/编辑后四组合均验证旧模型及旧编辑源lease释放、新实例值和缓存实际命中、仍存活consumer有效、统计包含独立缓存SM且无4MiB无关编辑数据/retained query。SDK新2组17断言还验证原SM释放后的宏名、line directive、相对include和语法视图。当前完整928CTest、服务器ASan913组170389断言、SDK Native/ASan各2628组29267断言、Python17、transport43通过，ASan leak关闭。709文件离线恢复补丁1ad0093d，gitlink保持151a801；安装资源由当前build重新安装，不混用旧ABI的预解析结果。

87b7真实CVA6的448个Native状态、38个ASan状态及112份完整图已全部通过审计；性能20组未开始；B1未关闭。完整快照仍会保留创建时的非query原生namespace数据，受缓存预算计费；它不会随之后的编辑增长。后续峰值、候选重叠及回收接纳继续按B2/B3处理，不能把本次释放回归等价为这些问题已全部解决。

## B1 初始化重复解析后续

87b7完整正确性通过，但fixed14暖态中位查询+10.6%，direct6启动约+20%、常规footprint约+6.8%，性能未验收。下一候选只在同次初始化隔离中复用刚生成的native语法树与原始read/negative/include记录，保留独立编辑SM与发布前认证；retirement/reload不复用认证。named-library/library-map仍走原解析。新增输入变化、负lookup与源生命周期回归；仍需完整正确性及新候选预声明性能验收。

9d7c初始化复用候选已完成929CTest、服务器ASAN914组170406断言、SDK Native/ASAN各2629组29283断言、52组scope定向，以及17Python/43transport/709 SDK完整性核验。真实CVA6十一场景448个Native状态、38个ASAN状态、112份完整图全部审计通过。接下来沿用固定14/main基线与direct6/A基线评估，门槛不变；新性能未验证，B1尚未关闭。

B1后续真实负载证明：9d7c虽通过固定14，但direct6仍有启动/内存回退。元数据trace发现CVA原SM在capture后登记同一uvm_pkg.sv的额外BufferID（62614 vs62613），使严格等长guard拒绝复用。a7b改为仅允许tail FileInfo共享已捕获的同一FileData指针；新正文、query和macro expansion仍拒绝。真实CVA启动已确认命中，不把小型回归当大项目命中证明。929CTest、服务器ASan914/170406、SDK Native/ASan2629/29286与52组scope定向通过；完整CVA及新性能资格待完成。

a7b完整正确性验证已通过：448 Native、38 ASAN状态和112份完整Native诊断图审计，逐状态核对612URI与hover及UVM/完整DUT实际复用。进入原固定14/main基线及direct6/A基线性能评估，门槛保持不变；B1仍待全部性能条件通过。

## B1验收剩余的文件认证尾延迟

a7b的固定14通过，direct6中只有forward1008末次诊断42ms vs31ms未通过；初始化及内存已过原门槛，不能重采旧候选求过。失败现有trace显示查询文件认证约23ms、发布前约6.2ms，保留逐文件完整证明要求。当前60f72765按已排序物理路径只在一次认证内保留一个目录FD，相邻文件openat，最终descriptor路径、绝对leaf元数据与完整字节比较保持，负向打开重试绝对路径，句柄资源不足释放后退回原路径。不增加跨请求成功缓存、并行回调或固定防抖。11组11807文件证明回归含低48FD限额、取消释放、目录移动；931CTest与ASan916/170694通过，真实CVA及性能资格待完成。SDK仍5c94d370未改，通过明确来源复用既有SDK两套测试证据。

60f72765完整正确性验证已通过：448 Native状态、38 ASAN状态及112份无损Native完整图审计，逐状态612URI/hover与UVM/完整DUT实际共享已核验。进入原固定14/main与direct6/A的性能评估，门槛不变；B1仍待所有性能门槛通过。SDK未改的继承证据及来源hash一并绑定，未声称重跑SDK。

## B2 独占后缀的 arena 接管约定

`withCachedPrefix` 保留原来的非破坏性组合行为。新入口 `withCachedPrefixOwnedSuffix` 接收调用方交出的后缀；只有唯一持有、由原生 parser 新建、拥有完整根节点且不额外保留其他 arena 的树可以接管。共享树、命名空间视图、声明视图和已经组合的树继续复制。

接管路径复用后缀的 compilation-unit 根，把前缀成员加入该根的成员列表，并把后缀 arena 移入结果。后缀节点及枚举 preview、指令 trivia 的父关系保持指向同一个根；前缀节点的物理祖先保持不变。结果继续保留前缀、后缀元数据和源缓冲区，调用方不得再访问交出的后缀。扩展前缀时，恢复所需的指令摘要、include guards 和工作目录在交出后缀之前保存。

解析得到的“可交出根”标记仅在内存中使用，不写入预解析文件。用户的实验配置默认值、依赖固定及手动重建约定、物理输入与版本校验、源历史和冻结缓存预算均不改变，也不新增防抖。

本项的验收包括独占复用、共享和借用视图回退、Native 语义诊断和元数据一致、宏引用及释放后的生命周期检查；还需完整 SDK/server ASan、真实 CVA6 和原性能门槛。约 92 MiB 重复 arena 的定位和短场景堆占用下降只作为机制证据，不能代替完整验收。此前 0cd 候选的两项失败结果保留。

## B3 硬预算期间的输入暂存实施顺序

已确认1MiB/20ms压力下的主要同步段是新driver的解析与模型建立，而不是旧driver析构。不能仅取消硬回收后继续向旧SourceManager追加文本，否则重复取消会绕过原硬上限。

1. 提取现有协议文本变更事务，按每个contentChanges的顺序处理UTF-16/协商UTF-8坐标；原SlangDoc同步也使用同一函数，避免暂存路径另写一套范围处理。当前DocumentEdits已提取，三组24断言、941CTest及17Python通过；真实CVA范围/回收重测进行中。
2. 硬预算触发后，以每个打开文档的一份最新文本及版本暂存后续编辑，不再为这些中间版本向旧源命名空间追加buffer。保留每次范围变更的顺序；候选构造取得独立文本/版本快照。暂存文本的占用单独报告，不能以隐藏该占用宣称预算或RSS改善。
3. 将最新暂存文本和版本送入私有替换driver，在现有解析检查点和disposable-root检查点传播取消。取消销毁候选、保留旧owner及最新暂存文本，不发布半成品，也不把部分绑定模型当作可继续模型。成功才交换driver并清理已消费的暂存版本。
4. 语义查询和诊断发布必须先完成最新输入接纳，不能从旧driver回答尚未接纳的版本；保存、关闭、重新打开、配置重载及多文档变更也必须有明确的接纳/丢弃规则。普通didChange通知继续同步每个范围操作，不因取消丢字符。保持原硬预算、完整内容/输入版本证明及无新增固定防抖。
5. 在范围增量、Unicode、多次同通知修改、取消发生在输入/模型/提交边界，以及保存/关闭/重载交错上补确定性回归。再做真实CVA反复取消及硬预算压力，报告包含排队的输入到查询/诊断耗时、源存储和进程峰值，以及原生完整诊断图。测试通过只证明相应边界，不以普通预算短测关闭硬预算问题。

步骤2–4已接入当前实验原型：独立最新文本、可取消候选接纳和未使用旧语义释放；三个取消边界与query/save/close/reopen/build-reload交错已过定向回归。步骤5完整验收尚未结束；多文档/无效配置恢复及共享绑定或析构不可中断区段仍需核对，不预先承诺完全消除所有停顿。

B3补充边界（c7e48322）：真实双文档无效配置wire测试已发现并修复关闭通知在admission失败时被丢弃的问题；重新打开和保存附带全文仍有同类失败，故步骤4尚未完成。父4e7的448+38/112审计已过，但ASan原924case缺新文本事务文件三case，已离线重新配置发现该文件，需修复其余通知后重编译并补全覆盖。

B3通知输入更新（2bc8252a）：无效配置下open/reopen、save全文及close现均独立同步，未落盘新根显式登记编辑器overlay；保存索引重放位于原生认证之后，局部hover不提前触发它。Native/ASan六场景协议对照与索引Native对照通过；完整ASan927/172107通过，注册名称比对928一致补齐前述发现缺口。CVA广覆盖、取消全时延及原性能资格仍待结束，B3保持未完成。
