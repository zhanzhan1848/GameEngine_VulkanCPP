# GI 剩余工作路线图（新 Session 指南）

**前次 Session**: 2026-09-08，13 commits 在 dev 本地（未 push），M0–M3 + M5 部分 ~40 项关闭。
**本文件**: 新 session 按序完成剩余全部项的操作手册。

---

## 第一步：恢复上下文

```bash
git log --oneline -15   # 确认 13+ commits 存在（最新 b04ada93 T3.5 前置）
cat tasks/gi-supplement-todo.md   # 读销账台账（注意 ◐ = 部分完成）
cat Docs/environment-variables.md # 读 env 旋钮清单
```

门禁基线（所有 band 已按 M2 标度校准后重标定）：
```
Metal GIFusion: probe=9.99 ∈ [0.08,13], gather_avg=5.57 ∈ [0.005,6.3], final_avg=0.38 ∈ [0.005,0.8]
Vulkan GIVerify: probe=4.56 ∈ [0.08,6], gather_avg=5.55 ∈ [0.005,6.3]
Vulkan GIGolden: worstRel < 0.05% (EngineTest/assets/golden/gi_probe_sh_vulkan.csv)
```

已知坑：
- **两棵构建树共用 `Darwin/Debug/libEngine.a`**——切换树前必须 `rm -f Darwin/Debug/libEngine.a`
- **增量构建可能混链**——大批量头文件改动后用 `--clean-first`
- **Transform.cpp consume-on-read 已生效**——实体创建 flag 读后即清，不会每帧触发
- **WindowServer watchdog**：GPU 过载会崩桌面，控制同帧 dispatch 总量

---

## 第一批：T3.5 Radiance Cascade 完整实施

**前置已入库**（b04ada93）：
- `Vulkan/Shaders/Lumen/RadianceCascadeUpdate.comp` + `.spv` ✓
- `ScreenProbeGIPass.h` cascade 成员声明 ✓

### 步骤 1.1 — Metal kernel

创建 `Engine/Graphics/Metal/shaders/Lumen/RadianceCascadeUpdate.metal`：
- 入口 `radianceCascadeUpdate`，参数 `(uint3 tid, texture3d<float,write> out, texture3d<float,read> prev, constant CascadeParams& p, texture3d<float,read> ddgiIrr)`
- CascadeParams struct：origin_spacing(float4) + counts_enabled(float4) + ddgi_origin_spacing(float4) + ddgi_counts(float4) + temporal_weight(float) + frame_index(float) + pad(float2) = 96B
- 逻辑与 `.comp` 完全一致（DDGI trilinear L0 注入 + temporal EMA）
- 注意：Metal `texture3d<float, access::read>` 的 `read(uint3)` 不做越界 clamp，需手动 `min(coord, size-1)`

### 步骤 1.2 — CPU 创建

在 `ScreenProbeGIPass::Initialize` 末尾调 `CreateRadianceCascade()`：

创建内容（`ScreenProbeGIPass.cpp`）：
- 2 × 32³ RGBA16F 3D 纹理（`radiance_cascade_[0/1]`）
- Cascade update compute pipeline（Vulkan 读 `.spv` / Metal 读 `.metal` 源码运行时编译）
- Descriptor set layout（4 bindings：out image / prev sampled / params UBO / ddgi irradiance sampled）
- 3 × descriptor sets（triple-buffer）
- 1 × params constant buffer (96B)
- Cascade extent/origin 从 DDGI volume 推导（origin − extent×0.25, extent = probe_grid×2）

Gotcha：`utils::ShaderRegistry::ResolveShaderPath` 处理 CWD 无关路径。Metal 端 entry 名 `radianceCascadeUpdate`（Metal 命名约定），Vulkan 端 entry `main`。

### 步骤 1.3 — Dispatch 接线

在 `ScreenProbeGIPass::AddPass` 的 execute lambda 中（DDGI update 之后）调 `DispatchCascadeUpdate(cmd, frameIdx, cascadeIdx)`。

每帧 dispatch `(32/4)³ = 8³ = 512` 组。双缓冲：读 `radiance_cascade_[1-writeIdx]` 作 EMA 历史，写 `radiance_cascade_[writeIdx]`。frame≥2 后 temporal_weight=0.9 生效。

### 步骤 1.4 — far-cache 消费者

`ScreenProbeTraceFinalize.comp` + `DDGITraceRays.metal` finalize 入口：

hit.w==3（far-cache）分支改为：
```
旧：4-probe tetrahedral DDGI query (16 reads)
新：texelFetch(radiance_cascade, ivec3(stopPos / cascadeSpacing), 0).rgb  (1 read)
```

需要绑定 cascade 纹理到 finalize descriptor set（Vulkan flat 13/14/15 或 Metal texture(7)）。

**验收**：far-cache 分支从 16 reads → 1 read；探针网格外不再落天空渐变；GI_ISOLATE A/B dump 对比。

---

## 第二批：T3.6/D1 Flat-buffer 多 volume（atlas + offset）

**方案**：仿 UE Lumen——所有 volume 打包进一张大 SSBO + per-volume 偏移表。不依赖 descriptor indexing。

### 步骤 2.1 — CPU 侧 flat buffer

`GIGatherModule.cpp`：
- 新 buffer：所有 volume 的 irradiance 探针数据连续打包 + 头部 offset 表
- Layout：`[header(16B): totalProbes, volumeCount] [vol0: probes × 9×vec4] [vol1: ...] ...`
- 替代现有 per-volume SSBO 数组（binding 17 array[4]）
- Metal 分支同样写入 → 消除 `nvolume_supported_` 差异

### 步骤 2.2 — Shader 消费

`DDGIGIGather.comp` + `.metal`：
- 删除 binding 17（nvolIrradiance[4] array）+ binding 18（NVolumeCB）
- 新 binding：单个 SSBO（flat buffer）+ params UBO（含 per-volume origin/spacing/counts/probeOffset）
- Trilinear 查找：先确定 worldPos 在哪个 volume → 用 volume 的 probeOffset 偏移索引
- 删除 legacy 双槽路径（binding 7/8 + 15/16）

### 步骤 2.3 — Metal 远场

D2 已实现的 far 辐射三线性（Metal texture(10)/CB(11)）→ 改为读 flat buffer
（far volume 就是 flat buffer 中的一个 volume）

**验收**：Metal/Vulkan 日志都显示 `path = flat-buffer (volumes=N)`；N-volume 遮挡/法线加权一致。

---

## 第三批：T3.2 Cone 消费者

**架构结论**（前次 session 已定）：min-mip 是保守下界（≤真距离），正确用途是 **cone 命中检测**（宽 footprint 内是否有表面），不是步进加速。

### 实施

消费者是 **T3.3 Radiosity 的 indirect trace**（`SurfaceCacheIndirectTrace.comp`）——其 cosine 采样射线 footprint 随距离增长：
1. 给 indirect trace 绑定 cascade mip 纹理（复用 T3.2 DispatchMipChain 产出）
2. 射线 march 时：`footprint = t × rayAngle` → `mip = coneMipFrac(footprint, voxelSize)` → 采样 `sampleSDFCascadeMip`
3. 命中判定：`mipD < footprint × 0.5` → 该方向有表面遮挡 → 不累积天空 radiance

**验收**：薄墙/地牢漏光用例通过（GI_ISOLATE 对比 dump）。

---

## 第四批：T4.2–T4.4 内容链

### T4.1 补完：事件总线
- `SceneExtractionSystem` 集成进 StandardRenderPipeline（参考 ForwardRenderer.cpp:2245 模式）
- dirty transform → `PartialUpdate`（不是 Rebind——v3 教训：Rebind 太贵）+ revox + probe boost
- 帧预算控制：30 帧冷却 + consume-on-read

### T4.2 离线 card bake
- ContentTools 新增：体素化 mesh → 6 向卡片 bounds + Surfel 生成
- 输出：per-mesh card AABB + resolution → 运行时 CardGenerator 加载
- 运行时 AABB 派生降级为 fallback

### T4.3 per-mesh SDF
- `RHIGpuMesh.cpp:190-256`：已上传的 sdf/voxel/vector_field 三张纹理激活
- 命中点精化：DDGI trace 命中后用 per-mesh SDF 精化位置
- 梯度法线重建：中心差分 6 采样

### T4.4 emissive 闭环
- GBuffer emissive 输出 → SC capture 采样 → LightEval emissive 项 → 探针反弹
- 前置：材质系统加 emissive 字段（VTMaterialGPUParams 或独立 buffer）
- Radiance Intensity Clamp（UE 0x32）： emissive × clampedIntensity 防爆炸

---

## 第五批：M5 收尾

### T5.3 CLI 命令面
- `PrimalEditor_Avalonia/Cli/` 新增 `GiCommands.cs`：gi.status/gi.set/gi.preset/gi.probe.dump
- 投影 EngineDLL 既有导出（RenderPipelineAPI.cpp:414-1158）
- schema 快照重生成（PrimalEditor_Avalonia.Tests 的 Cli schema 门禁）

### T5.5 剩余
- 4 死管线核实（indirect_trace/resolve 已接活，其余 2 个？）
- 死旋钮接线或删除（F4 清单在 Docs/environment-variables.md §6）

### T5.8 终验
- 对照 `2026-09-07-lumen-source-analysis-notes.md` 特性面自查表
- GI_ISOLATE 全消融 dump（all/noSsgi/noDdgi/noSpgi 四组对比）

---

## 关键风险与缓解

| 风险 | 缓解 |
|---|---|
| 生命周期改动 → GPU hang（T4.1 教训） | 新 pass 全部 env 门控 + 新 session 首批跑基线门禁 |
| 增量构建混链 | 头文件批量改动后 `--clean-first` |
| Transform flags 永不清零 | consume-on-read 已修（保留） |
| WindowServer watchdog | 控制同帧 dispatch 总量；GPU 过载前减负载 |
| 两棵树 libEngine.a 混链 | 切树前 rm；大批量改动后 --clean-first |
