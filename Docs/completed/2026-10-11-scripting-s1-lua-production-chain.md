# Scripting S1：Lua 生产接通（scripting-properties-and-events S1 一期）

> 日期：2026-10-11；分支：dev；状态：Stage A（引擎+Player）已实施并自动化验证；Stage B（编辑器面）已实施，GUI 会话端到端与 B 门禁待用户验收。
> 上游：[总规划](../2026-10-04-agent-production-and-3d-printing-roadmap.md) 第一梯队枢纽模块；[子规划 S1](../plans/production/scripting-properties-and-events.md)（T1.1-T1.3 落地，T1.4 部分待验收）。
> 背景：引擎脚本框架（Engine/Components/Script*）此前完整但休眠——ABI 未导出（nm 0 符号）、PipelineTick 不调 frame_tick、编辑器组件是 data-only 占位。本轮把链路从测试树接进生产。

## 变更清单

| 文件 | 变更 |
|---|---|
| `Engine/Scripting/LuaBackend.{h,cpp}` | 新增：测试树 Lua 后端拷贝生产化。扩展：create_instance 带 params（并行数组 → Lua `params` 表，begin_play 前置）；每类型额外全局注册器钩子（保持后端不接触 game_entity） |
| `Engine/CMakeLists.txt` | Scripting 进 COMMON_SOURCES；LuaBackend.cpp 加 -fexceptions；lua_static 目标（缺 third_party/lua 显式 FATAL_ERROR——生产不静默跳过）；Engine PUBLIC 链接 lua_static |
| `EngineTest/.../ScriptLuaBackend/CMakeLists.txt` | lua_static 定义加 if(NOT TARGET) 守卫（先到先得，测试树不受影响） |
| `EngineDLL/ScriptAPI.cpp` | 新增（ABI 只增）：ScriptEnsureSystems（script::initialize + Lua 后端 + 沙箱默认根 ScriptsData/Save/Log/Cache，宿主 cwd 相对）、ScriptCreateForEntity（路径→类型缓存，坏脚本/缺文件 stderr 定位不崩宿主）、ScriptDestroyForEntity、ScriptSetParamsForEntity（S1=同实体销毁重建，返回新 handle）。`entity` 表：position/set_position、rotation/set_rotation（**欧拉弧度**，与 SetEntityTransform 全链同语义——该链路无度→弧度转换是存量事实，本轮勘误记录）、scale/set_scale |
| `EngineDLL/PhysicsAPI.cpp` | PipelineTick 接 `script::frame_tick`；顺序契约冻结：**script → physics → pfx → bindings**（脚本产出意图，物理/粒子消费） |
| `Tools/PrimalPlayer/main.cpp` | 每帧渲染前 PipelineTick(dt)（钳 0.1s）；v2 包新增 `script "<路径>"`/`param <k> "<v>"` 行（解析+注入；纯脚本实体也会 CreateEntity）；拆除顺序脚本先行；飞行相机单位勘误（弧度——原按度写的灵敏度实际是 8.6°/px） |
| `PrimalEditor_Avalonia/Components/LuaScriptComponent.cs` | 新增组件（"scriptLua"）：ScriptAssetGuid + Parameters KV；旧 Rotate/Patrol 占位保留不删 |
| `Serializer.cs` | 三处 switch 注册（PFX 教训：漏一处保存抛 Unknown） |
| `INativeEngineBridgeLike.cs`/`NativeEngineBridge.cs` | ScriptCreateForEntity/ScriptDestroyForEntity（接口默认实现——三个测试 Fake 桥零改动） |
| `SceneRuntimeSynchronizer.cs` | Sync/SyncEntity 脚本分支 + EnsureScript（销毁重建=SetParams 宿主语义）；Teardown/RemoveEntity 脚本先行拆除；编辑态只挂实例不 tick（EngineTick 不驱动 PipelineTick） |
| `RuntimeSceneExporter.cs` | v2 包 script/param 行（项目相对路径；含空格键拒绝） |
| `WorldEditorViewModel.cs` | DropAssetAsync 脚本分支：单选拖挂/换绑（抄 PFX author-then-apply）、空投建实体（AddEntityAction undo） |
| `Cli/ScriptCommands.cs` + CliHost + CliSchemaSnapshotTests + 快照 | `script.attach`/`script.set-param`/`script.info`（§6.3 描述+Example；parametersJson 字符串参数——Dictionary 无 schema 规则）；快照已重生成 |
| `MainWindow.axaml.cs` | New Script 的 Lua 存根升级为可运行旋转模板（params.speed，含契约注释） |
| `RuntimeSceneExporterTests.cs` | +1 测试：script/param 行 + 含空格键拒绝（6/6） |

## 验证记录（2026-10-11，Darwin/Debug Metal 树）

- **A01 生命周期（自动化）**：v2 包（mesh+tex+script+param speed=0.8）→ Player `--record-frames 3 --editor-mode 1` → 沙箱 Log 根 trace = `begin_play → [fixed_update → update → late_update]×3 → destroy`，次序与计数精确，零 Lua 错误；帧 md5 随时间变化（脚本旋转真实反映在渲染）。证据 SHA256 前 16：frame_0002 `8dcba925d510ba14`、trace `7c33624d9b4eea44`（/tmp/script_a01）。
- **A02 边界（自动化）**：坏脚本（begin_play 调 nil 函数）→ `Lua error in begin_play: <file>:3: attempt to call a nil value`，分发继续、录制完成；缺文件 → load error + attach failed、宿主不退出。
- **ctest Unit**：18/18 全过（含 TestLuaBackend——测试树自编副本与 Engine 内副本经静态库惰性拉取规则零冲突）。
- **C# 门禁**：Cli 57/57（含快照）、Synchronizer/Launcher/Exporter 合计 103/103；编辑器构建 0 错误。
- **待用户 GUI 验收（B 门禁性质，单人现状如实标注）**：编辑器拖放挂载 → Inspector/CLI 改参数 → Run → Player 观感；相机手感（弧度修正后灵敏度）。

## 实施后发现并修复的坑（复现价值高）

0. **编辑器进项目即崩（2026-10-12 修复）**：接入时假设"frame_tick 未初始化自守卫"是错的——`script::frame_tick` 入口 `check_main_thread()` 先断言 `g_initialized`；编辑器视口渲染循环每帧调 `PipelineTick`（EngineViewport3D.axaml.cs:484），无脚本组件的会话从未调 `ScriptEnsureSystems` → 进项目首帧 SIGABRT（崩溃栈：check_main_thread ← frame_tick ← PipelineTick）。修复：`PipelineTick` 的 script 段加 `is_initialized()` 守卫（PhysicsAPI.cpp）。验证：编辑器进项目 12s+ 存活无新崩溃报告；Player 挂脚本的 A01 trace 不变。教训：给既有每帧入口加新子系统调用，必须先确认其在"从未初始化"宿主里的行为。
1. **SIM ▶ Play 崩编辑器（2026-10-12 修复，构建配置根因）**：`PhysicsInitialize → JPH PhysicsSystem::Init` 堆损坏（realloc 收到 RTTI 哈希样小值）或 hang 在异常分发。最小复现：dlopen 编辑器 dylib 直调 `InitializeEngine+PhysicsInitialize`（无 .NET 参与）——**build-wt/DebugEditor 必崩、Debug 配置干净**。根因：顶层 CMake 把 Jolt 归一化按 Debug 编（无 NDEBUG → JPH_DEBUG/JPH_ENABLE_ASSERTS 开），而 `Engine/CMakeLists.txt` 的 `$<$<NOT:$<CONFIG:Debug>>:NDEBUG>` 给 DebugEditor 的 Engine TU 注入 NDEBUG → 同一份 Jolt 头两套条件编译，库与消费者内联状态错位。修复：genexpr 改 `$<$<NOT:$<CONFIG:Debug,DebugEditor>>:NDEBUG>`。验证：DebugEditor dylib PhysicsInitialize 干净返回 + 真实编辑器 `sim.play` ok:true 存活。教训：含条件编译成员/断状态的头文件库，任何自定义 build type 必须与库侧配置做宏一致性核对（顶层 395-414 行既有 mitigation 只归一化了库侧，管不了消费者的 NDEBUG）。
1. **Lua `self` 命名冲突**：实体绑定表起名 `self` 被方法语法接收者遮蔽（`function M:update` 的 self=M）——改名 `entity` 表。
2. **沙箱根前缀匹配顺序**：Data 根设 `"."` 会把 log:// 等所有 cwd 路径先吸进只读 Data 根（按 Data→Save→Log→Cache 顺序前缀比对）——Data 必须是窄目录。
3. **钩子方法语义**：Lua 钩子按 `M:update(self, dt)` 派发（':' 声明），`function M.update(dt)` 会把 self 表当 dt。
4. **存量事实勘误**：SetEntityTransform 旋转参数全链按**弧度**消费（simd_quaternion 吃 rad、编辑器推送无转换）——飞行相机灵敏度已按弧度重标。

## 明确不做 / 遗留

- 子规划 S1 不做项照旧（属性稳定元数据/事件调试跳转/热重载迁移/脚本包/API 版本迁移/依赖诊断——S2/S3）。
- 本轮遗留：Inspector 脚本参数编辑 UI 段（现用 CLI script.set-param + 拖放覆盖；MS 包装+XAML 段待补）；New Script 模板下拉（现默认存根即旋转模板）；编辑器内 runtime 预览的脚本 tick（待 mode-0 黑帧修复，见 runtime-chain 移交）；script.* 命令的编辑器会话实测。
- Player 退出 SIGABRT(134)（RHIAllocator MetalRenderPass 泄漏）为引擎侧存量，见 [runtime-chain 完工文档](2026-10-09-runtime-chain-material-injection.md) §5 移交——本轮未动。

## Lua 脚本契约（面向使用者）

- 钩子：`begin_play/fixed_update/update/late_update/destroy`，':' 声明；dt 秒。
- `entity` 表：position/set_position、rotation/set_rotation（欧拉弧度）、scale/set_scale（自身实体）。
- `params` 表：字符串值（组件 Parameters / script.set-param / 运行时包 param 行）。
- 沙箱：bus/state/file/post（见测试树 LuaBackend 文档）；文件四根 = 宿主 cwd 下 ScriptsData/ScriptsSave/ScriptsLog/ScriptsCache。
