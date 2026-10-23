# 性能优化五大类进展

五类总目标仍active。本阶段补齐owner/包提示/依赖等解析的取消传播，修复浅层选项刷新丢回调、包提示吞取消，以及部分索引缓存提交问题。实验默认关闭，正常输入/查询/诊断无新增固定防抖。

| 大类 | 当前已完成 | 尚待完成 |
| --- | --- | --- |
| 1. 响应调度与卡顿控制 | 过期取消/事务替换/版本证明；回收实耗重试和前台活动后移，输入/wake/当前诊断优先；owner、包提示、依赖、快照、引用、CodeLens和top依赖的可丢弃解析取消传播及完整缓存提交 | 完整模型ensureCurrent/refresh/构造的ctx传播和原子段；完整语义安全取消；提交后状态更新义务、硬限回收及Native回退仍有成本 |
| 2. 认证与依赖校验降耗 | 续期/最终证明分离；复用raw诊断归属；源见证去重、Mac完整内容证明、依赖变化重载及最新文本保留；取消不固定半成品hint/interface结果 | 完整文件证明约7～8ms；更大规模及外围诊断分区，持续首次/尾延迟验证 |
| 3. 扩大局部更新能力 | 函数/任务体查询、合格类方法诊断；固定库原始引用证明；后续日志运行时行号证明；当前展示/补全票据 | 一般新增/移除引用和package/static/task影响闭包；package函数/module automatic任务及更多上下文；声明/类型/非运行时行号影响仍Native |
| 4. 缓存生命周期与内存 | UVM/package/module/层次复用及预算回退；实际source/query/pinned计量与私有消费者释放；回收重复取消降耗/硬限保持；弱hint owner不固定旧文本 | source位置表及普通Native历史、保留初始SM后缀；旧/新owner重叠、用户source预算及失败回收；更长连续输入边界 |
| 5. 真实负载与交付 | 同一CVA6完整验证环境、UVM/404DUT冻结；三轮常规/类成员Native编辑、1008输入、双序内存/CPU、强制回收/2秒自主空闲；56公开状态含14全不冻结；820CTest含2594SDK/17Python/43transport及709恢复通过 | 更多已有大型项目；持续每阶段同场景回归，五类总验收和最终交付/清理 |

类成员（函数/任务之外）编辑三轮中位：最后成功查询 880.37→784.32ms，末次诊断 1211.33→1109.62ms，原状态命令的队列应答p50 322.14→102.39ms。它仍走Native，不是新增局部更新覆盖。
普通1008方法体输入新版全部成功，旧版2次内容变化取消（中间版本）：成功查询p50/p95 26.77/31.27→26.07/30.43ms，最大 54.30→84.71ms，末次诊断 32.38→37.83ms。所有负差值、双序内存/CPU和回退数据完整保留于报告。

下一步推进完整模型构造与ensureCurrent等ctx传播，再建立完整语义的安全取消边界；继续一般影响闭包、更多局部上下文、source历史/预算/峰值及其他负载。不会以这些解析路径完成代替整类完成。

详情：[owner取消报告](owner-cancellation-evaluation.md)、[回收维护](maintenance-retry-evaluation.md)、[源码存储](source-storage-evaluation.md)、[后续宏](later-macro-line-evaluation.md)、[固定引用](fixed-provider-reference-evaluation.md)、[输入恢复](method-input-recovery-evaluation.md)、[支持边界](../features/method-body-certification.md)。
