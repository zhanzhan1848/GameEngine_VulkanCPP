# 玩法规则骨架与人机共创回路

> 日期：2026-10-28；状态：📋 规划，未实施（S1 待启动）。阶段属于本模块，不代表全引擎排期。
> 上级：[总规划与统一验收](../../2026-10-04-agent-production-and-3d-printing-roadmap.md)。所有阶段均须满足总规划的代码、交互、成本和视觉门禁。

## 现有代码复核与规划调整（2026-10-28）

| 项目 | 结论 |
|---|---|
| 基线状态 | 事件总线、脚本生命周期、Lua 生产链与属性/事件契约已通；无状态机/触发器/任务容器；全仓无行为树/Blackboard/NPC AI 代码（`Engine/Navigation/` 仅寻路） |
| 已核对源码 | [Engine/Components/ScriptEventBus.h](../../../Engine/Components/ScriptEventBus.h)、[Engine/Components/Script.cpp](../../../Engine/Components/Script.cpp)（生命周期与异常边界）、[Engine/Scripting/LuaBackend.cpp](../../../Engine/Scripting/LuaBackend.cpp) 及 AudioLua/CharacterLua/InputLua/NavLua、[Tools/PrimalPlayer/main.cpp](../../../Tools/PrimalPlayer/main.cpp)（行协议包解析）、[Engine/Navigation/](../../../Engine/Navigation/)（独立目录先例）、[脚本子规划 S2 契约](scripting-properties-and-events.md) |
| 代码证据与限制 | `emit_by_name`/`subscribe_by_name` 字符串键 C ABI 通道、FIFO 投递、重发隔离、depth cap 8、drain 后 deferred 订阅变更与 compact 均已实现；Lua 侧已有 audio/input/character/nav/bus/file 表，无 `game` 表；热重载"失败保留上一可运行版本"语义随 scripting S2 已定；Player 为单文件行协议解析。本次为静态复核，未运行测试 |
| 对本模块规划的调整 | 不做题材框架（沿 scripting 子规划"不内置题材限定"边界），只交付通用规则容器 + 确定性解释器 + 受控写入口 + 人在环三道门；类 RSI NPC 回路为 S4 扩展，依赖 S2/S3 产物；不引入行为树/Blackboard |

本次为静态源码/调用路径复核：实现、空分支、测试代码和注释分别判断；未运行功能/交互/视觉测试，不能据此标为已验收。未找到的能力只指本次限定的自有源码与入口，不证明所有历史分支或外部软件均不存在。共同断点与全模块结果见 [代码基线评估](code-baseline-review.md)。

## 最终目标

通用规则容器（状态机/触发器/任务的薄数据契约）+ 确定性解释器 + AI 写 Lua 响应 + 人在环确认/共创回路；S4 在同一骨架上以受限自我改进回路消除 NPC 行为重复。数值、曲线、条件与人设参数始终由人持有并可 Undo；AI 产出永远以提案形式进入，经行为证据与人确认后原子生效；全程不重编引擎，渲染/物理/流体逻辑零改动。

设计动机：商业引擎把游戏逻辑沿"程序员→脚本→可视化→数据资产"光谱推向策划可改；本模块把"作者"换成 AI，但把**确认权**留给人——与 agent 工程/harness 工程的 plan-approval 同构。

## 设计总纲：四层模型

| 层 | 内容 | 归属 |
|---|---|---|
| L0 数据契约 | `GameplayStateMachine`（状态表+转移表，guard 为声明式条件而非代码）、`Trigger`（空间/事件/属性阈值三类谓词→发事件）、`Task`（目标+完成/失败条件，复用同一封闭谓词集：属性比较/距离/计时器/事件计数）。场景组件+v2 包行；编辑走文档模型+UndoRedo | 人（数值/条件/人设参数） |
| L1 确定性解释器 | 新 `Engine/Gameplay/`（沿 Navigation 独立目录先例）。每 tick 纯数据求值→经 ScriptEventBus 字符串通道（`emit_by_name`）发事件；同输入同帧序同事件流，可录像回放。不含任何题材语义 | 引擎 |
| L2 Lua 响应层 | 复用现有生命周期（begin_play/update/fixed_update/late_update/destroy）与 bus 订阅；新增 `game` 表四个受控原语：`game.state.get/transition`、`game.task.complete/fail`。契约写入口收窄于此，禁止裸写属性绕过状态机——人改的数据不被脚本静默篡改 | AI（提案） |
| L3 人在环三道门 | ①提案门：AI 产出永远是提案（Lua diff + 契约 diff 落 `*.proposed` 不直接生效），契约面过 schema 快照门禁；②证据门：固定种子 dry-run 跑 N 帧，录 PNG+CSV+事件流 trace 出 verdict（复用 workflows S2 拓扑序/断点恢复/幂等键与 task.* 长任务登记）；③确认门：人决定面收到最小（approve / reject+原因 / 只调参数=改 L0 数据不需要 AI 重写），确认后原子热切换生效，驳回原因回喂 AI 迭代 | 人（最小决定面） |

S4 世代化回路：NPC = L0 契约实例 + Lua 行为模块 + 版本化记忆（key-value 边车文件）。外层 harness 驱动四步：观察（trace）→提案（N 个行为变体）→评估（dry-run + 任务成功率 + 行为熵/动作 n-gram 新颖度）→择优应用（经确认门，在世代切换点原子生效）。LLM 调用只发生在 PrimalCli/MCP 侧，引擎不感知、不内置。

消除重复的三个来源：效用选择替代固定遍历（转移不在 L0 写死顺序，AI 在 Lua 按人格权重打分）；记忆驱动变化（NPC 记住玩家交互史，响应随历史分叉）；多样性进评估函数（重复感成为可度量回归信号被门禁拦截）。

## 本模块阶段与验收

| 阶段 | 交付范围 | 代码与数据验收 | 交互、成本与视觉验收 |
|---|---|---|---|
| S1 状态机+触发器闭环 | L0 两契约+解释器+`game` 表+EngineDLL 导出+Player 包行+E2E 独立场景（靠近按钮→事件→Lua 开门） | 同输入同帧序同事件流可回放；非法转移/悬空状态/谓词越界有确定语义；保存重开与 Player 同行为；tick 位置冻结项：解释器插在 script frame_tick 之后、物理步进之前（S1 启动时定死不再变更，它直接决定确定性） | 模板起步 5 次操作内跑通门开关样例；不重编引擎；错误可定位到契约行/脚本文件行 |
| S2 任务契约+证据门 | `Task` 契约+事件流 trace/dry-run harness+`gameplay.propose/dryrun/apply` 命令面（schema 快照随提交重生成） | trace 完整（事件/帧号/实体可关联）；固定种子逐位复现；提案 diff 在 apply 前不生效；verdict 含证据指针（复用 verdict 格式，作为 P0 观测性在本层的最小试点） | 人看的不是代码而是行为证据（PNG/CSV/trace 摘要）；dry-run 有费用与时长预算并显示 |
| S3 共创回路验收 | 提案→证据→确认全链 + 分级自主（初期每提案必审→成熟后审政策不审个案：人批准预算和约束，回路在沙盒内自主，定期审阅） | 两个题材样例复用同一契约零引擎改动；驳回原因回喂后迭代不丢上下文；契约编辑全程 Undo 可回滚 | 人改数值与 AI 改行为正交、冲突面为零；单次确认 ≤3 个决定 |
| S4 类 RSI NPC 回路 | NPC 记忆+世代化+多样性门禁+世代回滚 | 重放钉 generation ID 逐位复现；改进只发生在世代切换点（局末/漂移检测/人发起，不逐帧）；多样性只作下限约束不作最大化目标（防 Goodhart）；人设参数世代间零改动（审计断言）；世代一键回滚 | 靶子样例：同一 NPC 20 局行为 n-gram 重复率显著下降且任务成功率不降；LLM 调用按预算触发并有费用记录 |

## 端到端验收

S1 用"靠近按钮→触发门开关→显示状态→保存→重启→Player"；S3 用两个题材样例（复用同一契约、不新增引擎代码）；S4 用"重复感"本身当靶子（20 局 n-gram 重复率下降 + 任务成功率不降 + 人设参数零改动）。全程录像检查确认面、错误面板与回滚入口；异常不能淹没界面。

## 依赖

scripting S1/S2（生命周期/属性事件契约/热重载语义）为前置；workflows S2 与 task.* 自 S2 起复用；runtime-ui/Sequence/导航经事件接入但不为强依赖；S4 的 LLM 调用方在 PrimalCli/MCP 侧，引擎零依赖。

## 详细设计与接口契约

> 本节是拟议实施合同，尚未实现。类名/命令名是概念命名；实施时对齐现有 Registry、ABI 与文档 schema，不表示已可调用。

| 设计项 | 本模块约定 |
|---|---|
| 数据模型 | `GameplayStateMachine/StateTransition/Trigger/Task/Predicate`（L0）；`NpcMemory/Generation`（S4）；业务身份稳定，不以原生地址/数组位置作持久引用 |
| 字段/版本 | 契约带 schemaVersion；Generation 记 proposal 来源、评估证据指针、批准人与时间；记忆条目为事件流投影（帧号+事件名+参数摘要） |
| 接口/执行 | `game.state.get/transition`、`game.task.complete/fail` 四原语为契约唯一写入口；解释器每 tick 求值谓词→发事件；提案 apply 为原子切换（热重载语义：失败保留上一可运行版本） |
| 数据/运行路径 | L0 编辑（文档模型+Undo）→场景挂载/序列化→v2 包行→Player 加载→L1 解释器+L2 脚本同帧序执行→trace/record |
| 失败/恢复 | 沿 scripting：语法错保留旧版本；运行异常定位实体/文件/行；drain 内异常不中断总线既有契约；世代回滚=generation 指针回退+热重载 |
| 修改归属 | 新 `Engine/Gameplay/` + 独立测试子目录；EngineDLL 只增导出（static_assert 锁 POD 镜像）；Player 包行加分支；编辑器组件 Serializer.cs 三处登记；UI/CLI 共用，编辑写入走文档/Undo |
| 保存/迁移 | NPC 记忆/世代持久化为边车文件或场景格式加法字段，不改既有字段语义；未知版本不静默覆写 |
| 线程/事件/取消 | 总线主线程契约不变；dry-run/改进回路为外部长任务（task.* 登记），取消有确定语义 |
| 扩展成本 | 新谓词/条件类型经注册加入封闭集；不能为样例改核心或复制另一套 UI/CLI |

## 不做项目与范围边界

| 范围 | 明确不做 | 仍需保留 |
|---|---|---|
| S1–S3 | 题材框架（战斗/背包/剧情系统）；行为树/Blackboard；多脚本语言；AI 直接改契约数值（必须经人） | 确定性回放；文档模型+Undo；ABI 只增不改 |
| S4 | 无界自我改进（改进目标仅限 NPC 行为模块，不含引擎/harness 自身）；逐帧在线学习；AI 改人设参数（人格参数是人的缰绳）；多样性最大化目标 | 世代内确定性；人的最终确认权（分级但不可取消）；世代回滚与审计 |

题材玩法语义（战斗规则、经济、剧情分支）全部留在项目 Lua 层；本模块不因样例需求扩张谓词集之外的能力。

## 对既有系统的影响与并行纪律

- 渲染/流体/刚体逻辑零影响：不进 `Engine/Graphics/`、不改 `Engine/Simulation/` 内部、不碰 Volume/Froxel；唯一交互路径是脚本已有的 entity transform 回写。流体当前冻结未启动，零交集。
- 四个加法接触面：① EngineDLL 新导出；② `PrimalPlayer/main.cpp` 包解析加 gameplay 行（单文件加分支，不碰渲染循环）；③ 编辑器新组件三处登记+Inspector 段（文档模型+Undo）；④ CLI schema 快照随提交重生成。
- 并行纪律：`Docs/ARCHIVE.md` 与 `EngineTest` CMakeLists 为渲染会话共享热点——提交用白名单 `git add` + 手术暂存（Navigation 会话先例）；测试放独立子目录。
- E2E 用独立场景，不挂 Sponza/渲染金基线；行为脚本引入的渲染可见变化须在 gameplay 场景内自证，不修改渲染基线。
