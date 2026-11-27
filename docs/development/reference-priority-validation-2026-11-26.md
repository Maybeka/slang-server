# 参考实现剩余 12 项实测 · 2026-11-26

**后续验收已完成。** 项目范围内的 10 项均有回归测试，9 项修复了实现，#651 在探索模式和 `--single-unit` 下均已支持。Mac 上通过了 1,112 个 CTest 条目、42 个 Python 测试和 43 个独立诊断传输测试。CVA6、OpenTitan SoC、USBDEV 共验证 540 个编辑状态，修复前后两种分析模式的悬停结果和完整诊断逐项一致。验收数据保留了编译器和二进制版本、每轮用时和峰值内存。[十项修复与验收数据](../../validation/data/reference-ten-acceptance-2026-11-26.json)

结构编辑的增量开销主要来自 #624 的文件 URI 编码。插入一个声明时，每个项目会编码约 1.9 万至 4.2 万次文件路径，绝大部分来自诊断位置和关联位置。独立测量版本在同一进程中交替使用两种编码方式。对无需转义的路径整段复制后，CVA6、OpenTitan SoC、USBDEV 的路径编码时间中位数分别减少 11.48、12.74、6.83 ms，悬停和诊断结果一致。函数体内的局部编辑只触发 3 至 16 次路径编码。[对照方法与各轮数据](../../validation/data/reference-ten-acceptance-2026-11-26.json)

普通文件路径的整段复制已进入正式版本。Mac 上 1,113 个 CTest 条目、42 个 Python 测试和 43 个诊断传输测试全部通过。三轮对照中，CVA6 和 USBDEV 的结构编辑诊断中位数分别缩短 11.7 和 9.1 ms，OpenTitan SoC 的中位数增加 2.1 ms。针对 SoC 又交替运行了六对进程，首次结构编辑的配对耗时中位数缩短 12.9 ms，重复结构编辑缩短 15.6 ms。两次验证的全部悬停和诊断均与原生结果一致。[正式版本与复测数据](../../validation/data/file-uri-structural-diagnostics-2026-11-26.json)

## 修复前的逐项评估

Mac 上的独立构建来自 `main` 的 `d474d26`，使用官方 slang v12.2 和项目锁定的补丁，实验选项沿用默认关闭。**建议下一批先修 #650 和 #596**。两者都发生在编辑器寻找依赖和头文件上下文时。一个普通的文件删除就能让跳转报内部错误，另一个则能在跳转目标完全正确时显示错误位宽。[版本与逐项数据](../../validation/data/reference-priority-validation-2026-11-26.json)

12 项中有 10 项属于项目范围。1 项在验收场景通过，5 项在原样 main 上直接观察到错误，3 项通过观测构建定位了失效条件或能力缺口，1 项仅确认了输入校验缺口。另 2 项涉及波形查看器，仅保留历史测量。观测构建在可能失效的操作之前记录状态并安全返回，波形监听限于本机。表中明确标出了这些结果。39 个相关 C++ 测试（651 项断言）与 11 个编辑器协议测试通过。此轮做功能评估，没有变更产品代码。[原始评审范围](reference-feature-review-2026-11-26.md)

| 项 | 判断 | 对照测试和实际结果 |
| --- | --- | --- |
| #650 删除与索引 | **main 复现** | 单独做完整重索引，保留、已删除、被移出范围的三个符号从 1/1/1 变为 1/0/0。若编辑器先打开一个仍引用已删除文件的模块，而删除通知未送达，跳转返回 `-32603`；随后配置重载也因文件缺失而中断，旧索引仍在。同路径重建后能查到新符号，已打开的未保存文本仍能定位。[配置测试](https://github.com/hudson-trading/slang-server/blob/1a4a6c0c6b27a7917fad74f1fa2d0ba113bc54c2/tests/cpp/ConfigTests.cpp) · [重索引测试](https://github.com/hudson-trading/slang-server/blob/1a4a6c0c6b27a7917fad74f1fa2d0ba113bc54c2/tests/cpp/IndexerTests.cpp) |
| #651 分析时发现新文档 | **所测行为已支持** | 顶层 A 引用未打开的 B，两个文件各有一个独立错误。有 filelist 的 `--single-unit` 模式只打开 A 就发布 A/B 各 1 条；修好 B 后为 1/0。探索模式能在打开 B 前跳到 B，先发布 A 的错误，打开 B 后两份诊断齐全，修复后同样为 1/0。每种模式各跑 3 个新进程，修复前后均能定位 B。参考提交没有配套 server 测试。[参考条目](https://github.com/hudson-trading/slang-server/commit/ceea37a989d25900e8aced48b276d9bfc40ce980) |
| #580 include 深度与恢复 | **main 复现重复诊断** | 用有限自包含文件和深度上限 1，触及上限后同一文件出现 2 条深度错误，参考测试要求 1 条。去掉回边后为 0，符号跳转恢复。普通 include guard 也能正常结束。还在有限的多层头文件链上得到相同的重复诊断；这些测量没有给出任意递归输入的最坏耗时。[参考测试](https://github.com/hudson-trading/slang-server/blob/0ad4b830ecd5df2f7d3329b61022c1fea4df528e/tests/cpp/DriverTests.cpp) |
| #596 互相引用的头文件上下文 | **main 复现错误位宽** | 工程有两个源码文件和两个头文件，两个接口分别应为 4 和 8 位。配置构建下，从四个文件分别首次进入，跨文件定义编辑前后均正确，悬停却均显示 2/2 位；编辑右侧头文件后应为 4/9，右侧连数值宽度也未返回。探索模式的观测构建在四种入口均拦到同一文档的分析尚未完成又开始刷新；单一包含者对照能正确显示 4 位。[参考测试](https://github.com/hudson-trading/slang-server/blob/310dae4371d063d39c1d9bc9b35680ca3eaa6f34/tests/cpp/DriverTests.cpp) |
| #624 文件 URI 与重命名 | **main 复现** | 参考单文件场景返回了应有的 2 处编辑，但 URI 末段输出为 `a %23#.sv`，预期为 `a%20%2523%23.sv`。按 URI 解析后找不到原文件。跨文件测试返回两组编辑，其中只有普通文件的 URI 可定位；带中文、空格、百分号和井号的目标失败。通过两种等价编码修改和修复时，服务端仍能发布并清除同一条诊断。[重命名测试](https://github.com/hudson-trading/slang-server/blob/7c46697016da28d5ca7e0168b3658a7756275ba6/tests/cpp/ReferencesTests.cpp) · [URI 测试](https://github.com/hudson-trading/slang-server/blob/7c46697016da28d5ca7e0168b3658a7756275ba6/tests/cpp/URITests.cpp) |
| #589 按光标位置补全宏 | **main 复现** | 同名宏从一个参数改为两个，前后两个补全位置都展示末次的双参定义；参数行内提示却能分别给出两次调用的正确参数名。含 1,500 个其他宏的头文件在 include 之前就进入候选，局部宏在 `undefineall` 后仍会被索引补回。命令行宏在使用处能补全，未调用的头文件宏也能找到。[补全测试](https://github.com/hudson-trading/slang-server/blob/07991aa071d1e9f99b850096a8baf570a4cbb57a/tests/cpp/CompletionTests.cpp) · [提示测试](https://github.com/hudson-trading/slang-server/blob/07991aa071d1e9f99b850096a8baf570a4cbb57a/tests/cpp/InlayHintTests.cpp) |
| #617 虚接口驱动与递归 foreach | **部分已支持，SDK 有缺口** | main 上，含虚接口句柄和两个接口实例的工程零诊断。4 个驱动查询均能分清实例，分别追到顶层 a 和 b，通过句柄跳转也能定位成员。普通包导出、特化类与 foreach 组合可以分析。按参考 SDK 测试先强制特化，再访问循环块成员，观测构建记录到重复绑定和未解析的数组表达式；加保护后有 1 个循环块，恰好留下参考预期的 1 个 `UndeclaredIdentifier`。该提交的 SDK 指针区间只新增了 foreach 测试。驱动结论限于本地测过的连接关系。[参考提交](https://github.com/hudson-trading/slang-server/commit/9de96efe0d80103382d3d5c002194826655b6de9) · [SDK 测试](https://github.com/AndrewNolte/slang/blob/d87035964aee3cb60c977c7431f1f6b36bd43b97/tests/unittests/ast/StatementTests.cpp) |
| #611 未连接端口的负载 | **观测确认缺口** | main 上全连接端口的驱动和负载正确。观测构建遇到未连接输入、输出时记录到空连接表达式；跳过后，已连接源只返回子实例的有效输入，未连接输出的负载为空。两次查询均与参考集合及声明位置吻合。[参考测试](https://github.com/hudson-trading/slang-server/blob/7a81185186c1e8499645792c3fccee7479a5144e/tests/cpp/ConeTests.cpp) |
| #642 无内部符号的显式端口 | **观测确认缺口** | 正常的显式端口可以没有内部符号。观测构建在建立引用索引和收集信号锥结果两处都遇到了这种情况。两处均处理后，被查询线路保留 1 个有效本地负载，驱动为空，匹配参考断言。仅处理 #611 的空连接仍会遗漏这个入口。[参考测试](https://github.com/hudson-trading/slang-server/blob/47b01fd69fcef5496789d4611c96d63ce211cb55/tests/cpp/ConeTests.cpp) |
| #645 消息读取与关闭 | **已定位校验缺口** | main 能读取分 19 段写入、带 Content-Type 的正常消息，随后查询成功，退出码 0。当前读取器在长度转换、分配和读满检查之间缺少验证。带校验的观测构建拒绝无效长度后仍处理下一次查询，正文未收完时也能结束。异常输入恢复仅在有保护的构建验收，原样 main 的异常会话结果未直接测定。[参考读取器测试](https://github.com/hudson-trading/slang-server/blob/4349e86487d3766b77f156a01fbc695e4854007f/tests/cpp/JsonRpcServerTests.cpp) · [本项目读取器](../../include/lsp/JsonRpc.h) |
| #648 等待波形查看器 | **范围外，历史测量** | 本机查看器替身推迟 450 ms 连接，在此期间发送编辑和悬停。观测构建的空闲悬停为 0.128 ms，等待期间的悬停为 472.444 ms，握手前没有回复，握手后返回新文本的结果。这是一次人为控制的等待实验。当前代码的 accept 没有连接期限，后续的问候超时从连接成功后才开始计时。参考提交没有配套测试。[参考条目](https://github.com/hudson-trading/slang-server/commit/1ca3ef76e9c1559fc39cb11d718a3dca0f521743) · [连接流程](../../src/ast/WcpClient.cpp) |
| #643 波形退出和重连 | **范围外，历史测量** | 同一查看器成功加载两个文件。查看器正常退出后，在替换会话和最后销毁会话时分别测得会话运行标志已清零、线程对象仍待 join。观测构建补做回收后，第二个查看器收到第三个文件，悬停可用，server 退出码 0。参考提交没有配套测试。[参考条目](https://github.com/hudson-trading/slang-server/commit/b93cfb88f80e630091319a10bafda8e14a1aa55f) · [会话对象](../../include/ast/WcpClient.h) |

## 建议实施顺序

1. **下一批处理依赖失效和包含上下文，先 #650，再 #596。** 文件缺失应作为一次可恢复的查找失败，避免打断配置重载；同路径重建或存在未保存缓冲时重新核对来源。随后让同一版本中正在分析的包含者可以被依赖查询识别，头文件查询使用对应编译单元的宏状态。验收要同时看跳转目标、4/8 → 4/9 的实际位宽、诊断清除，以及连续编辑后仍使用最新文本。保留 #651 的 A/B 诊断作为回归对照。
2. **接着成对处理信号追踪 #611、#642。** 在索引、驱动、负载与位置输出共用一致的端口有效性判断。验收同时检查有效端点、空结果和声明位置，防止为了跳过一个空端口而丢掉其他连接。
3. **随后修 #624，再修 #589。** 先保证跨文件编辑的每个 URI 都能往返到原路径；再令宏补全取光标处可见的定义，并保住现有正确的参数提示。宏表场景回归首次及连续补全，避免每次输入扫描整个宏表。
4. **其余按责任边界收尾。** 传输层单独补 #645 的输入校验和恢复测试。SDK 补丁集中处理 #580 和 #617 的 foreach 子项。
