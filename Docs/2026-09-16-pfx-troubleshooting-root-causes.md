# PFX/编辑器排障根因速查（2026-09-16）

状态：📖 长期参考（随代码演进回填）
关联：`2026-09-12-particle-framework-architecture.md`、`2026-09-16-pfx-instance-collision-roadmap.md`、`2026-09-14-pfx-editor-ui-panels-design.md`

> 本文档记录 PFX/粒子编辑链路中**已踩过且容易复发**的问题根因。
> 现象 → 根因 → 现行规则/修复位置，供后续排查直接对照。

---

## 1. 打开/编辑 PFX 编辑器后，场景粒子行为突变（life 缩水、悬空、换了一套行为）

**根因**（两级，均已修）：

1. `PushHotPreview` 曾把**窗口内存 JSON** `PfxEmitterReloadFromMemory` 推进场景活发射器——窗口状态与磁盘稍有出入（外部改过文件、窗口开着时别处更新），场景立即跑起另一套程序。
2. `ResolvePreviewEmitterId` 曾**优先复用场景活发射器当预览目标**，导致即便只推"预览"，目标其实还是场景发射器。

**现行规则**（`PfxEmitterEditorViewModel.PushHotPreview` / `ResolvePreviewEmitterId`）：

- 预览只推**私有 editor_preview 发射器**（`PfxSetEditorPreview(preview:true)`，不进世界渲染）。
- 场景发射器**只经** `Save → 落盘 → AssetSavedOnDiskCallback → SceneRuntimeSynchronizer.ResyncPfxEmitter / ReloadPfxEmittersForAsset`（销毁旧 → 按磁盘重建 → 恢复位置/暴露参数）。
- ⚠️ 不要以"所见即所得"为由恢复对活发射器的直推；要实时看效果，看编辑器内预览窗格。

## 2. 同 guid 的 scene.load 原地换数据，不重建原生实体（实体泄漏）

- `SceneCommands.Load` 命中已打开 guid 时只 `existing.Entities.Clear()+Add`，**不走原生 teardown**。
- 后果：反复 `scene.load` 同一场景，原生侧实体/发射器持续累积（状态栏 entity 数虚高），旧发射器带旧程序继续跑。
- 需要"按当前磁盘内容强制重建"时：**切到另一场景再切回**（跨 guid 走全量 teardown+Sync），或对单实体调 `SceneRuntimeSynchronizer.ResyncPfxEmitter`。

## 3. PhysicsCollision 三个"半径"的对齐

- **radius 语义**：粒子缩放 = 1 时的半径；有效半径 = radius × 粒子当前缩放（SetScale/ScaleOverLife 自动跟随）。引擎实现：`PhysicsCollisionModule.cpp`（读 `attr::scale_rotation`）。
- **内置几何基准 0.05m**：builtin:cube（half 0.05）/ builtin:sphere（半径 0.05）。自定义 mesh 用真实包围半径，`PfxInstanceMeshRadius(path)`（EngineDLL）可查；PFX 编辑器 INSTANCE 下方有显示 + "radius→collision" 一键填入。
- **症状对照**：radius 偏大 → 粒子悬空"够不着地面"（位置回退到 hit+normal×radius）；偏小 → 半截入土。
- **前置条件**：物理必须已初始化（SIM ▶ Play），未初始化时射线静默无操作（设计行为）。视口已有黄条提示（`WorldEditorView.UpdateSimInactiveHint`）。

## 4. 新增编辑器组件必须登记序列化器三处

`GameProject/Serializer.cs`：序列化 switch、`GetComponentTypeDiscriminator`、`DeserializeComponent` 判别名 switch。漏一处 → **整个场景保存失败**（`Unknown component type` / discriminator），自动保存同样失败。判别名与 `EditorComponentRegistry` 键一致（collider/rigidbody/particleFX）。scene.save 报 Unknown component type 时先查这里。

## 5. scene.load 的 path 相对 Content 根解析

`scene.load {"path":"Scenes/xx.scene"}` 相对 `AssetRegistry.ProjectContentRoot ?? project.Path` 解析——项目场景在 `<project>/Scenes/`，**不在 Content 下**，相对路径会 NOT_FOUND。用绝对路径最稳。

## 6. 环境层（agent 自动化记忆）

- **显示器休眠（displaysleep 5 分钟）连锁故障**：CUA 显示枚举/截图失效（list_displays 空）、WindowServer 前台态卡死（无任何前台应用）、Avalonia AX 桥对新启动进程不可用（`[cg-only-no-ax-window]`）。`caffeinate -u -t 3` 唤醒屏幕，但 CUA/AX 连接可能已永久失效 → 涉及 GUI 自动化的长会话先 `caffeinate -display`。
- **Avalonia 编辑器无 bundle_id**：zcode CUA 的 app_ref/元素点击绑定常失败；用 open-computer-use 插件 `sky_click` + 应用窗口截图坐标。
- **GUI 全挂时的兜底**：编辑器进程内 CLI `./scripts/primal.sh <命令>`（socket 不走 AX/WindowServer，AX 失效时依然可用），与 UI 同一文档模型。常用：`scene.tree / scene.new / scene.load(绝对路径) / entity.create / sim.body.add / sim.play / scene.save / editor.screenshot`。
- **sim.body.add 后物理体**：`sim.play`（= ▶ Play：PhysicsInitialize→MaterializeAll→SetRunning）时统一物化；之前创建的碰撞体也此时生效。

## 7. 已知未修（观察项）

- `event_bus` capacity 4096：rate ≥ 40 时 `dropped_total` 持续增长（仅影响事件消费者如刚体化，不影响视觉模拟）。待扩容/自适应。
- 原生实体计数（状态栏）在反复同 guid scene.load 后虚高（见 §2），重启编辑器清零。
- 编辑器窗口**内存态晚于磁盘时点 Save 会覆盖外部修改**（Save 即写盘，无冲突检测）——外部改过 .pfx_emitter 后先 Discard/重开编辑器窗口。
