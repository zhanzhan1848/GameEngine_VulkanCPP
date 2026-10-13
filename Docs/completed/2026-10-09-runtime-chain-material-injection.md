# 运行时链路材质注入 + 相机桥（Run 链路补齐一期）

> 日期：2026-10-09；分支：dev；状态：已实施并通过声明范围验证（引擎侧两项遗留单独移交，见 §5）。
> 上游规划：[总规划](../2026-10-04-agent-production-and-3d-printing-roadmap.md) release-maintenance S1 最小切片；
> 执行 [2026-09-03 规划 §5.5](2026-09-03-editor-content-creation-and-four-features-plan.md) 留痕②（"材质注入留给 Phase 3"）与留痕①（Esc 暂缺），
> 闭合 [2026-09-05 规划](2026-09-05-sequence-video-recording-plan.md) 存量问题③的导出侧根因。
> 约束：零 Engine/ 与 EngineDLL/ 改动（全部走现成 ABI）；与并行 Lumen 专项零文件重叠。

## 1. 变更清单

| 文件 | 变更 |
|---|---|
| `PrimalEditor_Avalonia/Content/RuntimeSceneExporter.cs` | 格式 v1→v2：mesh 行改写 `CookedArtifactResolver` 解析后的 cooked `.mesh`（项目相对路径优先，越界回退绝对+警告）；新增 `tex <slot> "<path>"` 槽位行（`MaterialGuids[0]` 经 `MaterialWorldTextureResolver.Resolve` 解析的 [albedo,normal,orm] 三元组，与编辑器视口绑定同一解析器/事实源） |
| `PrimalEditor_Avalonia.Tests/Content/RuntimeSceneExporterTests.cs` | 新增 5 项契约测试：v2 头、cooked 相对路径、无 cooked 产物省行、tex 槽位行/ORM 空洞、越界绝对回退 |
| `Tools/PrimalPlayer/main.cpp` | ①补调 `InitializeAssetSystem()`（启动）与 `ShutdownAssetSystem()`（退出，趁 device 存活释放 registries）；②mesh 导入结果判定改为只认 `0xFFFFFFFF` 哨兵（content id 0 是合法首槽，原 `==0` 判据误杀）；③`PipelineRegisterMeshEntity` 返回值同修正（ECS slot 0 合法）；④接受 v1/v2 头与 `tex` 键；相对路径按包位置三级上溯解析项目根；⑤纹理槽位经 `PipelineImportCookedTexture` 绑定（路径为键缓存去重，失败槽留哨兵走白材质回退）；⑥`--editor-mode` 旗标；⑦飞行相机（WASD/QE/拖拽/方向键/滚轮/Shift，pitch 钳 ±89°，从选中相机 transform 播种） |
| `Tools/PrimalPlayer/player_window.h/.mm` | `PlayerInputView : NSView` 输入视图（`hitTest:` 截获，绕开引擎 MTKView 子视图抢命中；不用 `addLocalMonitorForEventsMatchingMask` 旧坑）；Esc 在 keyDown 走与关闭按钮同一退出路径（**关闭留痕①**）；`PlayerWindowPollInput` 快照 C 接口 |
| `PrimalEditor_Avalonia/Common/RuntimeLauncher.cs` | 临时追加 `--editor-mode 1`（见 §5 引擎侧移交；修复后删除该段恢复 Player 默认 runtime 路径） |

## 2. v2 格式契约（编辑器为单一事实源）

```
# primal-runtime-scene v2          （v1 仍被 player 接受；未知键双向忽略）
entity
  mesh "<cooked .mesh 路径>"        ← v1 写源/sidecar 绝对路径，引擎 ImportMeshAsync 拒绝 .model.json（实测）
  tex <slot> "<.texture 路径>"      ← slot 0=albedo 1=normal 2=orm；空洞省行；未来槽位旧 player 安全忽略
  ...
end
```

路径规则：项目相对（`/` 分隔）优先，player 按 `<proj>/.Primal/runtime/<pkg>` 三级上溯反推项目根解析；绝对路径直用（越出项目根回退）。加法式扩展——这是后续组件（脚本引用等）继续扩展的同一条路。

## 3. 验证记录（2026-10-09，Darwin/Debug Metal 树）

- **Player 侧三个堆叠 bug 修复实测**：修复前 `ImportMeshAsync rejected`（JobSystem 未启）→ 修复后 `mesh import failed(status=1)`（mesh_id==0 误杀）→ 再修后 `PipelineRegisterMeshEntity failed`（entity 0 误杀）→ 最终网格注册成功。逐层证据在 /tmp 各轮 stderr（已清理，结论留档）。
- **端到端纹理**：v2 包（corridor-intersection.mesh + tex0=Lion_Albedo.texture，editor-path mode 0/3）→ `bound world texture (content=2147483648)`；PNG 5786B(白模)→70349B(贴图)，meanLuma 0.081→0.044；视觉确认狮纹贴图映射到走廊表面。
- **相机播种差分**：包内相机 yaw=0 vs 45° → 帧 md5/大小完全不同（7a629f…/124835B vs 4dd1c2…/58831B），transform 回写→视图链路成立。交互手感（WASD/拖拽方向语义）待用户 GUI 实测——B 门禁性质，单人现状如实标待验收。
- **成片**：720p×240 帧（mode 3 + editor-path，静态机位）→ ffmpeg（VideoEncoder 同参数 libx264/crf20/yuv420p）→ `/tmp/runtime_final/runtime_chain_textured.mp4`（38KB/8s）。证据 SHA256（前 16 位）：mp4 `0eeacb33e6fc348a`、frame_0000 `8ff6be893fece78e`、metrics `caa651449349e9c0`、贴图帧 `ca7bfd371288cb1e`、白模对照帧 `1a2830ce54e4a69c`。
- **门禁**：`dotnet test --filter Cli` 57/57；`--filter RuntimeLauncher` 17/17（`--editor-mode` 追加未破坏参数拼装断言）；新增导出器测试 5/5。无 CLI 命令面变化，schema 快照不动。未触碰 Engine/，ctest Unit 不适用。
- **磁盘纪律**：本轮 /tmp 诊断目录（baseline/spike×4/mode0/inview/edmode1/camtest×2 等）验证归档 SHA 后已清理；TestProject_01/.Primal/runtime 的临时测试包已删（active.primalrun 保留）；证据保留 /tmp/runtime_final（51MB，volatile）+ /tmp/runtime_v2tex2、/tmp/runtime_edmode1 各一帧。

## 4. 明确不做（本增量边界）

PipelineTick/脚本宿主（等 Lumen 专项收尾后做，EngineDLL 需加导出）；PrimalBuild cooked staging/可搬移安装包（Phase 3 完整形态，本增量是其直接注入子集）；MaterialGuids[1..] 多材质槽与 MaterialOverride per-submesh（编辑器世界渲染本身也只消费首槽）；BuiltinMesh 导出；run.record CLI、独立离线渲染进程（09-05 二期项）。

## 5. 引擎侧移交（渲染专项，非本增量范围）

1. **`PipelineSetEditorMode(0)`（runtime 全延迟路径）在 Metal 上输出全 NaN/黑**。
   证据：修复 mesh 导入后，几何在视锥内+双灯+天空开启，mode 0 与 mode 3 均 meanLuma=nan、PNG 973B、窗口实测纯黑（截图确认）；同 EngineDLL 下 primal-engine render verb（mode 1，程序化场景）出正常图，player `--editor-mode 1` 同场景同包出正常图。此前所有"Player 录制验证"（2026-09-05）实际都是空场景（mesh 被拒），故该问题一直被存量问题③掩盖。
   嫌疑范围：StandardRenderPipeline editor-mode 分支差异（Metal 端无任何台架覆盖 mode 0——编辑器视口、primal-engine、09-05 录制全是 mode 1；Vulkan 端 mode 0 由 TestVulkanSponzaRenderGraph 天天在跑）。
   复现：`Darwin/Debug/PrimalPlayer --scene <v2包> --mode 0 --editor-mode 0 --record …`（NaN）vs `--editor-mode 1`（正常）。
   现状处置：RuntimeLauncher 临时传 `--editor-mode 1`；**引擎修复后删除 RuntimeLauncher 中标注 2026-10-09 的临时段**即可恢复 runtime 路径默认。
2. **`RHIAllocator` MetalRenderPass 泄漏断言 → 进程退出 SIGABRT(134)**。`[RHIAllocator] LEAK: N active allocations (T=MetalRenderPass, size=432)` 后 `Assertion failed: _stats.activeAllocations == 0`（RHIAllocator.h:102）。player（1 个泄漏）与 primal-engine render（4 个）今天同源复现，与网格注册相关（空场景不触发）。这是 09-05 存量问题②（"退出后 mutex abort"）的实际签名。AssetAPI 头注释预期 ShutdownAssetSystem 应在 device 存活时释放——已照做但未消除，泄漏在 RHI 层。
3. TEXBIND 刷屏日志为渲染专项的调试追踪（MetalCommandBuffer.cpp:1063 计数器封顶 4000），非错误，无需处理。

## 6. 遗留与后续

- 相机交互手感（LH/RH 方向语义、灵敏度）待用户 GUI 会话实测；如方向反了翻转 strafe/look 符号即可，契约不变。
- 编辑器 Run 按钮全链（导出 v2→启动→录制→BuildLog 通知）在 GUI 会话端到端走一遍（本轮 CLI 侧已分段验证：导出器单测+player 手工包）。
- 视频编码入口仍是 CLI（editor.record/sequence.render）或手拼 ffmpeg；Run 完成后自动编码属 09-05 二期（run.record），未在本期。
