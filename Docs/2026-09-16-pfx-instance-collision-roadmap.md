# PFX 渲染 Instance 与碰撞 排期路线图

> 日期:2026-09-16 ｜ 状态:📋 排期中 ｜ 前置:v1 已交付(见 §1)
> 范围:粒子渲染 instance(content browser 资产实例化)、粒子 vs 世界静态碰撞 v1 的后续阶段,
> 以及发射源 mesh 采样的补完项。工作量均为估算值(人日,含测试与门禁)。

---

## 1. v1 已交付基线(2026-09-16,当日完成)

| 能力 | 落点 |
|---|---|
| 发射源 mesh_surface 采样(OBJ,scatter 四模式 + 权重通道 + bias) | `Engine/ParticleFX/core/MeshGeometry.h/.cpp`、`modules/BuiltinModules.cpp` |
| ShapeLocation 参数 param_ref 化(radius/height/angle 可暴露) | 同上 |
| 渲染 instance(`instance.mesh` 声明 → 多几何槽实例化) | `core/EmitterProgram.cpp`、`render/Bridges.cpp`、`Graphics/Passes/ParticleFXMeshPass.*`、`ForwardSceneRenderer.cpp` |
| PhysicsCollision(粒子 vs 世界静态碰撞,射线探测 + bounce/friction/kill) | `modules/PhysicsCollisionModule.cpp`、`EngineDLL/PhysicsAPI.cpp` |
| 编辑器:INSTANCE 行、SIM ▶ Play/⏸ Pause、CookedArtifactResolver 提取 | `PfxEmitterEditorView*.axaml*`、`WorldEditorView*`、`Native/CookedArtifactResolver.cs` |

门禁基线:TestParticleFX 59 组、C# Pfx 30 / Cli 57 全绿;Metal 集成回归无 fault。

---

## 2. 排期总览

| 阶段 | 内容 | 估算 | 依赖 | 建议窗口 |
|---|---|---|---|---|
| **P1** | instance 链路短板补完(内置 mesh/实例旋转/Vulkan 步长/材质/内嵌预览) | ~3.5 人日 | 无 | 09-16 → 09-18(本周) |
| **P2** | 逐粒子刚体(RigidBodySpawner 接入运行时) | ~3.5 人日 | 无(不依赖 P1) | 09-21 → 09-23(下周) |
| **P3** | 碰撞质量与规模(convex 碰撞体/collision 事件/物理多线程) | ≥4 人日 | P2 | 排期待定(按 P2 验收后评估) |
| **P4** | 发射源补完(mesh_volume 发射/模块表单 asset 类型 hint) | ~3 人日 | 无(独立线) | 穿插池,随空档插入 |

> 估算按单人含门禁计;每阶段独立可交付,完成即按仓库惯例移入 `Docs/completed/` 并同步 ARCHIVE.md。

---

## 3. 阶段详单

### P1:instance 链路短板补完(~3.5 人日)

| # | 项 | 落点(钩子已存在) | 估 |
|---|---|---|---|
| 1.1 | 内置 mesh 作 instance 选项(Sphere/Box/Plane/Capsule;`MeshCPU::create_*` CPU 数据 → 同一解包注册;编辑器下拉加"内置"分组) | `ProceduralMeshAPI.cpp`、`ParticleFXMeshPass::register_geometry`、`PfxEmitterEditorViewModel.RefreshInstanceMeshOptions` | 0.5 |
| 1.2 | 实例旋转:消费 `scale_rotation.zw`(pack 侧按通道构建四元数,替换硬编码单位姿态;shader 不动) | `render/Bridges.cpp:pack_mesh_instances`(`rot` 字段注释已预留) | 0.5 |
| 1.3 | Vulkan 顶点拉取步长错位修复(`ParticleFXMesh.vert` 声明 vec4 而 CPU 上传 12B float3,既有 bug) | `Engine/Graphics/Vulkan/Shaders/PFX/ParticleFXMesh.vert` + spv 重导 | 0.5 |
| 1.4 | 材质:per-emitter tint(实例数据或 uniform 扩展;48B 布局 pad 区可用)或顶点色通道 | `ParticleFXMeshPass`(uniform `color_` 现为全局固定) | 1 |
| 1.5 | PFX 内嵌预览显示 instance 几何(快照含位置/缩放;预览画包围盒或简化 mesh 即可,不必走 GPU pass) | `Editors/Particle/ParticlePreviewControl.cs` | 1 |

验收:TestParticleFX 全绿 + 新增实例旋转/内置槽单测;Vulkan 树 `TestVulkanSponzaRenderGraph` 实跑 mesh 粒子不花屏。

### P2:逐粒子刚体(~3.5 人日)

RigidBodySpawner 已完整且 N3 单测覆盖(建/删/物理权威回写/预算 reject_retire),本阶段是接线:

| # | 项 | 落点 | 估 |
|---|---|---|---|
| 2.1 | PhysicsBindings 管理器(emitter_id→spawner;install 适配器复用;tick 事件泵) | 新 `Engine/ParticleFX/interop/PhysicsBindings.*` | 1 |
| 2.2 | 资产格式:`"instance":{"physics":{mode/collider/radius/mass/max_bodies}}` + 编译期解析 | `core/EmitterProgram.cpp` | 0.5 |
| 2.3 | EngineDLL 接线:`PipelineTick` 尾部 tick;`ParticleFXAPI` create/remove/reload 挂 set_enabled/remove(零新 ABI) | `EngineDLL/PhysicsAPI.cpp`、`ParticleFXAPI.cpp` | 0.5 |
| 2.4 | 编辑器 PHYSICS 行(模式下拉 Off/Rigid Bodies + Radius/Mass/MaxBodies) | `PfxEmitterEditorView*.axaml*` | 0.5 |
| 2.5 | 预算对接:`host/BudgetManager` 与 spawner max_live_bodies 统一口径 + 单测 | `host/BudgetManager.*`、N3 风格测试扩展 | 1 |

验收:场景 box collider + PFX( physics=rigid_body)→ SIM Play → 粒子出生即刚体、位置由物理回写、超预算走 retire;确定性哈希门禁(`PhysicsDeterminismHash`)不回归。

### P3:碰撞质量与规模(≥4 人日,依赖 P2)

- **convex hull 逐粒子碰撞体**:`body_create_convex` API 已在;需网格 LOD 简化(凸包顶点上限),否则 spawn 尖峰撑爆预算。估 2。
- **collision 事件管道**:`PfxTypes.h` 的 `event_type::collision` 已定义、无产生者/消费者;接通后事件驱动效果(撞击火花)。估 1.5。
- **物理多线程 JobSystem 适配**:物理侧 M2 本身未接,属独立大项,建议单独立项评估。估 1+。

### P4:发射源补完(~3 人日,独立线,穿插池)

- **mesh_volume 发射**:枚举已占位、编译期显式拒绝;需内部点云或单 mesh SDF 烘焙(`interop/CacheConverters` 有转换器骨架)。估 2。
- **模块表单 asset 类型 hint**:ShapeLocation 的 mesh 参数现为文本行,升级为真正的资产选择器(与 INSTANCE 行同款 ComboBox + 解析)。估 1。

---

## 4. 边界与风险

- 几何槽上限 8、max_instances 4096 为 v1 常量;P1.4 或 P3 若触及上限,再做槽淘汰策略(按 LRU 注销,需同步 `pfx_instance_geometry_slots_` 缓存)。
- P2 的物理权威回写(ADR3)与渲染读 position 通道天然兼容,但与 PhysicsCollision 模块同开时语义重叠(刚体自带碰撞)——资产校验应拒绝二者同开,或文档明确优先级。
- `TestVulkanSponza` 属 Vulkan 树,交叉构建纪律照旧(切树先 `rm Darwin/Debug/libEngine.a`)。
